// Tiny non-blocking UDP socket wrapper over Winsock / BSD sockets.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace wf {

struct UdpEndpoint {
  uint32_t addr = 0;  // IPv4, network byte order
  uint16_t port = 0;  // host byte order
  std::string IpString() const;
};

// Resolves "192.168.1.10" (or a hostname, blocking) to an IPv4 endpoint.
bool ResolveIPv4(const std::string& host, uint16_t port, UdpEndpoint* out);
UdpEndpoint BroadcastEndpoint(uint16_t port);

class UdpSocket {
 public:
  UdpSocket() = default;
  ~UdpSocket() { Close(); }
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  // Binds 0.0.0.0:<port> (0 = ephemeral).
  bool Open(bool broadcast, std::string* error, uint16_t port = 0);
  uint16_t LocalPort() const;
  void Close();
  bool IsOpen() const { return fd_ != kInvalid; }

  bool SendTo(const UdpEndpoint& to, const void* data, size_t len);
  // Returns bytes received, 0 if nothing is pending, -1 on error.
  int RecvFrom(void* buf, size_t cap, UdpEndpoint* from);

  static constexpr intptr_t kInvalid = -1;
  intptr_t Handle() const { return fd_; }

 private:
  intptr_t fd_ = kInvalid;
};

// Waits up to timeoutMs for any of the (open) sockets to become readable.
void WaitReadable(UdpSocket* const* sockets, int count, int timeoutMs);

// Call once per process before using sockets (Winsock); no-op elsewhere.
bool NetInit();
void NetShutdown();

}  // namespace wf
