#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "utils/UeiStructs.hpp"

namespace uei::dsp
{

  struct FftProcessorConfig
  {
    bool active{false};
    int size{1024};
    std::string window_type{"hann"};
    double overlap{0.5};
    double sample_rate_hz{0.0};
  };

  class FftProcessor
  {
  public:
    explicit FftProcessor(const std::unordered_map<std::string, FftProcessorConfig> &configs);
    ~FftProcessor();

    /**
     * @brief Push time-domain samples and emit zero or more FFT frames.
     * @param frame Input RawFrame (time-domain, scan-major).
     * @return One or more FFT frames, one per window.
     */
    std::vector<uei::FftFrame> ProcessFrame(const uei::RawFrame &frame);

  private:
    struct State
    {
      int fft_size{0};
      int num_channels{0};
      double sample_rate_hz{0.0};
      std::string window_type;
      double overlap{0.0};
      int hop{0};
      std::vector<double> window;
      std::vector<std::vector<double>> buffers;
      size_t head{0};
      void *kiss_cfg{nullptr};
    };

    std::unordered_map<std::string, FftProcessorConfig> configs_;
    std::unordered_map<std::string, State> states_;

    static std::vector<double> BuildWindow(const std::string &window_type, int size);
    static int ClampHop(int size, double overlap);
    static std::string NormalizeWindowName(const std::string &window_type);
    void ResetState(State &st, const FftProcessorConfig &cfg, int num_channels);
  };

} // namespace uei::dsp
