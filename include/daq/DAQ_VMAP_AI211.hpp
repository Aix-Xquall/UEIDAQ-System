#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <time.h>

#include "daq/DAQDevice.hpp"
#include "utils/UeiStructs.hpp"

// UEI headers
extern "C"
{
#include "PDNA.h"
#include "UeiPacUtils.h"
}

namespace uei
{

  /**
   * @brief AI-211 VMAP acquisition device (behavior aligned with SampleVMap211.c).
   */
  class DAQ_VMAP_AI211 final : public DAQDevice
  {
  public:
    /**
     * @brief PDNA_PARAMS-like runtime parameters (names aligned to SampleVMap211.c).
     */
    struct PDNA_PARAMS
    {
      int device{0};
      int numChannels{0};
      std::vector<int> channels;

      double frequency{0.0};       ///< sample scan rate (Hz)
      int numSamplesPerChannel{0};

      // Project metadata
      int slot_index{0};
      std::string group_name;

      // UEI open/RT config
      std::string iom_ip{"127.0.0.1"};
      int open_timeout_ms{500};
      bool enable_rt{true};
      int rt_priority{80};

      // AI config
      int gain{1};
      std::string input_mode{"diff"};

      // AI-211 advanced config (all channels share the same settings)
      bool apply_layer_default{true};
      bool apply_channel_config{true};
      std::string hpf{"dc"};
      std::string analog_filter{"on"};
      std::string comp_hi{"std"};
      std::string comp_lo{"std"};
      std::string alarm{"on"};
      bool bias_on{true};
      double bias_drive_ma{1.0};
    };

    explicit DAQ_VMAP_AI211(const PDNA_PARAMS &params);

    void Open() override;
    void Start() override;
    void Stop() override;
    void Close() override;

    /**
     * @brief Read one frame of raw interleaved samples.
     * @param out Output raw frame (scan-major interleaved).
     * @return true to continue running; false to stop acquisition.
     */
    bool ReadFrame(RawFrame &out) override;

  private:
    static void SigIntHandler(int);
    static void InstallSigIntHandler();

    static void TimespecAddNs(struct timespec *t, long long ns);

    PDNA_PARAMS params;

    // UEI handles
    int hd{0};
    int vmapid{0};
    DQRDCFG *rd_cfg{nullptr};

    // VMAP flags
    int vmap_flag{0};

    // pacing vars
    long long periodns{0};
    struct timespec next{};

    // raw buffer
    std::vector<uint32_t> bdata_storage;

    std::atomic<bool> stop_{false};
    uint32_t seq_{0};
  };

} // namespace uei
