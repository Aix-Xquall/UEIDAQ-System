#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "daq/DAQDevice.hpp"
#include "utils/UeiStructs.hpp"

// UEI SDK headers (C API)
extern "C"
{
#include "PDNA.h"
#include "UeiPacUtils.h"
}

namespace uei
{

  /**
   * @brief AI-217 VMAP acquisition device (MVP: raw int32 + UDP).
   */
  class DAQ_VMAP_AI217 final : public DAQDevice
  {
  public:
    struct Params
    {
      // UEI connection
      std::string iom_ip{"127.0.0.1"};
      int open_timeout_ms{500};

      // RT scheduling
      bool enable_rt{true};
      int rt_priority{80};

      // Device identifiers
      int device_id{0};
      int slot_index{1};
      std::string group_name;

      // Acquisition
      std::string subsystem{"DQ_SS0IN"}; ///< MVP supports DQ_SS0IN only
      double scan_rate_hz{10.0};
      int samples_per_channel{10};
      std::vector<int> channels;

      // Analog input config (generic semantic values)
      int gain{1};            ///< MVP supports 1 only
      std::string input_mode; ///< MVP supports "diff" only

      // Packet pacing
      int packet_interval_ms{1000}; ///< desired pacing interval (ms)
    };

    explicit DAQ_VMAP_AI217(const Params &p);

    void Open() override;
    void Start() override;
    void Stop() override;
    void Close() override;
    bool ReadFrame(RawFrame &out) override;

  private:
    static void InstallSigIntHandler();
    static void SigIntHandler(int);

    static void TimespecAddNs(struct timespec *t, uint64_t ns);

    Params p_;
    std::atomic<bool> stop_{false};

    int hd_{0};
    int vmapid_{0};
    DQRDCFG *rd_cfg_{nullptr};

    int vmap_flag_{0};

    std::vector<uint32> bdata_; // UEI raw buffer (uint32 words)
    uint32_t seq_{0};

    struct timespec next_{};
    uint64_t period_ns_{0};
  };

} // namespace uei
