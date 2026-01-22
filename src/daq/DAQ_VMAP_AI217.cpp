#include "daq/DAQ_VMAP_AI217.hpp"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <arpa/inet.h> // ntohl
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <sched.h>

namespace uei
{

  static std::atomic<bool> g_stop(false);

  static void LogInfo(const std::string &msg) { fprintf(stdout, "[INFO] %s\n", msg.c_str()); }
  static void LogWarn(const std::string &msg) { fprintf(stderr, "[WARN] %s\n", msg.c_str()); }
  static void LogErr(const std::string &msg) { fprintf(stderr, "[ERROR] %s\n", msg.c_str()); }

  void DAQ_VMAP_AI217::SigIntHandler(int) { g_stop.store(true); }

  void DAQ_VMAP_AI217::InstallSigIntHandler()
  {
    std::signal(SIGINT, &DAQ_VMAP_AI217::SigIntHandler);
  }

  void DAQ_VMAP_AI217::TimespecAddNs(struct timespec *t, long long ns)
  {
    const long long NSECS_PER_SEC = 1000000000LL;
    long long n = t->tv_nsec + ns;
    t->tv_sec += static_cast<time_t>(n / NSECS_PER_SEC);
    t->tv_nsec = static_cast<long>(n % NSECS_PER_SEC);
    if (t->tv_nsec < 0)
    {
      t->tv_nsec += static_cast<long>(NSECS_PER_SEC);
      t->tv_sec -= 1;
    }
  }

  static void ApplyRtScheduling(bool enable_rt, int prio)
  {
    if (!enable_rt)
      return;

    struct sched_param sp;
    std::memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;

    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
      LogWarn("sched_setscheduler(SCHED_FIFO) failed; continuing without RT priority.");
    else
      LogInfo("SCHED_FIFO enabled, priority=" + std::to_string(prio));
  }

  static int Ai217GainToMacro(int gain)
  {
    if (gain == 1)
      return DQ_AI217_GAIN_1;
    throw std::runtime_error("AI-217: unsupported gain (MVP supports gain=1 only)");
  }

  static int InputModeToMacro(const std::string &mode)
  {
    if (mode == "diff")
      return DQ_LNCL_DIFF;
    throw std::runtime_error("AI-217: unsupported input_mode (MVP supports \"diff\" only)");
  }

  DAQ_VMAP_AI217::DAQ_VMAP_AI217(const PDNA_PARAMS &p) : params(p)
  {
    // Same as SampleVMap217.c
    vmap_flag = DQ_VMAP_FIFO_STATUS | DQ_VMAP_FIFO_CLR_ON_OVF;
  }

  void DAQ_VMAP_AI217::Open()
  {
    InstallSigIntHandler();
    ApplyRtScheduling(params.enable_rt, params.rt_priority);

    if (params.channels.empty())
      throw std::runtime_error("AI-217: channels empty");
    if (params.frequency <= 0.0)
      throw std::runtime_error("AI-217: frequency must be > 0");
    if (params.numSamplesPerChannel <= 0)
      throw std::runtime_error("AI-217: numSamplesPerChannel must be > 0");

    // Align Sample naming/meaning
    params.numChannels = static_cast<int>(params.channels.size());

    // UEI init/open
    DqInitDAQLib();

    char iom_ip[64];
    std::memset(iom_ip, 0, sizeof(iom_ip));
    std::snprintf(iom_ip, sizeof(iom_ip) - 1, "%s", params.iom_ip.c_str());

    int ret = DqOpenIOM(iom_ip, DQ_UDP_DAQ_PORT, params.open_timeout_ms, &hd, &rd_cfg);
    if (ret < 0)
    {
      DqCleanUpDAQLib();
      throw std::runtime_error("DqOpenIOM failed ret=" + std::to_string(ret));
    }

    // Create VMap (Sample uses frequency)
    ret = DqRtVmapInit(hd, &vmapid, params.frequency);
    if (ret < 0)
    {
      Close();
      throw std::runtime_error("DqRtVmapInit failed ret=" + std::to_string(ret));
    }

    // Configure channels (like Sample: apply gain/mode bits)
    const int gain_macro = Ai217GainToMacro(params.gain);
    const int mode_macro = InputModeToMacro(params.input_mode);

    std::vector<int> channels = params.channels;
    for (size_t i = 0; i < channels.size(); ++i)
      channels[i] |= DQ_LNCL_GAIN(gain_macro) | mode_macro;

    // Add channels to VMAP
    ret = DqRtVmapAddChannel(hd, vmapid, params.device, DQ_SS0IN, channels.data(), &vmap_flag, 1);
    if (ret < 0)
    {
      Close();
      throw std::runtime_error("DqRtVmapAddChannel failed ret=" + std::to_string(ret));
    }

    // Set scan rate
    ret = DqRtVmapSetScanRate(hd, vmapid, params.device, DQ_SS0IN, params.frequency);
    if (ret < 0)
    {
      Close();
      throw std::runtime_error("DqRtVmapSetScanRate failed ret=" + std::to_string(ret));
    }

    // Program channel list
    ret = DqRtVmapSetChannelList(hd, vmapid, params.device, DQ_SS0IN, channels.data(), static_cast<int>(channels.size()));
    if (ret < 0)
    {
      Close();
      throw std::runtime_error("DqRtVmapSetChannelList failed ret=" + std::to_string(ret));
    }

    // Allocate bdata storage (Sample's bdata)
    bdata_storage.resize(static_cast<size_t>(params.numChannels * params.numSamplesPerChannel));

    // ---- pacing: FIFO half-full, read length, and UI FPS ----
    const double fifo_per_ch = 2048.0;
    const double ui_fps = 20.0;
    const double fs = params.frequency;

    const double half_fifo_time = (fs > 0.0) ? ((fifo_per_ch * 0.5) / fs) : 0.0;
    const double read_time = (fs > 0.0) ? (static_cast<double>(params.numSamplesPerChannel) / fs) : 0.0;
    const double ui_time = (ui_fps > 0.0) ? (1.0 / ui_fps) : 0.0;

    double period_s = 0.0;
    if (half_fifo_time > 0.0)
      period_s = half_fifo_time;
    if (read_time > 0.0)
      period_s = (period_s > 0.0) ? std::min(period_s, read_time) : read_time;
    if (ui_time > 0.0)
      period_s = (period_s > 0.0) ? std::min(period_s, ui_time) : ui_time;
    if (period_s <= 0.0)
      period_s = 0.001; // fallback 1ms

    periodns = static_cast<long long>(std::floor(1000000000.0 * period_s));
    clock_gettime(CLOCK_MONOTONIC, &next);

    LogInfo("AI217 Open OK: device=" + std::to_string(params.device) +
            " frequency=" + std::to_string(params.frequency) +
            " numChannels=" + std::to_string(params.numChannels) +
            " numSamplesPerChannel=" + std::to_string(params.numSamplesPerChannel) +
            " period_ms=" + std::to_string(periodns / 1000000LL));
    LogInfo("AI217 pacing: fifo_ms=" + std::to_string(half_fifo_time * 1000.0) +
            " read_ms=" + std::to_string(read_time * 1000.0) +
            " ui_ms=" + std::to_string(ui_time * 1000.0) +
            " period_ms=" + std::to_string(periodns / 1000000.0));
  }

  void DAQ_VMAP_AI217::Start()
  {
    int ret = DqRtVmapStart(hd, vmapid);
    if (ret < 0)
      throw std::runtime_error("DqRtVmapStart failed ret=" + std::to_string(ret));

    ret = DqCmdSwTrigger(hd, 1 << params.device);
    if (ret < 0)
      throw std::runtime_error("DqCmdSwTrigger failed ret=" + std::to_string(ret));

    LogInfo("AI217 acquisition started.");
  }

  void DAQ_VMAP_AI217::Stop()
  {
    stop_.store(true);
    g_stop.store(true);
    if (hd && vmapid)
      DqRtVmapStop(hd, vmapid);
  }

  void DAQ_VMAP_AI217::Close()
  {
    if (hd && vmapid)
    {
      DqRtVmapClose(hd, vmapid);
      vmapid = 0;
    }
    if (hd)
    {
      DqCloseIOM(hd);
      hd = 0;
    }
    DqCleanUpDAQLib();
  }

  bool DAQ_VMAP_AI217::ReadFrame(RawFrame &out)
  {
    if (stop_.load() || g_stop.load())
      return false;

    // Sample-aligned locals
    const int req_bytes = params.numSamplesPerChannel * params.numChannels * static_cast<int>(sizeof(uint32));

    int act_size = 0;
    int data_size = 0;
    int avl_size = 0;

    uint32 *bdata = bdata_storage.data();

    int ret = 0;

    // ---- Rq -> Refresh -> Get (same order as Sample) ----
    ret = DqRtVmapRqInputDataSz(hd, vmapid, 0, req_bytes, &act_size, NULL);
    if (ret < 0)
    {
      LogErr("DqRtVmapRqInputDataSz failed ret=" + std::to_string(ret));
      goto sleep_and_return;
    }

    ret = DqRtVmapRefresh(hd, vmapid, 0);
    if (ret < 0)
    {
      LogErr("DqRtVmapRefresh failed ret=" + std::to_string(ret));
      goto sleep_and_return;
    }

    ret = DqRtVmapGetInputData(hd, vmapid, 0, req_bytes, &data_size, &avl_size, reinterpret_cast<uint8 *>(bdata));
    if (ret < 0)
    {
      LogErr("DqRtVmapGetInputData failed ret=" + std::to_string(ret));
      goto sleep_and_return;
    }

    if (data_size == 0 || avl_size == 65535)
    {
      printf("[DBG] GetInputData: req_bytes=%d act_size=%d data_size=%d avl_size=%d\n",
             req_bytes, act_size, data_size, avl_size);
    }
    if (data_size > 0)
    {
      const int data_words = data_size / static_cast<int>(sizeof(uint32));
      const int numScansReceived = (params.numChannels > 0) ? (data_words / params.numChannels) : 0;

      if (numScansReceived > 0)
      {
        out.seq = seq_++;
        out.slot_index = params.slot_index;
        out.group_name = params.group_name;
        out.samples_per_channel = numScansReceived;
        out.num_channels = params.numChannels;

        out.raw.resize(static_cast<size_t>(numScansReceived * params.numChannels));
        for (int i = 0; i < numScansReceived * params.numChannels; ++i)
        {
          const uint32 host_u32 = ntohl(bdata[static_cast<size_t>(i)]);
          out.raw[static_cast<size_t>(i)] = static_cast<int32_t>(host_u32);
        }
      }
    }

  sleep_and_return:
    // Single pacing point (one nanosleep per ReadFrame), aligned with Sample’s absolute sleep style
    TimespecAddNs(&next, periodns);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

    return true; // keep running; upper layer decides stop condition
  }

} // namespace uei
