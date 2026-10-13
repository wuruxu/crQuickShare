// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/transport_channel.h"
#include "crQuickShare/connection_listener.h"
#include "base/files/scoped_file.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <algorithm>
#include <utility>

namespace crquickshare {
namespace {
class TcpChannel final : public TransportChannel {
 public:
  explicit TcpChannel(base::ScopedFD socket);
  ~TcpChannel() override;
  Result Read(base::span<char> bytes) override;
  Result Write(base::span<const char> bytes) override;
  Ready Wait(bool read, bool write, int timeout_ms) override;
  void Close() override;
  const char* name() const override;
 private:
  base::ScopedFD socket_;
};
TransportChannel::Result IoResult(ssize_t count) {
  if (count > 0) return {TransportChannel::Status::kOk, static_cast<size_t>(count), 0};
  if (count == 0) return {TransportChannel::Status::kClosed, 0, 0};
  const int error = errno;
  if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR)
    return {TransportChannel::Status::kWouldBlock, 0, 0};
  return {TransportChannel::Status::kError, 0, error};
}
TcpChannel::TcpChannel(base::ScopedFD socket) : socket_(std::move(socket)) {}
TcpChannel::~TcpChannel() = default;
TransportChannel::Result TcpChannel::Read(base::span<char> bytes) {
  if (bytes.empty()) return {Status::kOk, 0, 0};
  return IoResult(recv(socket_.get(), bytes.data(), bytes.size(), 0));
}
TransportChannel::Result TcpChannel::Write(base::span<const char> bytes) {
  if (bytes.empty()) return {Status::kOk, 0, 0};
  return IoResult(send(socket_.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL));
}
TransportChannel::Ready TcpChannel::Wait(bool read, bool write, int timeout_ms) {
  if (!socket_.is_valid()) return {false, false, EBADF};
  pollfd descriptor{socket_.get(), static_cast<short>((read ? POLLIN : 0) | (write ? POLLOUT : 0)), 0};
  const int result = poll(&descriptor, 1, std::max(0, timeout_ms));
  if (result < 0) return {false, false, errno == EINTR ? 0 : errno};
  if (!result) return {};
  if (descriptor.revents & POLLNVAL) return {false, false, EBADF};
  const bool terminal = descriptor.revents & (POLLHUP | POLLERR);
  return {read && ((descriptor.revents & POLLIN) || terminal),
          write && ((descriptor.revents & POLLOUT) || terminal), 0};
}
void TcpChannel::Close() { socket_.reset(); }
const char* TcpChannel::name() const { return "WIFI_LAN"; }
}  // namespace
std::unique_ptr<TransportChannel> AdoptTcpChannel(base::ScopedFD socket) {
  if (!socket.is_valid()) return nullptr;
  const int flags = fcntl(socket.get(), F_GETFL);
  if (flags < 0 || fcntl(socket.get(), F_SETFL, flags | O_NONBLOCK) < 0) return nullptr;
  return std::make_unique<TcpChannel>(std::move(socket));
}
ChannelConnectResult ConnectTcpChannel(const std::string& ip, uint16_t port,
                                      base::TimeTicks deadline,
                                      const std::function<bool()>& active,
                                      const std::string& local_address, unsigned interface_index) {
  if (!active()) return {nullptr, "cancelled", 0};
  if (base::TimeTicks::Now() >= deadline) return {nullptr, "timed_out", 0};
  base::ScopedFD socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (!socket.is_valid()) return {nullptr, "socket_failed", errno};
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) return {nullptr, "invalid_address", 0};
  if (interface_index || !local_address.empty()) {
    sockaddr_in source{}; source.sin_family = AF_INET;
    if (!interface_index || inet_pton(AF_INET, local_address.c_str(), &source.sin_addr) != 1)
      return {nullptr, "invalid_source_route", 0};
    ifaddrs* raw = nullptr;
    if (getifaddrs(&raw)) return {nullptr, "interface_check_failed", errno};
    bool present = false;
    for (auto* link = raw; link; link = link->ifa_next) {
      if (link->ifa_addr && link->ifa_netmask && link->ifa_addr->sa_family == AF_INET && (link->ifa_flags & IFF_UP) &&
          if_nametoindex(link->ifa_name) == interface_index &&
          reinterpret_cast<const sockaddr_in*>(link->ifa_addr)->sin_addr.s_addr == source.sin_addr.s_addr &&
          ((address.sin_addr.s_addr ^ source.sin_addr.s_addr) &
           reinterpret_cast<const sockaddr_in*>(link->ifa_netmask)->sin_addr.s_addr) == 0)
        present = true;
    }
    freeifaddrs(raw);
    if (!present) return {nullptr, "source_route_unavailable", 0};
    const uint32_t index = htonl(interface_index);
    if (setsockopt(socket.get(), IPPROTO_IP, IP_UNICAST_IF, &index, sizeof(index)))
      return {nullptr, "interface_bind_failed", errno};
    if (bind(socket.get(), reinterpret_cast<sockaddr*>(&source), sizeof(source)))
      return {nullptr, "source_bind_failed", errno};
  }
  bool connected = connect(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  if (!connected && errno != EINPROGRESS) return {nullptr, "connect_failed", errno};
  while (!connected && active() && base::TimeTicks::Now() < deadline) {
    pollfd descriptor{socket.get(), POLLOUT, 0};
    const int wait_ms = static_cast<int>(std::clamp<int64_t>((deadline - base::TimeTicks::Now()).InMilliseconds(), 0, 100));
    const int result = poll(&descriptor, 1, wait_ms);
    if (result < 0 && errno != EINTR) return {nullptr, "connect_failed", errno};
    if (result <= 0) continue;
    if (descriptor.revents & POLLNVAL) return {nullptr, "connect_failed", EBADF};
    int error = 0; socklen_t size = sizeof(error);
    if (getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &error, &size) != 0) return {nullptr, "connect_failed", errno};
    if (error) return {nullptr, "connect_failed", error};
    connected = true;
  }
  if (!active()) return {nullptr, "cancelled", 0};
  if (!connected) return {nullptr, "timed_out", 0};
  auto channel = AdoptTcpChannel(std::move(socket));
  if (!channel) return {nullptr, "socket_failed", errno};
  return {std::move(channel), "connected", 0};
}
namespace {
class TcpListener final : public ConnectionListener {
 public:
  TcpListener(base::ScopedFD socket, uint16_t port);
  ~TcpListener() override;
  uint16_t port() const override;
  Accepted Accept(int timeout_ms) override;
 private:
  base::ScopedFD socket_;
  uint16_t port_;
};
TcpListener::TcpListener(base::ScopedFD socket, uint16_t port)
    : socket_(std::move(socket)), port_(port) {}
TcpListener::~TcpListener() = default;
uint16_t TcpListener::port() const { return port_; }
ConnectionListener::Accepted TcpListener::Accept(int timeout_ms) {
  pollfd descriptor{socket_.get(), POLLIN, 0};
  const int ready = poll(&descriptor, 1, std::max(0, timeout_ms));
  if (ready < 0 && errno != EINTR) return {nullptr, {}, "listener_poll_failed", errno};
  if (ready <= 0) return {};
  sockaddr_in peer{}; socklen_t length = sizeof(peer);
  base::ScopedFD accepted(accept4(socket_.get(), reinterpret_cast<sockaddr*>(&peer), &length, SOCK_CLOEXEC | SOCK_NONBLOCK));
  if (!accepted.is_valid()) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return {};
    return {nullptr, {}, "accept_failed", errno};
  }
  char ip[INET_ADDRSTRLEN];
  if (!inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip))) return {nullptr, {}, "peer_address_failed", errno};
  auto channel = AdoptTcpChannel(std::move(accepted));
  if (!channel) return {nullptr, {}, "channel_setup_failed", errno};
  return {std::move(channel), ip, nullptr, 0};
}
}  // namespace
ListenerResult CreateConnectionListener() {
  base::ScopedFD socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  sockaddr_in address{}; address.sin_family = AF_INET;
  if (!socket.is_valid() || bind(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(socket.get(), 4))
    return {nullptr, "listener_setup_failed", errno};
  socklen_t size = sizeof(address);
  if (getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address), &size))
    return {nullptr, "listener_address_failed", errno};
  return {std::make_unique<TcpListener>(std::move(socket), ntohs(address.sin_port)), nullptr, 0};
}
}  // namespace crquickshare
