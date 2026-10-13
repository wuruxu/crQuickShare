// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/transport_channel.h"
#include "crQuickShare/connection_listener.h"
#include "crQuickShare/platform_socket.h"
#include <algorithm>
#include <utility>

namespace crquickshare {
namespace {
#if BUILDFLAG(IS_WIN)
constexpr int kBadSocket = WSAENOTSOCK;
#else
constexpr int kBadSocket = EBADF;
#endif
class TcpChannel final : public TransportChannel {
 public:
  explicit TcpChannel(platform::Socket socket);
  ~TcpChannel() override;
  Result Read(base::span<char> bytes) override;
  Result Write(base::span<const char> bytes) override;
  Ready Wait(bool read, bool write, int timeout_ms) override;
  void Close() override;
  const char* name() const override;
 private:
  platform::Socket socket_;
};
TransportChannel::Result IoResult(int count) {
  if (count > 0) return {TransportChannel::Status::kOk, static_cast<size_t>(count), 0};
  if (count == 0) return {TransportChannel::Status::kClosed, 0, 0};
  const int error = platform::Error();
  if (platform::Pending(error))
    return {TransportChannel::Status::kWouldBlock, 0, 0};
  return {TransportChannel::Status::kError, 0, error};
}
TcpChannel::TcpChannel(platform::Socket socket) : socket_(std::move(socket)) {}
TcpChannel::~TcpChannel() = default;
TransportChannel::Result TcpChannel::Read(base::span<char> bytes) {
  if (bytes.empty()) return {Status::kOk, 0, 0};
  return IoResult(platform::Read(socket_.get(), bytes));
}
TransportChannel::Result TcpChannel::Write(base::span<const char> bytes) {
  if (bytes.empty()) return {Status::kOk, 0, 0};
  return IoResult(platform::Write(socket_.get(), bytes));
}
TransportChannel::Ready TcpChannel::Wait(bool read, bool write, int timeout_ms) {
  if (!socket_.is_valid()) return {false, false, kBadSocket};
  platform::PollFd descriptor{socket_.get(), static_cast<short>((read ? POLLIN : 0) | (write ? POLLOUT : 0)), 0};
  const int result = platform::Poll(descriptor, std::max(0, timeout_ms));
  if (result < 0) return {false, false, platform::Interrupted(platform::Error()) ? 0 : platform::Error()};
  if (!result) return {};
  if (descriptor.revents & POLLNVAL) return {false, false, kBadSocket};
  const bool terminal = descriptor.revents & (POLLHUP | POLLERR);
  return {read && ((descriptor.revents & POLLIN) || terminal),
          write && ((descriptor.revents & POLLOUT) || terminal), 0};
}
void TcpChannel::Close() { socket_.reset(); }
const char* TcpChannel::name() const { return "WIFI_LAN"; }
}  // namespace
std::unique_ptr<TransportChannel> AdoptTcpChannel(platform::Socket socket) {
  if (!socket.is_valid()) return nullptr;
  if (!platform::Configure(socket.get())) return nullptr;
  return std::make_unique<TcpChannel>(std::move(socket));
}
ChannelConnectResult ConnectTcpChannel(const std::string& ip, uint16_t port,
                                      base::TimeTicks deadline,
                                      const std::function<bool()>& active) {
  if (!active()) return {nullptr, "cancelled", 0};
  if (base::TimeTicks::Now() >= deadline) return {nullptr, "timed_out", 0};
  auto socket = platform::Open(SOCK_STREAM);
  if (!socket.is_valid()) return {nullptr, "socket_failed", platform::Error()};
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) return {nullptr, "invalid_address", 0};
  bool connected = connect(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  if (!connected && !platform::Pending(platform::Error())) return {nullptr, "connect_failed", platform::Error()};
  while (!connected && active() && base::TimeTicks::Now() < deadline) {
    platform::PollFd descriptor{socket.get(), POLLOUT, 0};
    const int wait_ms = static_cast<int>(std::clamp<int64_t>((deadline - base::TimeTicks::Now()).InMilliseconds(), 0, 100));
    const int result = platform::Poll(descriptor, wait_ms);
    if (result < 0 && !platform::Interrupted(platform::Error())) return {nullptr, "connect_failed", platform::Error()};
    if (result <= 0) continue;
    if (descriptor.revents & POLLNVAL) return {nullptr, "connect_failed", kBadSocket};
    const int error = platform::SocketFailure(socket.get());
    if (error) return {nullptr, "connect_failed", error};
    connected = true;
  }
  if (!active()) return {nullptr, "cancelled", 0};
  if (!connected) return {nullptr, "timed_out", 0};
  auto channel = AdoptTcpChannel(std::move(socket));
  if (!channel) return {nullptr, "socket_failed", platform::Error()};
  return {std::move(channel), "connected", 0};
}
namespace {
class TcpListener final : public ConnectionListener {
 public:
  TcpListener(platform::Socket socket, uint16_t port);
  ~TcpListener() override;
  uint16_t port() const override;
  Accepted Accept(int timeout_ms) override;
 private:
  platform::Socket socket_;
  uint16_t port_;
};
TcpListener::TcpListener(platform::Socket socket, uint16_t port)
    : socket_(std::move(socket)), port_(port) {}
TcpListener::~TcpListener() = default;
uint16_t TcpListener::port() const { return port_; }
ConnectionListener::Accepted TcpListener::Accept(int timeout_ms) {
  platform::PollFd descriptor{socket_.get(), POLLIN, 0};
  const int ready = platform::Poll(descriptor, std::max(0, timeout_ms));
  if (ready < 0 && !platform::Interrupted(platform::Error())) return {nullptr, {}, "listener_poll_failed", platform::Error()};
  if (ready <= 0) return {};
  sockaddr_in peer{}; platform::Length length = sizeof(peer);
  platform::Socket accepted(accept(socket_.get(), reinterpret_cast<sockaddr*>(&peer), &length));
  if (!accepted.is_valid()) {
    if (platform::Pending(platform::Error())) return {};
    return {nullptr, {}, "accept_failed", platform::Error()};
  }
  char ip[INET_ADDRSTRLEN];
  if (!inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip))) return {nullptr, {}, "peer_address_failed", platform::Error()};
  auto channel = AdoptTcpChannel(std::move(accepted));
  if (!channel) return {nullptr, {}, "channel_setup_failed", platform::Error()};
  return {std::move(channel), ip, nullptr, 0};
}
}  // namespace
ListenerResult CreateConnectionListener() {
  auto socket = platform::Open(SOCK_STREAM);
  sockaddr_in address{}; address.sin_family = AF_INET;
  if (!socket.is_valid() || bind(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(socket.get(), 4))
    return {nullptr, "listener_setup_failed", platform::Error()};
  platform::Length size = sizeof(address);
  if (getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address), &size))
    return {nullptr, "listener_address_failed", platform::Error()};
  return {std::make_unique<TcpListener>(std::move(socket), ntohs(address.sin_port)), nullptr, 0};
}
}  // namespace crquickshare
