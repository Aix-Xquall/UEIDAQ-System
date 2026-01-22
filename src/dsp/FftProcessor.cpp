#include "dsp/FftProcessor.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <stdexcept>

extern "C" {
#include "dsp/kiss_fftr.h"
}

namespace uei::dsp
{

  namespace
  {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kFullScaleCode = 8388608.0;
    constexpr double kMinRatio = 1e-12; // avoid -inf dB
  } // namespace

  FftProcessor::FftProcessor(const std::unordered_map<std::string, FftProcessorConfig> &configs) : configs_(configs) {}

  FftProcessor::~FftProcessor()
  {
    for (auto &kv : states_)
    {
      if (kv.second.kiss_cfg)
      {
        kiss_fftr_free(kv.second.kiss_cfg);
        kv.second.kiss_cfg = nullptr;
      }
    }
  }

  std::string FftProcessor::NormalizeWindowName(const std::string &window_type)
  {
    std::string name = window_type;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
  }

  std::vector<double> FftProcessor::BuildWindow(const std::string &window_type, int size)
  {
    std::vector<double> w;
    if (size <= 0)
      return w;

    w.resize(static_cast<size_t>(size), 1.0);
    const std::string name = NormalizeWindowName(window_type);
    if (name == "hann" || name == "hanning")
    {
      if (size <= 1)
      {
        w[0] = 1.0;
      }
      else
      {
        for (int n = 0; n < size; ++n)
          w[static_cast<size_t>(n)] = 0.5 * (1.0 - std::cos(2.0 * kPi * n / (size - 1)));
      }
    }
    else if (name == "rect" || name == "rectangular")
    {
      // keep as all-ones
    }
    else
    {
      // Default to Hann for unknown window types.
      if (size <= 1)
      {
        w[0] = 1.0;
      }
      else
      {
        for (int n = 0; n < size; ++n)
          w[static_cast<size_t>(n)] = 0.5 * (1.0 - std::cos(2.0 * kPi * n / (size - 1)));
      }
    }
    return w;
  }

  int FftProcessor::ClampHop(int size, double overlap)
  {
    if (size <= 0)
      return 0;
    if (overlap < 0.0)
      overlap = 0.0;
    if (overlap >= 1.0)
      overlap = 0.99;
    const double hop_f = size * (1.0 - overlap);
    const int hop = static_cast<int>(std::max(1.0, std::round(hop_f)));
    return hop;
  }

  void FftProcessor::ResetState(State &st, const FftProcessorConfig &cfg, int num_channels)
  {
    if (st.kiss_cfg)
    {
      kiss_fftr_free(st.kiss_cfg);
      st.kiss_cfg = nullptr;
    }

    st.fft_size = cfg.size;
    st.num_channels = num_channels;
    st.sample_rate_hz = cfg.sample_rate_hz;
    st.window_type = cfg.window_type;
    st.overlap = cfg.overlap;
    st.hop = ClampHop(cfg.size, cfg.overlap);
    st.window = BuildWindow(cfg.window_type, cfg.size);
    st.buffers.assign(static_cast<size_t>(num_channels), std::vector<double>());
    st.head = 0;

    st.kiss_cfg = kiss_fftr_alloc(cfg.size, 0, NULL, NULL);
    if (!st.kiss_cfg)
      throw std::runtime_error("FftProcessor: kiss_fftr_alloc failed");
  }

  std::vector<uei::FftFrame> FftProcessor::ProcessFrame(const uei::RawFrame &frame)
  {
    std::vector<uei::FftFrame> outputs;

    const auto it_cfg = configs_.find(frame.group_name);
    if (it_cfg == configs_.end() || !it_cfg->second.active)
      return outputs;

    const FftProcessorConfig &cfg = it_cfg->second;
    if (cfg.size <= 0)
      throw std::runtime_error("FftProcessor: fft.size must be > 0");
    if (frame.num_channels <= 0)
      return outputs;

    State &st = states_[frame.group_name];

    if (st.fft_size != cfg.size ||
        st.num_channels != frame.num_channels ||
        st.sample_rate_hz != cfg.sample_rate_hz ||
        st.window_type != cfg.window_type ||
        st.overlap != cfg.overlap)
    {
      ResetState(st, cfg, frame.num_channels);
    }

    if (st.buffers.size() != static_cast<size_t>(frame.num_channels))
      return outputs;

    const int num_ch = frame.num_channels;
    const int samples = frame.samples_per_channel;
    const size_t total_in = frame.raw.size();

    for (int s = 0; s < samples; ++s)
    {
      const int base = s * num_ch;
      for (int ch = 0; ch < num_ch; ++ch)
      {
        const size_t idx = static_cast<size_t>(base + ch);
        if (idx >= total_in)
          throw std::runtime_error("FftProcessor: input index out of range");
        const double centered = static_cast<double>(frame.raw[idx]) - kFullScaleCode;
        st.buffers[static_cast<size_t>(ch)].push_back(centered);
      }
    }

    if (st.fft_size <= 0)
      return outputs;
    const int nfft = st.fft_size;
    const int bins = nfft / 2 + 1;
    const bool has_nyquist = (nfft % 2 == 0);

    while (st.buffers[0].size() >= st.head + static_cast<size_t>(nfft))
    {
      uei::FftFrame out;
      out.seq = frame.seq;
      out.slot_index = frame.slot_index;
      out.group_name = frame.group_name;
      out.fft_size = nfft;
      out.sample_rate_hz = st.sample_rate_hz;
      out.window_type = st.window_type;
      out.overlap = st.overlap;
      out.num_channels = num_ch;
      out.magnitude_db.resize(static_cast<size_t>(num_ch * bins));

      for (int ch = 0; ch < num_ch; ++ch)
      {
        std::vector<kiss_fft_scalar> in(static_cast<size_t>(nfft));
        std::vector<kiss_fft_cpx> freq(static_cast<size_t>(bins));

        const std::vector<double> &buf = st.buffers[static_cast<size_t>(ch)];
        for (int i = 0; i < nfft; ++i)
        {
          const double v = buf[st.head + static_cast<size_t>(i)] * st.window[static_cast<size_t>(i)];
          in[static_cast<size_t>(i)] = static_cast<kiss_fft_scalar>(v);
        }

        kiss_fftr(static_cast<kiss_fftr_cfg>(st.kiss_cfg), in.data(), freq.data());

        const double inv_n = 1.0 / static_cast<double>(nfft);
        for (int k = 0; k < bins; ++k)
        {
          const double re = freq[static_cast<size_t>(k)].r;
          const double im = freq[static_cast<size_t>(k)].i;
          const double mag = std::sqrt(re * re + im * im);
          const bool is_dc = (k == 0);
          const bool is_nyq = (has_nyquist && k == bins - 1);
          const double scale = (is_dc || is_nyq) ? inv_n : (2.0 * inv_n);
          const double amp = mag * scale;
          const double ratio = std::max(amp / kFullScaleCode, kMinRatio);
          const double db = 20.0 * std::log10(ratio);
          const size_t out_idx = static_cast<size_t>(ch * bins + k);
          out.magnitude_db[out_idx] = db;
        }
      }

      outputs.push_back(std::move(out));
      st.head += static_cast<size_t>(st.hop);
    }

    if (st.head > static_cast<size_t>(nfft * 4))
    {
      for (auto &buf : st.buffers)
      {
        if (st.head < buf.size())
          buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(st.head));
        else
          buf.clear();
      }
      st.head = 0;
    }

    return outputs;
  }

} // namespace uei::dsp
