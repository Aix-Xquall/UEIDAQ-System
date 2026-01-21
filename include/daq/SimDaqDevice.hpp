#pragma once

#include <string>
#include <vector>

#include "daq/DAQDevice.hpp"
#include "utils/UeiStructs.hpp"

namespace uei
{

  /**
   * @brief 模擬 UEI DAQ (AI-217) 的裝置，產生 scan-major int32 原始碼。
   *
   * - 仍遵循 DAQDevice 介面，便於在 DAQFactory 中以設定切換真實/模擬。
   * - 以 offset-binary 24-bit 形式輸出（0=-10V, 0x800000=0V, 0xFFFFFF=+10V）。
   * - 使用簡單的正弦波 + 雜訊，頻率依 channel index 線性遞增。
   */
  class SimDaqDevice final : public DAQDevice
  {
  public:
    struct Params
    {
      int slot_index{0};
      std::string group_name;
      std::vector<int> channels;

      double sample_rate_hz{0.0};
      int samples_per_channel{0};

      double base_frequency{100.0};
      double frequency_step_percent{20.0};
      double amplitude{1.0};
      double noise_percent{5.0};
    };

    explicit SimDaqDevice(const Params &params);

    void Open() override;
    void Start() override;
    void Stop() override;
    void Close() override;
    bool ReadFrame(RawFrame &out) override;

  private:
    static int32_t VoltToCode(double volt);

    Params params_;

    uint32_t seq_{0};
    double t_{0.0};
    bool running_{false};
  };

} // namespace uei
