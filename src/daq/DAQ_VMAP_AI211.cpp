#include "daq/DAQ_VMAP_AI211.hpp"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <arpa/inet.h> // ntohl
#include <algorithm>
#include <cctype>
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

  void DAQ_VMAP_AI211::SigIntHandler(int) { g_stop.store(true); }

  void DAQ_VMAP_AI211::InstallSigIntHandler()
  {
    std::signal(SIGINT, &DAQ_VMAP_AI211::SigIntHandler);
  }

  void DAQ_VMAP_AI211::TimespecAddNs(struct timespec *t, long long ns)
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

  static int Ai211GainToMacro(int gain)
  {
    if (gain == 1)
      return DQ_AI211_GAIN_1;
    throw std::runtime_error("AI-211: unsupported gain (MVP supports gain=1 only)");
  }

  static int InputModeToMacro(const std::string &mode)
  {
    if (mode == "diff")
      return DQ_LNCL_DIFF;
    throw std::runtime_error("AI-211: unsupported input_mode (MVP supports \"diff\" only)");
  }

  static std::string Normalize(const std::string &in)
  {
    std::string out = in;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
  }

  static bool ParseUint16(const std::string &s, uint16_t &out)
  {
    if (s.empty())
      return false;
    char *end = nullptr;
    long v = std::strtol(s.c_str(), &end, 0);
    if (end == s.c_str() || *end != '\0')
      return false;
    if (v < 0 || v > 0xFFFF)
      return false;
    out = static_cast<uint16_t>(v);
    return true;
  }

  static uint16_t ParseHpf(const std::string &v)
  {
    const std::string n = Normalize(v);
    if (n == "dc")
      return DQ_211_HPF_DC;
    if (n == "0.1hz" || n == "0p1hz" || n == "0_1hz")
      return DQ_211_HPF_POINT1_HZ;
    if (n == "1hz")
      return DQ_211_HPF_1_HZ;
    if (n == "10hz")
      return DQ_211_HPF_10_HZ;
    uint16_t val = 0;
    if (ParseUint16(n, val))
      return val;
    throw std::runtime_error("AI-211: unsupported hpf setting: " + v);
  }

  static uint16_t ParseAnalogFilter(const std::string &v)
  {
    const std::string n = Normalize(v);
    if (n == "on")
      return DQ_211_ANALOG_FILTER_ON;
    if (n == "off")
      return DQ_211_ANALOG_FILTER_OFF;
    uint16_t val = 0;
    if (ParseUint16(n, val))
      return val;
    throw std::runtime_error("AI-211: unsupported analog_filter setting: " + v);
  }

  static uint16_t ParseCompHi(const std::string &v)
  {
    const std::string n = Normalize(v);
    if (n == "std")
      return DQ_211_COMP_HI_STD;
    if (n == "default")
      return DQ_211_COMP_HI_DEFAULT;
    uint16_t val = 0;
    if (ParseUint16(n, val))
      return val;
    throw std::runtime_error("AI-211: unsupported comp_hi setting: " + v);
  }

  static uint16_t ParseCompLo(const std::string &v)
  {
    const std::string n = Normalize(v);
    if (n == "std")
      return DQ_211_COMP_LO_STD;
    if (n == "default")
      return DQ_211_COMP_LO_DEFAULT;
    uint16_t val = 0;
    if (ParseUint16(n, val))
      return val;
    throw std::runtime_error("AI-211: unsupported comp_lo setting: " + v);
  }

  static uint16_t ParseAlarm(const std::string &v)
  {
    const std::string n = Normalize(v);
    if (n == "on")
      return DQ_211_ALARM_ON;
    if (n == "off")
      return DQ_211_ALARM_OFF;
    if (n == "red")
      return DQ_211_ALARM_RED;
    if (n == "green")
      return DQ_211_ALARM_GREEN;
    if (n == "orange")
      return DQ_211_ALARM_ORANGE;
    uint16_t val = 0;
    if (ParseUint16(n, val))
      return val;
    throw std::runtime_error("AI-211: unsupported alarm setting: " + v);
  }

  DAQ_VMAP_AI211::DAQ_VMAP_AI211(const PDNA_PARAMS &p) : params(p)
  {
    vmap_flag = DQ_VMAP_FIFO_STATUS;
  }

  void DAQ_VMAP_AI211::Open()
  {
    InstallSigIntHandler();
    ApplyRtScheduling(params.enable_rt, params.rt_priority);

    if (params.channels.empty())
      throw std::runtime_error("AI-211: channels empty");
    if (params.frequency <= 0.0)
      throw std::runtime_error("AI-211: frequency must be > 0");
    if (params.numSamplesPerChannel <= 0)
      throw std::runtime_error("AI-211: numSamplesPerChannel must be > 0");

    params.numChannels = static_cast<int>(params.channels.size());

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

    if (params.apply_layer_default)
    {
      DQCFGLAYER_211 cfgLayer;
      std::memset(&cfgLayer, 0, sizeof(cfgLayer));
      cfgLayer.mask = DQAI211_CFGLAYER_DEFAULTSET;
      ret = DqAdv211SetCfgLayer(hd, params.device, &cfgLayer);
      if (ret < 0)
      {
        Close();
        throw std::runtime_error("DqAdv211SetCfgLayer failed ret=" + std::to_string(ret));
      }
    }

    if (params.apply_channel_config)
    {
      DQCFGCH_211 cfgCh;
      std::memset(&cfgCh, 0, sizeof(cfgCh));
      cfgCh.channels = DQ_AI211_SEL_CHAN_ALL;
      cfgCh.mask = DQAI211_HPFSET |
                   DQAI211_ANAFILTSET |
                   DQAI211_COMPHISET |
                   DQAI211_COMPLOSET |
                   DQAI211_ALARMCTRLSET;
      cfgCh.hpf = ParseHpf(params.hpf);
      cfgCh.anafilt = ParseAnalogFilter(params.analog_filter);
      cfgCh.comphi = ParseCompHi(params.comp_hi);
      cfgCh.complo = ParseCompLo(params.comp_lo);
      cfgCh.alarmctrl = ParseAlarm(params.alarm);

      ret = DqAdv211SetCfgChannel(hd, params.device, &cfgCh);
      if (ret < 0)
      {
        Close();
        throw std::runtime_error("DqAdv211SetCfgChannel (base) failed ret=" + std::to_string(ret));
      }
    }

    const int gain_macro = Ai211GainToMacro(params.gain);
    const int mode_macro = InputModeToMacro(params.input_mode);
    const double bias_ma = std::max(0.0, std::min(8.0, params.bias_drive_ma));

    uint32 channelSelector[] = {DQ_AI211_SEL_CHAN_0, DQ_AI211_SEL_CHAN_1, DQ_AI211_SEL_CHAN_2, DQ_AI211_SEL_CHAN_3};

    for (size_t i = 0; i < params.channels.size(); ++i)
    {
      const int base_ch = DQ_LNCL_GETCHAN(params.channels[i]);
      if (base_ch < 0 || base_ch >= static_cast<int>(sizeof(channelSelector) / sizeof(channelSelector[0])))
      {
        Close();
        throw std::runtime_error("AI-211: unsupported channel index: " + std::to_string(base_ch));
      }

      params.channels[i] |= DQ_LNCL_GAIN(gain_macro) | mode_macro;

      DQCFGCH_211 cfgCh;
      std::memset(&cfgCh, 0, sizeof(cfgCh));
      cfgCh.channels = channelSelector[base_ch];
      cfgCh.mask = DQAI211_BIASDRIVESET | DQAI211_BIASONOFFSET;
      cfgCh.biasdrive = static_cast<uint16>(DQ211_DRIVE_CURRENT(bias_ma));
      cfgCh.biasonoff = params.bias_on ? DQ_211_BIAS_ON : DQ_211_BIAS_OFF;

      ret = DqAdv211SetCfgChannel(hd, params.device, &cfgCh);
      if (ret < 0)
      {
        Close();
        throw std::runtime_error("DqAdv211SetCfgChannel (bias) failed ret=" + std::to_string(ret));
      }
    }

    ret = DqRtVmapInit(hd, &vmapid, params.frequency);
    if (ret < 0)
    {
      Close();
      throw std::runtime_error("DqRtVmapInit failed ret=" + std::to_string(ret));
    }

    ret = DqRtVmapAddChannel(hd, vmapid, params.device, DQ_SS0IN, params.channels.data(), &vmap_flag, 1);
    if (ret < 0)
    {
      Close();
      throw std::runtime_error("DqRtVmapAddChannel failed ret=" + std::to_string(ret));
    }

    ret = DqRtVmapSetChannelList(hd, vmapid, params.device, DQ_SS0IN, params.channels.data(), static_cast<int>(params.channels.size()));
    if (ret < 0)
    {
      Close();
      throw std::runtime_error("DqRtVmapSetChannelList failed ret=" + std::to_string(ret));
    }

    ret = DqRtVmapSetScanRate(hd, vmapid, params.device, DQ_SS0IN, params.frequency);
    if (ret < 0)
    {
      Close();
      throw std::runtime_error("DqRtVmapSetScanRate failed ret=" + std::to_string(ret));
    }

    bdata_storage.resize(static_cast<size_t>(params.numChannels * params.numSamplesPerChannel));

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
      period_s = 0.001;

    periodns = static_cast<long long>(std::floor(1000000000.0 * period_s));
    clock_gettime(CLOCK_MONOTONIC, &next);

    LogInfo("AI211 Open OK: device=" + std::to_string(params.device) +
            " frequency=" + std::to_string(params.frequency) +
            " numChannels=" + std::to_string(params.numChannels) +
            " numSamplesPerChannel=" + std::to_string(params.numSamplesPerChannel) +
            " period_ms=" + std::to_string(periodns / 1000000LL));
    LogInfo("AI211 pacing: fifo_ms=" + std::to_string(half_fifo_time * 1000.0) +
            " read_ms=" + std::to_string(read_time * 1000.0) +
            " ui_ms=" + std::to_string(ui_time * 1000.0) +
            " period_ms=" + std::to_string(periodns / 1000000.0));
  }

  void DAQ_VMAP_AI211::Start()
  {
    int ret = DqRtVmapStart(hd, vmapid);
    if (ret < 0)
      throw std::runtime_error("DqRtVmapStart failed ret=" + std::to_string(ret));

    ret = DqCmdSwTrigger(hd, 1 << params.device);
    if (ret < 0)
      throw std::runtime_error("DqCmdSwTrigger failed ret=" + std::to_string(ret));

    LogInfo("AI211 acquisition started.");
  }

  void DAQ_VMAP_AI211::Stop()
  {
    stop_.store(true);
    g_stop.store(true);
    if (hd && vmapid)
      DqRtVmapStop(hd, vmapid);
  }

  void DAQ_VMAP_AI211::Close()
  {
    if (hd)
    {
      DQCFGCH_211 cfgCh;
      std::memset(&cfgCh, 0, sizeof(cfgCh));
      cfgCh.channels = DQ_AI211_SEL_CHAN_ALL;
      cfgCh.mask = DQAI211_CFGCH_DEFAULTSET;
      DqAdv211SetCfgChannel(hd, params.device, &cfgCh);
    }

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

  bool DAQ_VMAP_AI211::ReadFrame(RawFrame &out)
  {
    if (stop_.load() || g_stop.load())
      return false;

    const int req_bytes = params.numSamplesPerChannel * params.numChannels * static_cast<int>(sizeof(uint32));

    int act_size = 0;
    int data_size = 0;
    int avl_size = 0;

    uint32 *bdata = bdata_storage.data();

    int ret = 0;

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
    TimespecAddNs(&next, periodns);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

    return true;
  }

} // namespace uei
