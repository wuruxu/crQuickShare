// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/transport_channel.h"
#include "crQuickShare/connection_listener.h"
#include "crQuickShare/platform_socket.h"
#include <algorithm>
#include <utility>
#include "net/base/ip_endpoint.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include <iphlpapi.h>
#endif

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
                                      const std::function<bool()>& active,
                                      const std::string& local_address, unsigned interface_index) {
  if (!active()) return {nullptr, "cancelled", 0};
  if (base::TimeTicks::Now() >= deadline) return {nullptr, "timed_out", 0};
  const auto peer = net::IPAddress::FromIPLiteral(ip);
  if (!peer || ip.find('\0') != std::string::npos || peer->IsIPv4MappedIPv6())
    return {nullptr, "invalid_address", 0};
  const bool ipv6 = peer->IsIPv6();
  if (ipv6 && peer->IsLinkLocal() && !interface_index) return {nullptr, "invalid_source_route", 0};
  const net::IPEndPoint endpoint(*peer, port, ipv6 && peer->IsLinkLocal() ?
      std::optional<uint32_t>(interface_index) : std::nullopt);
  sockaddr_storage address{}; platform::Length address_size = sizeof(address);
  if (!endpoint.ToSockAddr(reinterpret_cast<sockaddr*>(&address), &address_size))
    return {nullptr, "invalid_address", 0};
  auto socket = platform::Open(SOCK_STREAM, ipv6 ? AF_INET6 : AF_INET);
  if (!socket.is_valid()) return {nullptr, "socket_failed", platform::Error()};
  if (interface_index || !local_address.empty()) {
    const auto source_ip = net::IPAddress::FromIPLiteral(local_address);
    if (!interface_index || !source_ip || local_address.find('\0') != std::string::npos || source_ip->size() != peer->size())
      return {nullptr, "invalid_source_route", 0};
    const net::IPEndPoint source_endpoint(*source_ip, 0, ipv6 && source_ip->IsLinkLocal() ?
        std::optional<uint32_t>(interface_index) : std::nullopt);
    sockaddr_storage source{}; platform::Length source_size = sizeof(source);
    if (!source_endpoint.ToSockAddr(reinterpret_cast<sockaddr*>(&source), &source_size))
      return {nullptr, "invalid_source_route", 0};
    if (ipv6) {
      if (!IsTcpRouteAvailable(ip, local_address, interface_index)) return {nullptr, "source_route_unavailable", 0};
    } else {
      const auto links = platform::Interfaces();
      if (!links) return {nullptr, "interface_check_failed", platform::Error()};
      const auto& remote = reinterpret_cast<const sockaddr_in&>(address);
      const auto& local = reinterpret_cast<const sockaddr_in&>(source);
      if (std::ranges::none_of(*links, [&](const auto& link) {
            return link.index == interface_index && link.address.s_addr == local.sin_addr.s_addr &&
                   ((remote.sin_addr.s_addr ^ local.sin_addr.s_addr) & link.mask.s_addr) == 0;
          })) return {nullptr, "source_route_unavailable", 0};
    }
#if BUILDFLAG(IS_WIN)
    const uint32_t index = ipv6 ? interface_index : htonl(interface_index);
    const int option = ipv6 ? IPV6_UNICAST_IF : IP_UNICAST_IF;
#else
    const uint32_t index = interface_index;
    const int option = ipv6 ? IPV6_BOUND_IF : IP_BOUND_IF;
#endif
    if (platform::SetOption(socket.get(), ipv6 ? IPPROTO_IPV6 : IPPROTO_IP, option, &index, sizeof(index)))
      return {nullptr, "interface_bind_failed", platform::Error()};
    if (bind(socket.get(), reinterpret_cast<sockaddr*>(&source), source_size))
      return {nullptr, "source_bind_failed", platform::Error()};
  }
  bool connected = connect(socket.get(), reinterpret_cast<sockaddr*>(&address), address_size) == 0;
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
  if (!connected || base::TimeTicks::Now() >= deadline) return {nullptr, "timed_out", 0};
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
bool IsPhysicalLanInterface(const std::string& name, unsigned interface_index) {
#if BUILDFLAG(IS_WIN)
  MIB_IF_ROW2 row{};
  row.InterfaceIndex = interface_index;
  return interface_index && GetIfEntry2(&row) == NO_ERROR &&
      row.OperStatus == IfOperStatusUp && row.InterfaceAndOperStatusFlags.HardwareInterface &&
      !row.InterfaceAndOperStatusFlags.FilterInterface && !row.InterfaceAndOperStatusFlags.EndPointInterface;
#else
  return interface_index && name.starts_with("en");
#endif
}
ListenerResult CreateBoundLanUpgradeListener(const TcpRoute& route) {
  const auto source = net::IPAddress::FromIPLiteral(route.local_address);
  if (!source || !source->IsIPv4() || !route.interface_index)
    return {nullptr, "invalid_lan_listener_route", 0};
  auto socket = platform::Open(SOCK_STREAM);
  if (!socket.is_valid()) return {nullptr, "lan_listener_setup_failed", platform::Error()};
  sockaddr_in address{};
  platform::Length size = sizeof(address);
  if (!net::IPEndPoint(*source, 0).ToSockAddr(reinterpret_cast<sockaddr*>(&address), &size))
    return {nullptr, "invalid_lan_listener_route", 0};
#if BUILDFLAG(IS_WIN)
  const uint32_t index = htonl(route.interface_index);
  const int option = IP_UNICAST_IF;
#else
  const uint32_t index = route.interface_index;
  const int option = IP_BOUND_IF;
#endif
  if (platform::SetOption(socket.get(), IPPROTO_IP, option, &index, sizeof(index)) ||
      bind(socket.get(), reinterpret_cast<sockaddr*>(&address), size) || listen(socket.get(), 4))
    return {nullptr, "lan_listener_bind_failed", platform::Error()};
  if (getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address), &size))
    return {nullptr, "lan_listener_address_failed", platform::Error()};
  return {std::make_unique<TcpListener>(std::move(socket), ntohs(address.sin_port)), nullptr, 0};
}
}  // namespace crquickshare
