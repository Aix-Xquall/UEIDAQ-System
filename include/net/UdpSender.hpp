#pragma once

#include <cstdint>
#include <string>
#include <netinet/in.h>   // sockaddr_in

namespace uei {

/**
 * @brief UDP sender wrapper (POSIX).
 *
 * MVP: IPv4 only.
 */
class UdpSender {
public:
  /** @brief Constructor. */
  UdpSender();

  /** @brief Destructor (auto closes). */
  ~UdpSender();

  UdpSender(const UdpSender&) = delete;
  UdpSender& operator=(const UdpSender&) = delete;

  /**
   * @brief Open UDP socket and set destination.
   * @param ip Destination IPv4 string.
   * @param port Destination port.
   * @throws std::runtime_error on errors.
   */
  void Open(const std::string& ip, uint16_t port);

  /**
   * @brief Send payload to configured destination.
   * @param data Payload bytes.
   * @param size Payload size.
   * @return true if sent successfully, false otherwise.
   */
  bool Send(const void* data, std::size_t size);

  /** @brief Close socket. Safe to call multiple times. */
  void Close();

private:
  int sock_{-1};
  bool ready_{false};
  ::sockaddr_in addr_{};   ///< destination address (IPv4)
};

}  // namespace uei
