#include "udp.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstring>

namespace wf {

namespace {

#ifdef _WIN32
typedef SOCKET NativeSocket;
const NativeSocket kNativeInvalid = INVALID_SOCKET;
void CloseNative(NativeSocket s) { closesocket(s); }
bool WouldBlock() {
  int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAECONNRESET;  // ICMP port unreachable surfaces as RESET
}
#else
typedef int NativeSocket;
const NativeSocket kNativeInvalid = -1;
void CloseNative(NativeSocket s) { close(s); }
bool WouldBlock() {
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNREFUSED;
}
#endif

NativeSocket ToNative(intptr_t h) { return static_cast<NativeSocket>(h); }

}  // namespace

std::string UdpEndpoint::IpString() const {
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&addr);
  return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
         std::to_string(b[3]);
}

bool ResolveIPv4(const std::string& host, uint16_t port, UdpEndpoint* out) {
  if (host.empty()) return false;
  in_addr a;
  if (inet_pton(AF_INET, host.c_str(), &a) == 1) {
    out->addr = a.s_addr;
    out->port = port;
    return true;
  }
  addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
  out->addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
  out->port = port;
  freeaddrinfo(res);
  return true;
}

UdpEndpoint BroadcastEndpoint(uint16_t port) {
  UdpEndpoint e;
  e.addr = htonl(INADDR_BROADCAST);
  e.port = port;
  return e;
}

bool UdpSocket::Open(bool broadcast, std::string* error, uint16_t port) {
  Close();
  NativeSocket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kNativeInvalid) {
    if (error) *error = "socket() failed";
    return false;
  }
  if (broadcast) {
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
  }
  // A scene recall makes the console push a burst of changes; give the kernel room to hold them.
  int rcvbuf = 512 * 1024;
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

  sockaddr_in local;
  std::memset(&local, 0, sizeof(local));
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(port);
  if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    if (error) *error = "bind() failed";
    CloseNative(s);
    return false;
  }
#ifdef _WIN32
  u_long nb = 1;
  ioctlsocket(s, FIONBIO, &nb);
#else
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
  fd_ = static_cast<intptr_t>(s);
  return true;
}

uint16_t UdpSocket::LocalPort() const {
  if (!IsOpen()) return 0;
  sockaddr_in sa;
  socklen_t len = sizeof(sa);
  if (getsockname(ToNative(fd_), reinterpret_cast<sockaddr*>(&sa), &len) != 0) return 0;
  return ntohs(sa.sin_port);
}

void UdpSocket::Close() {
  if (fd_ != kInvalid) {
    CloseNative(ToNative(fd_));
    fd_ = kInvalid;
  }
}

bool UdpSocket::SendTo(const UdpEndpoint& to, const void* data, size_t len) {
  if (!IsOpen()) return false;
  sockaddr_in sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = to.addr;
  sa.sin_port = htons(to.port);
  return sendto(ToNative(fd_), static_cast<const char*>(data), static_cast<int>(len), 0,
                reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == static_cast<int>(len);
}

int UdpSocket::RecvFrom(void* buf, size_t cap, UdpEndpoint* from) {
  if (!IsOpen()) return -1;
  sockaddr_in sa;
  socklen_t salen = sizeof(sa);
  int n = static_cast<int>(recvfrom(ToNative(fd_), static_cast<char*>(buf), static_cast<int>(cap), 0,
                                    reinterpret_cast<sockaddr*>(&sa), &salen));
  if (n < 0) return WouldBlock() ? 0 : -1;
  if (from) {
    from->addr = sa.sin_addr.s_addr;
    from->port = ntohs(sa.sin_port);
  }
  return n;
}

void WaitReadable(UdpSocket* const* sockets, int count, int timeoutMs) {
  fd_set rfds;
  FD_ZERO(&rfds);
  NativeSocket maxfd = 0;
  bool any = false;
  for (int i = 0; i < count; i++) {
    if (!sockets[i] || !sockets[i]->IsOpen()) continue;
    NativeSocket s = ToNative(sockets[i]->Handle());
    FD_SET(s, &rfds);
    if (s > maxfd) maxfd = s;
    any = true;
  }
  timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  if (!any) {
#ifdef _WIN32
    Sleep(timeoutMs);  // Winsock select() rejects empty sets
#else
    select(0, nullptr, nullptr, nullptr, &tv);
#endif
    return;
  }
  select(static_cast<int>(maxfd + 1), &rfds, nullptr, nullptr, &tv);
}

bool NetInit() {
#ifdef _WIN32
  WSADATA wsa;
  return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
  return true;
#endif
}

void NetShutdown() {
#ifdef _WIN32
  WSACleanup();
#endif
}

}  // namespace wf
