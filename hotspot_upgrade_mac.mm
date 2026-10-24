// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/hotspot_upgrade.h"
#include "crQuickShare/corewlan_client_mac.h"
#include "crQuickShare/platform_socket.h"
#include "crQuickShare/wifi_direct_go.h"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include "base/logging.h"
#include "net/base/ip_address.h"

namespace crquickshare {
namespace {
enum class StationMedium { kHotspot, kWifiDirect };
const char* SocketErrorName(int error) {
  switch (error) {
    case 0: return "none";
    case EACCES: return "EACCES";
    case EPERM: return "EPERM";
    case EINVAL: return "EINVAL";
    case ENXIO: return "ENXIO";
    case EBADF: return "EBADF";
    case EMFILE: return "EMFILE";
    case ENFILE: return "ENFILE";
    case ENOMEM: return "ENOMEM";
    case ENOBUFS: return "ENOBUFS";
    case EAFNOSUPPORT: return "EAFNOSUPPORT";
    case EPROTONOSUPPORT: return "EPROTONOSUPPORT";
    case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
    case ENETDOWN: return "ENETDOWN";
    case ENETUNREACH: return "ENETUNREACH";
    case ENETRESET: return "ENETRESET";
    case ECONNRESET: return "ECONNRESET";
    case ETIMEDOUT: return "ETIMEDOUT";
    case ECONNREFUSED: return "ECONNREFUSED";
    case EHOSTDOWN: return "EHOSTDOWN";
    case EHOSTUNREACH: return "EHOSTUNREACH";
    default: return "unclassified";
  }
}
bool RetryStationTcp(const ChannelConnectResult& result) {
  const std::string_view outcome = result.outcome;
  if (outcome == "source_route_unavailable" || outcome == "timed_out") return result.system_error == 0;
  if (outcome == "source_bind_failed") return result.system_error == EADDRNOTAVAIL;
  if (outcome != "connect_failed") return false;
  switch (result.system_error) {
    case EADDRNOTAVAIL:
    case ENETDOWN:
    case ENETUNREACH:
    case ENETRESET:
    case ECONNRESET:
    case ETIMEDOUT:
    case ECONNREFUSED:
    case EHOSTDOWN:
    case EHOSTUNREACH:
      return true;
    default:
      return false;
  }
}
class StationChannel final : public TransportChannel {
 public:
  StationChannel(std::unique_ptr<TransportChannel> channel, const char* medium)
      : channel_(std::move(channel)), medium_(medium) {}
  Result Read(base::span<char> bytes) override { return channel_->Read(bytes); }
  Result Write(base::span<const char> bytes) override { return channel_->Write(bytes); }
  Ready Wait(bool read, bool write, int ms) override { return channel_->Wait(read, write, ms); }
  void Close() override { channel_->Close(); }
  const char* name() const override { return medium_; }
 private:
  std::unique_ptr<TransportChannel> channel_;
  const char* const medium_;
};
HotspotUpgradeResult Join(const std::string& interface_name, const HotspotCredentials& credentials,
                         base::TimeTicks deadline, const std::function<bool()>& active,
                         StationMedium station_medium) {
  const auto started = base::TimeTicks::Now();
  const bool direct = station_medium == StationMedium::kWifiDirect;
  const char* medium = direct ? "WIFI_DIRECT" : "WIFI_HOTSPOT";
  const char* label = direct ? "Direct client" : "Hotspot";
  unsigned tcp_attempts = 0;
  ChannelConnectResult connected;
  auto network = std::make_unique<CoreWlanClient>(interface_name);
  auto failure = [&](const char* error) -> HotspotUpgradeResult {
    LOG(WARNING) << "crQuickShare: " << label << " join finished; backend=CoreWLAN; success=0; reason=" << error
                 << "; medium=" << medium << "; role=legacy_station"
                 << "; tcp_attempts=" << tcp_attempts
                 << "; last_tcp_outcome=" << (tcp_attempts ? connected.outcome : "not_attempted")
                 << "; last_tcp_errno=" << connected.system_error
                 << "; last_tcp_errno_name=" << SocketErrorName(connected.system_error)
                 << "; elapsed_ms=" << (base::TimeTicks::Now() - started).InMilliseconds();
    return {std::move(network), nullptr, error};
  };
  in_addr gateway{};
  const bool passphrase = credentials.password.size() >= 8 && credentials.password.size() <= 63 &&
      std::ranges::all_of(credentials.password, [](unsigned char c) { return c >= 32 && c < 127; });
  const bool hex_psk = direct && credentials.password.size() == 64 &&
      credentials.password.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
  const bool ipv4 = credentials.gateway.find('\0') == std::string::npos &&
      inet_pton(AF_INET, credentials.gateway.c_str(), &gateway) == 1 &&
      (ntohl(gateway.s_addr) >> 24) != 0 && (ntohl(gateway.s_addr) >> 24) != 127 &&
      (ntohl(gateway.s_addr) >> 28) < 14;
  // The existing Direct field carries an IPv6 link-local address. Its scope
  // comes from the joined Wi-Fi interface, never from peer-controlled text.
  const bool ipv6 = direct && credentials.ipv6_address.size() == 16 &&
      static_cast<unsigned char>(credentials.ipv6_address[0]) == 0xfe &&
      (static_cast<unsigned char>(credentials.ipv6_address[1]) & 0xc0) == 0x80;
  if (credentials.ssid.empty() || credentials.ssid.size() > 32 ||
      (!passphrase && !hex_psk) ||
      credentials.port < 1 || credentials.port > 65535 || (!ipv4 && !ipv6))
    return failure(direct ? "invalid_direct_client_credentials" : "invalid_hotspot_credentials");
  LOG(INFO) << "crQuickShare: " << label << " join requested; backend=CoreWLAN"
            << "; medium=" << medium << "; role=legacy_station; independent_p2p_interface=0"
            << "; ipv4_available=" << ipv4 << "; ipv6_available=" << ipv6;
  if (const char* error = network->Join(credentials.ssid, credentials.password,
          credentials.frequency, deadline, active)) return failure(error);
  const char* stop_reason = nullptr;
  auto running = [&] {
    if (stop_reason) return false;
    const bool active_now = active();
    if (!network->IsCurrent()) stop_reason = "wifi_connection_changed";
    else if (base::TimeTicks::Now() >= deadline) stop_reason = "timed_out";
    else if (!active_now) stop_reason = "cancelled";
    return !stop_reason;
  };
  LOG(INFO) << "crQuickShare: " << label << " join stage; backend=CoreWLAN; stage=" << (ipv6 ? "wait_ip" : "wait_ipv4");
  constexpr auto kAddressStableTime = base::Milliseconds(250);
  constexpr auto kTcpAttemptTime = base::Seconds(2);
  constexpr auto kTcpRetryTime = base::Seconds(8);
  constexpr unsigned kMaxTcpAttempts = 4;
  struct StationAddress {
    std::string peer;
    bool ipv6 = false;
    std::optional<TcpRoute> route;
    base::TimeTicks stable_since;
    unsigned attempts = 0;
  };
  std::vector<StationAddress> addresses;
  if (ipv6) addresses.push_back({net::IPAddress(base::as_byte_span(credentials.ipv6_address)).ToString(), true, std::nullopt, {}, 0});
  if (ipv4) addresses.push_back({credentials.gateway, false, std::nullopt, {}, 0});
  // Reserve attempts for IPv4 when both addresses are offered. In particular,
  // fast IPv6 failures must not consume every attempt before DHCP is ready.
  const unsigned attempts_per_address = addresses.size() > 1 ? 2 : kMaxTcpAttempts;
  base::TimeTicks tcp_deadline;
  unsigned route_revision = 0;
  while (running()) {
    const auto now = base::TimeTicks::Now();
    if (!tcp_deadline.is_null() && now >= tcp_deadline) return failure("tcp_retry_timed_out");
    StationAddress* selected = nullptr;
    for (auto& address : addresses) {
      if (address.attempts >= attempts_per_address) continue;
      std::optional<TcpRoute> route;
      if (address.ipv6) {
        const auto links = FindTcpRoutes(credentials.ipv6_address, true, network->interface_index());
        if (!links) return failure("interface_check_failed");
        if (!links->empty()) route = links->front();
      } else {
        const auto links = platform::Interfaces();
        if (!links) return failure("interface_check_failed");
        for (const auto& link : *links) {
          if (link.index != network->interface_index() || !link.mask.s_addr || !link.address.s_addr ||
              link.address.s_addr == gateway.s_addr || ((link.address.s_addr ^ gateway.s_addr) & link.mask.s_addr)) continue;
          const uint32_t host = ntohl(link.address.s_addr);
          if ((host >> 24) == 0 || (host >> 24) == 127 || (host >> 28) >= 14) continue;
          char source[INET_ADDRSTRLEN];
          if (!inet_ntop(AF_INET, &link.address, source, sizeof(source))) return failure("invalid_source_route");
          route = TcpRoute{source, link.index, static_cast<unsigned>(std::countl_one(ntohl(link.mask.s_addr)))};
          break;
        }
      }
      if (route != address.route) {
        address.route = std::move(route);
        address.stable_since = address.route ? now : base::TimeTicks();
        LOG(INFO) << "crQuickShare: Wi-Fi station route state; medium=" << medium
                  << "; event=" << (address.route ? "address_candidate" : "address_lost")
                  << "; family=" << (address.ipv6 ? "IPv6" : "IPv4")
                  << "; interface_index=" << network->interface_index()
                  << "; route_revision=" << ++route_revision
                  << "; required_stable_ms=" << kAddressStableTime.InMilliseconds();
      }
      if (address.route && now - address.stable_since >= kAddressStableTime &&
          (!selected || address.attempts < selected->attempts)) selected = &address;
    }
    if (!selected) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    // Address stability is a settling guard, not proof of DHCP completion or
    // gateway reachability. The bound TCP attempt verifies the actual path.
    if (tcp_deadline.is_null()) tcp_deadline = std::min(deadline, now + kTcpRetryTime);
    const auto attempt_started = base::TimeTicks::Now();
    ++tcp_attempts;
    ++selected->attempts;
    LOG(INFO) << "crQuickShare: " << label << " join stage; backend=CoreWLAN; stage=connect_tcp"
              << "; attempt=" << tcp_attempts << "; route_revision=" << route_revision
              << "; family=" << (selected->ipv6 ? "IPv6" : "IPv4")
              << "; address_stable_ms=" << (attempt_started - selected->stable_since).InMilliseconds();
    connected = ConnectTcpChannel(selected->peer, static_cast<uint16_t>(credentials.port),
        std::min(tcp_deadline, attempt_started + kTcpAttemptTime), running,
        selected->route->local_address, network->interface_index());
    const bool active_after_attempt = running();
    const auto attempt_finished = base::TimeTicks::Now();
    if (connected.channel && !active_after_attempt) {
      connected.channel->Close();
      connected.channel.reset();
    }
    const bool retryable = RetryStationTcp(connected);
    const bool retry = !connected.channel && active_after_attempt && retryable &&
        tcp_attempts < kMaxTcpAttempts && attempt_finished < tcp_deadline;
    const char* disposition = connected.channel ? "connected" :
        !active_after_attempt ? "stopped" : !retryable ? "not_retryable" :
        tcp_attempts >= kMaxTcpAttempts ? "attempt_limit" :
        attempt_finished >= tcp_deadline ? "retry_deadline" : "retry";
    LOG(INFO) << "crQuickShare: Wi-Fi station TCP attempt finished; medium=" << medium
              << "; interface_index=" << network->interface_index() << "; attempt=" << tcp_attempts
              << "; family=" << (selected->ipv6 ? "IPv6" : "IPv4")
              << "; route_revision=" << route_revision << "; outcome=" << connected.outcome
              << "; errno=" << connected.system_error << "; errno_name=" << SocketErrorName(connected.system_error)
              << "; retry=" << retry << "; disposition=" << disposition
              << "; stop_reason=" << (stop_reason ? stop_reason : "none")
              << "; elapsed_ms=" << (attempt_finished - attempt_started).InMilliseconds()
              << "; retry_remaining_ms=" << std::max<int64_t>(0, (tcp_deadline - attempt_finished).InMilliseconds());
    if (connected.channel) break;
    if (!retry) return failure(stop_reason ? stop_reason : connected.outcome);
    // Each attempt owns a fresh socket. Keep the station association and lease,
    // resample its source address, and service Bluetooth/cancellation while
    // backing off. No Nearby introduction has been written on these sockets.
    const auto retry_at = std::min(tcp_deadline, attempt_finished + base::Milliseconds(250 * tcp_attempts));
    while (running() && base::TimeTicks::Now() < retry_at)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!connected.channel) return failure(stop_reason ? stop_reason : "timed_out");
  LOG(INFO) << "crQuickShare: " << label << " join finished; backend=CoreWLAN; success=1; interface_index="
            << network->interface_index() << "; medium=" << medium << "; role=legacy_station"
            << "; tcp_attempts=" << tcp_attempts << "; elapsed_ms="
            << (base::TimeTicks::Now() - started).InMilliseconds();
  return {std::move(network), std::make_unique<StationChannel>(std::move(connected.channel), medium), nullptr};
}
}  // namespace
HotspotConnector MakeMacHotspotConnector(const std::string& requested_interface) {
  return [requested_interface](const HotspotCredentials& credentials, base::TimeTicks deadline,
                               const std::function<bool()>& active) {
    return Join(requested_interface, credentials, deadline, active, StationMedium::kHotspot);
  };
}
WifiDirectClientConnector MakeMacWifiDirectClientConnector(const std::string& requested_interface) {
  return [requested_interface](const HotspotCredentials& credentials, base::TimeTicks deadline,
                               const std::function<bool()>& active) {
    return Join(requested_interface, credentials, deadline, active, StationMedium::kWifiDirect);
  };
}
}  // namespace crquickshare
