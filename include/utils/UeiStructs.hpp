#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace uei
{

  /** @brief Moving average config (reserved). */
  struct MovingAverageConfig
  {
    bool active{false};
    int decimation{1};
  };

  /** @brief FFT config (reserved). */
  struct FftConfig
  {
    bool active{false};
    int size{1024};
    std::string window_type{"hann"};
    double overlap{0.5};
  };

  /** @brief Channel group configuration. */
  struct ChannelGroupConfig
  {
    std::string group_name;
    bool active{false}; ///< Rule A: active=true means this group will be streamed
    double target_hz{0.0};
    std::vector<int> channels;

    MovingAverageConfig moving_average;
    FftConfig fft;
  };

  /** @brief Analog input config (semantic values; board driver maps to UEI macros). */
  struct AiConfig
  {
    int gain{1};                    ///< MVP: AI-217 supports 1
    std::string input_mode{"diff"}; ///< MVP: "diff"
  };

  /** @brief Per-slot configuration. */
  struct SlotConfig
  {
    int slot_index{0};
    std::string board_name;
    bool active{false};

    int device_id{0};
    std::string subsystem{"DQ_SS0IN"};

    /** @brief Sample scan rate in Hz. In AI-217 driver, this maps to Sample's "frequency". */
    double sample_rate_hz{0.0};

    AiConfig ai_config;
    std::vector<ChannelGroupConfig> channel_groups;
  };

  /** @brief UEI open/RT settings. */
  struct UeiConfig
  {
    std::string iom_ip{"127.0.0.1"};
    int open_timeout_ms{500};
    bool enable_rt{true};
    int rt_priority{80};
  };

  /** @brief System settings. */
  struct Settings
  {
    std::string system_name;
    std::string udp_target_ip;
    uint16_t udp_target_port{0};

    struct DaqSimulationSettings
    {
      bool active{false};
      double base_frequency{100.0};
      double frequency_step_percent{20.0};
      double amplitude{1.0};
      double noise_percent{5.0};
    };

    DaqSimulationSettings daq_simulation;

    int config_version{2};
    UeiConfig uei;

    /**
     * @brief Number of scans per channel requested per ReadFrame.
     * Aligned to SampleVMap217.c PDNA_PARAMS::numSamplesPerChannel.
     */
    int numSamplesPerChannel{0};

    std::vector<SlotConfig> slots;
  };

  /** @brief Raw data frame from UEI (scan-major interleaved). */
  struct RawFrame
  {
    uint32_t seq{0};
    int slot_index{0};
    std::string group_name;

    int samples_per_channel{0}; ///< actual scans received
    int num_channels{0};

    /** scan-major interleaved raw samples (int32). size = samples_per_channel * num_channels */
    std::vector<int32_t> raw;
  };

} // namespace uei
