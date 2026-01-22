#include "daq/SimDaqDevice.hpp"

#include <chrono>
#include <thread>
#include <stdexcept>
#include <cmath>
#include <cstdlib>
#include <ctime>

namespace uei
{

  SimDaqDevice::SimDaqDevice(const Params &p)
      : params_(p)
  {
  }

  void SimDaqDevice::Open()
  {
    if (params_.channels.empty())
      throw std::runtime_error("SimDaqDevice: channels empty");
    if (params_.sample_rate_hz <= 0.0)
      throw std::runtime_error("SimDaqDevice: sample_rate_hz must be > 0");
    if (params_.samples_per_channel <= 0)
      throw std::runtime_error("SimDaqDevice: samples_per_channel must be > 0");

    static bool seeded = false;
    if (!seeded)
    {
      std::srand(static_cast<unsigned int>(std::time(nullptr)));
      seeded = true;
    }

    running_ = false;
    seq_ = 0;
    t_ = 0.0;
  }

  void SimDaqDevice::Start()
  {
    running_ = true;
  }

  void SimDaqDevice::Stop()
  {
    running_ = false;
  }

  void SimDaqDevice::Close()
  {
    running_ = false;
  }

  int32_t SimDaqDevice::VoltToCode(double volt)
  {
    // clamp to +/-10V
    if (volt > 10.0)
      volt = 10.0;
    else if (volt < -10.0)
      volt = -10.0;

    // inverse of convert_ai217_raw_to_volt: code in [0, 0xFFFFFF]
    const double code = ((volt / 10.0) * 8388608.0) + 8388608.0;
    const int32_t c = static_cast<int32_t>(std::llround(code));
    if (c < 0)
      return 0;
    if (c > 0xFFFFFF)
      return 0xFFFFFF;
    return c;
  }

  bool SimDaqDevice::ReadFrame(RawFrame &out)
  {
    if (!running_)
      return false;

    const int num_ch = static_cast<int>(params_.channels.size());
    const int spc = params_.samples_per_channel;
    const double dt = 1.0 / params_.sample_rate_hz;

    out.seq = seq_++;
    out.slot_index = params_.slot_index;
    out.group_name = params_.group_name;
    out.samples_per_channel = spc;
    out.num_channels = num_ch;
    out.raw.resize(static_cast<size_t>(spc * num_ch));

    const double noise_amp = params_.amplitude * params_.noise_percent * 0.01;
    constexpr double kPi = 3.14159265358979323846;

    const bool is_ai211 = (params_.board_name == "DNA-AI-211");
    const bool is_ai217 = (params_.board_name.empty() || params_.board_name == "DNA-AI-217");

    for (int s = 0; s < spc; ++s)
    {
      const double t = t_ + dt * s;
      const int base = s * num_ch;
      for (int ch_idx = 0; ch_idx < num_ch; ++ch_idx)
      {
        const int logical_ch = params_.channels[static_cast<size_t>(ch_idx)];
        int base_idx = logical_ch;
        if (is_ai211)
        {
          if (logical_ch == 2)
            base_idx = 0;
          else if (logical_ch == 3)
            base_idx = 1;
        }
        else if (is_ai217)
        {
          if (logical_ch == 6)
            base_idx = 0;
          else if (logical_ch == 7)
            base_idx = 1;
        }

        const double freq = params_.base_frequency * (1.0 + params_.frequency_step_percent * 0.01 * base_idx);
        const double noise = noise_amp * ((static_cast<double>(std::rand()) / RAND_MAX) * 2.0 - 1.0);
        const double v = params_.amplitude * std::sin(2.0 * kPi * freq * t) + noise;
        out.raw[static_cast<size_t>(base + ch_idx)] = VoltToCode(v);
      }
    }

    t_ += dt * spc;

    if (seq_ % 1 == 0)
    {
      printf("[SimDaq] slot=%d seq=%u samples=%d num_ch=%d\\n\r",
             params_.slot_index, out.seq, spc, num_ch);
      fflush(stdout);
    }

    // pace to roughly align with real time
    const auto frame_period = std::chrono::duration<double>(dt * spc);
    std::this_thread::sleep_for(frame_period);
    return true;
  }

} // namespace uei
