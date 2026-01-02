#pragma once

#include <string>
#include "utils/UeiStructs.hpp"

namespace uei {

/**
 * @brief CSV-like packetizer for MVP (Scheme A).
 *
 * Format:
 *   D,1,<seq>,<slot_index>,<group_name>,<samples_per_channel>,<raw...>
 *
 * raw layout: scan-major interleaved (scan0 ch0..chN-1, scan1..., ...)
 */
class CsvPacketizer {
public:
  /**
   * @brief Encode a RawFrame into a single-line CSV-like string.
   * @param frame RawFrame data.
   * @return Encoded string (no trailing newline).
   */
  static std::string Encode(const RawFrame& frame);
};

}  // namespace uei
