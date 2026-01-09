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
   * @brief AI-217 VMAP acquisition device (behavior aligned with SampleVMap217.c).
   */
  class DAQ_VMAP_AI217 final : public DAQDevice
  {
  public:
    /**
     * @brief PDNA_PARAMS-like runtime parameters (names aligned to SampleVMap217.c).
     */
    struct PDNA_PARAMS
    {
      int device{0};
      int numChannels{0};
      std::vector<int> channels;

      double frequency{0.0};       ///< sample scan rate (Hz) - aligned name with Sample
      int numSamplesPerChannel{0}; ///< aligned name with Sample

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
      std::string input_mode{"diff"}; // "diff" only in MVP
    };

    explicit DAQ_VMAP_AI217(const PDNA_PARAMS &params);

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

    // UEI handles (names chosen to resemble Sample)
    int hd{0};
    int vmapid{0};
    DQRDCFG *rd_cfg{nullptr};

    // VMAP flags
    int vmap_flag{0};

    // pacing vars (names aligned to Sample)
    long long periodns{0};
    struct timespec next{};

    // raw buffer (bdata in Sample)
    std::vector<uint32_t> bdata_storage;

    std::atomic<bool> stop_{false};
    uint32_t seq_{0};
  };

} // namespace uei
