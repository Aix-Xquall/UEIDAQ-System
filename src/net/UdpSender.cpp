#include "net/UdpSender.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace uei {

UdpSender::UdpSender() = default;

UdpSender::~UdpSender() {
  Close();
}

void UdpSender::Open(const std::string& ip, uint16_t port) {
  Close();

  sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (sock_ < 0) {
    const int err = errno;
#ifdef SIM_BUILD
    std::fprintf(stderr, "[WARN] UDP socket() failed: %s (SIM_BUILD no-op send)\n", std::strerror(err));
    ready_ = false;
    return;
#else
    throw std::runtime_error("socket() failed: " + std::string(std::strerror(err)));
#endif
  }

  std::memset(&addr_, 0, sizeof(addr_));
  addr_.sin_family = AF_INET;
  addr_.sin_port = htons(port);

  if (::inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) != 1) {
    Close();
    throw std::runtime_error("inet_pton() failed for ip=" + ip);
  }

  ready_ = true;
#ifdef SIM_BUILD
  std::fprintf(stdout, "[SimUdp] target=%s:%u (socket ok)\n", ip.c_str(), static_cast<unsigned>(port));
#endif
}

bool UdpSender::Send(const void* data, std::size_t size) {
  if (!ready_ || sock_ < 0) {
#ifdef SIM_BUILD
    return true; // no-op in simulation mode if socket unavailable
#else
    return false;
#endif
  }

  const auto sent = ::sendto(
      sock_,
      data,
      size,
      0,
      reinterpret_cast<const ::sockaddr*>(&addr_),
      sizeof(addr_));

  return (sent == static_cast<ssize_t>(size));
}

void UdpSender::Close() {
  ready_ = false;
  if (sock_ >= 0) {
    ::close(sock_);
    sock_ = -1;
  }
  std::memset(&addr_, 0, sizeof(addr_));
}

}  // namespace uei
