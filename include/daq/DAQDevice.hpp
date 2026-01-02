#pragma once

#include <string>
#include "utils/UeiStructs.hpp"

namespace uei {

/**
 * @brief Abstract DAQ device interface.
 */
class DAQDevice {
public:
  virtual ~DAQDevice() = default;

  /**
   * @brief Open device resources (library init, open IOM, create VMAP, etc).
   * @throws std::runtime_error on failure.
   */
  virtual void Open() = 0;

  /** @brief Start acquisition (start VMAP + trigger). */
  virtual void Start() = 0;

  /** @brief Stop acquisition. Safe to call multiple times. */
  virtual void Stop() = 0;

  /** @brief Close and cleanup resources. Safe to call multiple times. */
  virtual void Close() = 0;

  /**
   * @brief Read one block into frame (one refresh worth of data).
   * @param out Output frame (raw scan-major interleaved).
   * @return true if data produced, false if should stop.
   */
  virtual bool ReadFrame(RawFrame& out) = 0;
};

}  // namespace uei
