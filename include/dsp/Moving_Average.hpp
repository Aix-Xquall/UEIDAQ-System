#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "utils/UeiStructs.hpp"

namespace uei::dsp
{

  /**
   * @brief Moving Average 設定。
   *
   * decimation <= 1 視為停用，僅直通原始資料。
   */
  struct MovingAverageConfig
  {
    bool active{false};
    int decimation{1};
  };

  /**
   * @brief 針對每個 group_name 進行移動平均與降頻的處理器（frame 內 decimation）。
   *
   * - 對單一 frame 內的樣本，以 decimation 為窗做降頻平均。
   * - 尾端不足 decimation 的樣本會跨 frame 暫存並在下一 frame 補齊。
   * - 若通道數或 decimation 改變，會重置累積並重新開始計算。
   */
  class MovingAverageProcessor
  {
  public:
    /**
     * @brief 建立處理器。
     * @param configs group_name 對應的 Moving Average 設定。
     */
    explicit MovingAverageProcessor(const std::unordered_map<std::string, MovingAverageConfig> &configs);

    /**
     * @brief 處理一筆 RawFrame，回傳 0 或多筆已完成平均的結果。
     * @param frame 輸入的 RawFrame（將被移動）。
     * @return 已完成平均的 RawFrame 列表；未達 decimation 時回傳空。
     * @throws std::runtime_error 當 decimation 非法等嚴重錯誤。
     */
    std::vector<uei::RawFrame> ProcessFrame(uei::RawFrame &&frame);

  private:
    struct State
    {
      int decimation{1};
      int num_channels{0};
      int samples_in_window{0};
      std::vector<int64_t> accum; // size = num_channels
    };

    std::unordered_map<std::string, MovingAverageConfig> configs_;
    std::unordered_map<std::string, State> states_;
  };

} // namespace uei::dsp
