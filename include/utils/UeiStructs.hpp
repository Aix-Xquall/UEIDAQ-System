#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace uei
{

  /** @brief Moving average config (reserved; MVP not enabled). */
  struct MovingAverageConfig
  {
    bool active{false};
    int decimation{1};
  };

  /** @brief FFT config (reserved; MVP not enabled). */
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

  /** @brief Analog input config in generic semantic values (board-specific mapping in driver). */
  struct AiConfig
  {
    int gain{1};            ///< generic gain multiplier (MVP supports 1 only for AI-217)
    std::string input_mode; ///< "diff" (MVP supports "diff" only)
  };

  /** @brief Per-slot configuration. */
  struct SlotConfig
  {
    int slot_index{0};
    std::string board_name;
    bool active{false};

    int device_id{0}; ///< UEI device id (required for MVP)
    std::string subsystem{"DQ_SS0IN"};

    double sample_rate_hz{0.0}; ///< base scan rate
    AiConfig ai_config;

    std::vector<ChannelGroupConfig> channel_groups;
  };

  /** @brief UEI library/RT settings. */
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

    int config_version{2};

    UeiConfig uei;

    /** @brief Desired UDP packet interval in milliseconds (used to derive samples_per_channel). */
    int packet_interval_ms{1000};

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
