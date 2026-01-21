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

    // 重置累積條件：decimation 或通道數改變
    if (st.decimation != decimation || st.num_channels != frame.num_channels)
    {
      st.decimation = decimation;
      st.num_channels = frame.num_channels;
      st.samples_in_window = 0;
      st.accum.assign(static_cast<size_t>(frame.num_channels), 0);
    }

    const int num_ch = frame.num_channels;
    const int decim = st.decimation;
    const int samples = frame.samples_per_channel;
    const size_t total_in = frame.raw.size();

    // 預估輸出樣本數（整除部份），提前保留容量以減少重配置
    const int possible_out_samples = (st.samples_in_window + samples) / decim;
    std::vector<int32_t> out_raw;
    out_raw.reserve(static_cast<size_t>(possible_out_samples * num_ch));

    for (int s = 0; s < samples; ++s)
    {
      const int base = s * num_ch;
      for (int ch = 0; ch < num_ch; ++ch)
      {
        const size_t idx = static_cast<size_t>(base + ch);
        if (idx >= total_in)
          throw std::runtime_error("MovingAverage: input index out of range");
        st.accum[static_cast<size_t>(ch)] += static_cast<int64_t>(frame.raw[idx]);
      }
      st.samples_in_window += 1;

      if (st.samples_in_window == decim)
      {
        for (int ch = 0; ch < num_ch; ++ch)
        {
          out_raw.push_back(static_cast<int32_t>(st.accum[static_cast<size_t>(ch)] / decim));
          st.accum[static_cast<size_t>(ch)] = 0;
        }
        st.samples_in_window = 0;
      }
    }

    const int out_samples = static_cast<int>(out_raw.size() / static_cast<size_t>(num_ch));
    if (out_samples <= 0)
      return outputs; // 尚未累積到 decimation，保留尾巴等待下一 frame

    uei::RawFrame out = frame; // 保留 seq / slot / group metadata
    out.samples_per_channel = out_samples;
    out.raw = std::move(out_raw);
    outputs.push_back(std::move(out));
    return outputs;
  }

} // namespace uei::dsp
