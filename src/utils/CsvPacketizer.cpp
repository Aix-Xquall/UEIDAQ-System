#include "utils/CsvPacketizer.hpp"

#include <iomanip>
#include <sstream>

namespace uei {

std::string CsvPacketizer::Encode(const RawFrame& frame) {
  std::ostringstream oss;

  // D,1,<seq>,<slot_index>,<group_name>,<samples_per_channel>,<raw...>
  oss << "D,1,"
      << frame.seq << ","
      << frame.slot_index << ","
      << frame.group_name << ","
      << frame.samples_per_channel;

  for (const auto v : frame.raw) {
    oss << "," << v;
  }

  return oss.str();
}

std::string CsvPacketizer::Encode(const FftFrame& frame) {
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss << std::setprecision(6);

  const int bins = frame.fft_size / 2 + 1;

  // F,1,<seq>,<slot_index>,<group_name>,<fft_size>,<bins>,<sample_rate_hz>,<window_type>,<overlap>,<num_channels>,<mag_db...>
  oss << "F,1,"
      << frame.seq << ","
      << frame.slot_index << ","
      << frame.group_name << ","
      << frame.fft_size << ","
      << bins << ","
      << frame.sample_rate_hz << ","
      << frame.window_type << ","
      << frame.overlap << ","
      << frame.num_channels;

  for (const auto v : frame.magnitude_db) {
    oss << "," << v;
  }

  return oss.str();
}

}  // namespace uei
