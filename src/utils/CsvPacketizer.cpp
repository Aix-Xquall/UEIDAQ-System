#include "utils/CsvPacketizer.hpp"

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

}  // namespace uei
