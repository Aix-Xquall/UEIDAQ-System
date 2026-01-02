#include "daq/DAQ_VMAP_AI217.hpp"

#include <arpa/inet.h> // ntohl
#include <cmath>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <sched.h>
#include <time.h>

namespace uei
{

  static std::atomic<bool> g_stop(false);

  static void LogInfo(const std::string &msg) { fprintf(stdout, "[INFO] %s\n", msg.c_str()); }
  static void LogWarn(const std::string &msg) { fprintf(stderr, "[WARN] %s\n", msg.c_str()); }
  static void LogErr(const std::string &msg) { fprintf(stderr, "[ERROR] %s\n", msg.c_str()); }

  DAQ_VMAP_AI217::DAQ_VMAP_AI217(const Params &p)
      : p_(p)
  {
    vmap_flag_ = DQ_VMAP_FIFO_STATUS | DQ_VMAP_FIFO_CLR_ON_OVF;
  }

  void DAQ_VMAP_AI217::SigIntHandler(int)
  {
    g_stop.store(true);
  }

  void DAQ_VMAP_AI217::InstallSigIntHandler()
  {
    std::signal(SIGINT, &DAQ_VMAP_AI217::SigIntHandler);
  }

  void DAQ_VMAP_AI217::TimespecAddNs(struct timespec *t, uint64_t ns)
  {
    const uint64_t NSECS_PER_SEC = 1000000000ULL;
    uint64_t n = static_cast<uint64_t>(t->tv_nsec) + ns;
    t->tv_sec += static_cast<time_t>(n / NSECS_PER_SEC);
    t->tv_nsec = static_cast<long>(n % NSECS_PER_SEC);
  }

  static void ApplyRtScheduling(bool enable_rt, int prio)
  {
    if (!enable_rt)
      return;

    struct sched_param sp;
    std::memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
    {
      LogWarn("sched_setscheduler(SCHED_FIFO) failed; continuing without RT priority.");
    }
    else
    {
      LogInfo("SCHED_FIFO enabled, priority=" + std::to_string(prio));
    }
  }

  static int Ai217GainToMacro(int gain)
  {
    // MVP: only support gain=1 to avoid silent wrong config
    if (gain == 1)
      return DQ_AI217_GAIN_1;
    throw std::runtime_error("AI-217: unsupported ai_config.gain (MVP supports gain=1 only)");
  }

  static int InputModeToMacro(const std::string &mode)
  {
    // MVP: only support "diff" to avoid guessing other macros
    if (mode == "diff")
      return DQ_LNCL_DIFF;
    throw std::runtime_error("AI-217: unsupported ai_config.input_mode (MVP supports \"diff\" only)");
  }

  void DAQ_VMAP_AI217::Open()
  {
    InstallSigIntHandler();
    ApplyRtScheduling(p_.enable_rt, p_.rt_priority);

    if (p_.channels.empty())
      throw std::runtime_error("AI-217: channels list is empty.");
    if (p_.samples_per_channel <= 0)
      throw std::runtime_error("AI-217: samples_per_channel must be > 0.");
    if (p_.scan_rate_hz <= 0.0)
      throw std::runtime_error("AI-217: scan_rate_hz must be > 0.");
    if (p_.subsystem != "DQ_SS0IN")
      throw std::runtime_error("AI-217 MVP: only subsystem DQ_SS0IN is supported.");

    // UEI init/open
    DqInitDAQLib();

    // DqOpenIOM expects char* in headers; use mutable buffer to avoid -Wwrite-strings
    char iom_ip_buf[64];
    std::memset(iom_ip_buf, 0, sizeof(iom_ip_buf));
    std::snprintf(iom_ip_buf, sizeof(iom_ip_buf) - 1, "%s", p_.iom_ip.c_str());

    int ret = DqOpenIOM(iom_ip_buf, DQ_UDP_DAQ_PORT, p_.open_timeout_ms, &hd_, &rd_cfg_);
    if (ret < 0)
    {
      DqCleanUpDAQLib();
      throw std::runtime_error("DqOpenIOM failed ret=" + std::to_string(ret));
    }

    ret = DqRtVmapInit(hd_, &vmapid_, p_.scan_rate_hz);
    if (ret < 0)
    {
      Close();
      throw std::runtime_error("DqRtVmapInit failed ret=" + std::to_string(ret));
    }

    // Build channel entries: channel_id | gain | input_mode
    const int gain_macro = Ai217GainToMacro(p_.gain);
    const int mode_macro = InputModeToMacro(p_.input_mode);

    std::vector<int> ch_entries = p_.channels;
    for (auto &ce : ch_entries)
    {
      ce |= DQ_LNCL_GAIN(gain_macro) | mode_macro;
    }

    ret = DqRtVmapAddChannel(hd_, vmapid_, p_.device_id, DQ_SS0IN, ch_entries.data(), &vmap_flag_, 1);
    if (ret < 0)
    {
      Close();
      throw std::runtime_error("DqRtVmapAddChannel failed ret=" + std::to_string(ret));
    }

    ret = DqRtVmapSetScanRate(hd_, vmapid_, p_.device_id, DQ_SS0IN, p_.scan_rate_hz);
    if (ret < 0)
    {
      Close();
      throw std::runtime_error("DqRtVmapSetScanRate failed ret=" + std::to_string(ret));
    }

    ret = DqRtVmapSetChannelList(hd_, vmapid_, p_.device_id, DQ_SS0IN,
                                 ch_entries.data(), static_cast<int>(ch_entries.size()));
    if (ret < 0)
    {
      Close();
      throw std::runtime_error("DqRtVmapSetChannelList failed ret=" + std::to_string(ret));
    }

    // Allocate raw buffer (uint32 words)
    const int num_channels = static_cast<int>(p_.channels.size());
    bdata_.resize(static_cast<size_t>(num_channels * p_.samples_per_channel));

    // Pacing: desired packet interval, but must not exceed FIFO-safe max period.
    // VMAP samples use refresh_rate = (scan_rate * num_channels)/1024 (half FIFO heuristic).
    const double fifo_safe_refresh_hz = (p_.scan_rate_hz * num_channels) / 1024.0;
    const double safe_refresh_hz = (fifo_safe_refresh_hz > 0.0) ? fifo_safe_refresh_hz : 1.0;
    const uint64_t fifo_max_period_ns = static_cast<uint64_t>(std::floor(1000000000.0 / safe_refresh_hz));

    const uint64_t desired_period_ns = static_cast<uint64_t>(p_.packet_interval_ms) * 1000000ULL;
    period_ns_ = (desired_period_ns < fifo_max_period_ns) ? desired_period_ns : fifo_max_period_ns;

    clock_gettime(CLOCK_MONOTONIC, &next_);

    LogInfo("AI217 Open OK: iom_ip=" + p_.iom_ip +
            " device_id=" + std::to_string(p_.device_id) +
            " scan_rate=" + std::to_string(p_.scan_rate_hz) +
            " channels=" + std::to_string(num_channels) +
            " samples_per_channel=" + std::to_string(p_.samples_per_channel) +
            " period_ms=" + std::to_string(period_ns_ / 1000000ULL));
  }

  void DAQ_VMAP_AI217::Start()
  {
    int ret = DqRtVmapStart(hd_, vmapid_);
    if (ret < 0)
      throw std::runtime_error("DqRtVmapStart failed ret=" + std::to_string(ret));

    ret = DqCmdSwTrigger(hd_, 1 << p_.device_id);
    if (ret < 0)
      throw std::runtime_error("DqCmdSwTrigger failed ret=" + std::to_string(ret));

    LogInfo("AI217 acquisition started.");
  }

  void DAQ_VMAP_AI217::Stop()
  {
    stop_.store(true);
    g_stop.store(true);
    if (hd_ && vmapid_)
      DqRtVmapStop(hd_, vmapid_);
  }

  void DAQ_VMAP_AI217::Close()
  {
    if (hd_ && vmapid_)
    {
      DqRtVmapClose(hd_, vmapid_);
      vmapid_ = 0;
    }
    if (hd_)
    {
      DqCloseIOM(hd_);
      hd_ = 0;
    }
    DqCleanUpDAQLib();
  }

  bool DAQ_VMAP_AI217::ReadFrame(RawFrame &out)
  {
    if (stop_.load() || g_stop.load())
      return false;

    const int num_channels = static_cast<int>(p_.channels.size());
    const int req_words = num_channels * p_.samples_per_channel;
    const int req_bytes = req_words * static_cast<int>(sizeof(uint32));

    int act_size = 0;
    int ret = DqRtVmapRqInputDataSz(hd_, vmapid_, 0, req_bytes, &act_size, NULL);
    if (ret < 0)
    {
      LogErr("DqRtVmapRqInputDataSz failed ret=" + std::to_string(ret));
      return false;
    }

    ret = DqRtVmapRefresh(hd_, vmapid_, 0);
    if (ret < 0)
    {
      LogErr("DqRtVmapRefresh failed ret=" + std::to_string(ret));
      return false;
    }

    int data_size = 0;
    int avl_size = 0;
    ret = DqRtVmapGetInputData(hd_, vmapid_, 0, req_bytes, &data_size, &avl_size,
                               reinterpret_cast<uint8 *>(bdata_.data()));
    if (ret < 0)
    {
      LogErr("DqRtVmapGetInputData failed ret=" + std::to_string(ret));
      return false;
    }

    const int got_words = data_size / static_cast<int>(sizeof(uint32));
    const int got_scans = (num_channels > 0) ? (got_words / num_channels) : 0;

    out.seq = seq_++;
    out.slot_index = p_.slot_index;
    out.group_name = p_.group_name;
    out.samples_per_channel = got_scans;
    out.num_channels = num_channels;

    out.raw.resize(static_cast<size_t>(got_scans * num_channels));
    for (int i = 0; i < got_scans * num_channels; ++i)
    {
      const uint32_t host_u32 = ntohl(bdata_[static_cast<size_t>(i)]);
      out.raw[static_cast<size_t>(i)] = static_cast<int32_t>(host_u32);
    }

    // Pace loop
    TimespecAddNs(&next_, period_ns_);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_, NULL);

    return true;
  }

} // namespace uei
