#include "dsp/Moving_Average.hpp"

#include <algorithm>
#include <stdexcept>

namespace uei::dsp
{

  MovingAverageProcessor::MovingAverageProcessor(const std::unordered_map<std::string, MovingAverageConfig> &configs) : configs_(configs) {}

  std::vector<uei::RawFrame> MovingAverageProcessor::ProcessFrame(uei::RawFrame &&frame)
  {
    std::vector<uei::RawFrame> outputs;

    const auto it_cfg = configs_.find(frame.group_name);
    const bool ma_active = (it_cfg != configs_.end() && it_cfg->second.active && it_cfg->second.decimation > 1);

    // 無設定或 decimation<=1 則直通
    if (!ma_active)
    {
      outputs.push_back(std::move(frame));
      return outputs;
    }

    const int decimation = it_cfg->second.decimation;
    if (decimation <= 1)
      throw std::runtime_error("MovingAverage decimation must be > 1 when active.");

    State &st = states_[frame.group_name];

    // 尺寸或 decimation 變更時重置累積
    if (st.decimation != decimation ||
        st.samples_per_channel != frame.samples_per_channel ||
        st.num_channels != frame.num_channels ||
        st.accum.size() != frame.raw.size())
    {
      st.decimation = decimation;
      st.samples_per_channel = frame.samples_per_channel;
      st.num_channels = frame.num_channels;
      st.frames_accum = 0;
      st.accum.assign(frame.raw.size(), 0);
    }

    // 累加
    const size_t sz = frame.raw.size();
    for (size_t i = 0; i < sz; ++i)
    {
      st.accum[i] += static_cast<int64_t>(frame.raw[i]);
    }
    st.frames_accum += 1;

    // 未達 decimation：暫不輸出
    if (st.frames_accum < decimation)
      return outputs;

    // 輸出平均後的 frame
    uei::RawFrame out = frame; // 保留追蹤資訊
    out.raw.resize(sz);
    for (size_t i = 0; i < sz; ++i)
    {
      out.raw[i] = static_cast<int32_t>(st.accum[i] / decimation);
    }

    // 重置累積
    st.frames_accum = 0;
    std::fill(st.accum.begin(), st.accum.end(), 0);

    outputs.push_back(std::move(out));
    return outputs;
  }

} // namespace uei::dsp
