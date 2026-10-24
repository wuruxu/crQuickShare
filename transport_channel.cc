// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/transport_channel.h"
#include <algorithm>
#include <utility>
#include "crQuickShare/connection_listener.h"
#include "crQuickShare/hotspot_upgrade.h"
#include "crQuickShare/mdns_network.h"
#include "base/strings/stringprintf.h"
#include "base/logging.h"
#include "base/strings/string_view_util.h"
#include "net/base/ip_address.h"
#include "net/base/network_interfaces.h"
namespace crquickshare {
TransportChannel::~TransportChannel() = default;
TransportChannel::SetupResult TransportChannel::PrepareConnection(
    const std::function<bool()>&, const std::function<void(const char*)>&) {
  return {};
}
std::string TransportChannel::connection_error() const { return {}; }
ConnectionListener::~ConnectionListener() = default;
LanUpgradeListenerResult CreateLanUpgradeListener() {
  LanUpgradeListenerResult result;
  net::NetworkInterfaceList links;
  if (!net::GetNetworkList(&links, net::EXCLUDE_HOST_SCOPE_VIRTUAL_INTERFACES)) {
    result.error = "lan_interfaces_unavailable"; return result;
  }
  // With no peer IP yet, prefer Wi-Fi. Do not guess between multiple addresses
  // of the preferred type, or publish VPN/bridge/link-local addresses.
  std::optional<net::NetworkInterface> selected;
  for (auto type : {net::NetworkChangeNotifier::CONNECTION_WIFI,
                    net::NetworkChangeNotifier::CONNECTION_ETHERNET}) {
    for (const auto& link : links) {
      if (link.type != type || !link.interface_index ||
          !link.address.IsIPv4() || link.address.IsLinkLocal() ||
          !link.prefix_length || link.prefix_length > 30 || link.ip_address_attributes) continue;
      const std::string bytes(base::as_string_view(link.address.bytes().span()));
      if (!IsTcpUnicastAddress(bytes, false) ||
          !IsPhysicalLanInterface(link.name, link.interface_index)) continue;
      if (selected && (selected->address != link.address || selected->interface_index != link.interface_index)) {
        result.error = "lan_local_address_ambiguous"; return result;
      }
      selected = link;
    }
    if (selected) break;
  }
  if (!selected) { result.error = "lan_local_address_unavailable"; return result; }
  result.route = {selected->address.ToString(), selected->interface_index, selected->prefix_length};
  result.ip_address = std::string(base::as_string_view(selected->address.bytes().span()));
  auto bound = CreateBoundLanUpgradeListener(result.route);
  result.listener = std::move(bound.listener);
  result.error = bound.error;
  result.system_error = bound.system_error;
  return result;
}
UpgradeNetwork::~UpgradeNetwork() = default;
bool IsTcpUnicastAddress(const std::string& address, bool allow_ipv6_link_local) {
  if (address.size() != 4 && address.size() != 16) return false;
  const net::IPAddress ip(base::as_byte_span(address));
  if (ip.IsZero() || ip.IsLoopback() || ip.IsMulticast() || ip.IsIPv4MappedIPv6()) return false;
  if (ip.IsIPv4()) return ip.bytes()[0] != 0 && ip.bytes()[0] < 224;
  return allow_ipv6_link_local || !ip.IsLinkLocal();
}
std::optional<std::vector<TcpRoute>> FindTcpRoutes(
    const std::string& address, bool allow_ipv6_link_local, unsigned interface_index) {
  std::vector<TcpRoute> result;
  if (!IsTcpUnicastAddress(address, allow_ipv6_link_local)) return result;
  const net::IPAddress peer(base::as_byte_span(address));
  if (peer.IsIPv6() && peer.IsLinkLocal() && !interface_index) return result;
  net::NetworkInterfaceList links;
  if (!net::GetNetworkList(&links, net::EXCLUDE_HOST_SCOPE_VIRTUAL_INTERFACES)) return std::nullopt;
  // A peer must not resolve to one of our own addresses. Link-local identity
  // is scoped to the selected interface, unlike globally scoped addresses.
  for (const auto& link : links)
    if (link.address == peer && (!peer.IsLinkLocal() || !interface_index || link.interface_index == interface_index))
      return result;
  for (const auto& link : links) {
    if (!link.interface_index || (interface_index && link.interface_index != interface_index) ||
        link.address.size() != peer.size() || !link.prefix_length || link.prefix_length > peer.size() * 8 ||
        (link.ip_address_attributes & (net::IP_ADDRESS_ATTRIBUTE_DEPRECATED | net::IP_ADDRESS_ATTRIBUTE_ANYCAST |
         net::IP_ADDRESS_ATTRIBUTE_TENTATIVE | net::IP_ADDRESS_ATTRIBUTE_DUPLICATED | net::IP_ADDRESS_ATTRIBUTE_DETACHED))) continue;
    const std::string source(base::as_string_view(link.address.bytes().span()));
    if (!IsTcpUnicastAddress(source, true) ||
        (peer.IsIPv6() && peer.IsLinkLocal() != link.address.IsLinkLocal()) ||
        !net::IPAddressMatchesPrefix(peer, link.address, link.prefix_length)) continue;
    if (peer.IsIPv4()) {
      uint32_t host = 0;
      for (const auto byte : peer.bytes()) host = (host << 8) | byte;
      const uint32_t mask = 0xffffffffu << (32 - link.prefix_length);
      if (!(host & ~mask) || (host & ~mask) == ~mask) continue;
    }
    TcpRoute route{link.address.ToString(), link.interface_index, link.prefix_length};
    if (std::ranges::find(result, route) == result.end()) result.push_back(std::move(route));
  }
  return result;
}
bool IsTcpRouteAvailable(const std::string& address, const std::string& local_address,
                         unsigned interface_index) {
  const auto peer = net::IPAddress::FromIPLiteral(address);
  const auto source = net::IPAddress::FromIPLiteral(local_address);
  if (!peer || !source || !interface_index || peer->size() != source->size()) return false;
  const auto routes = FindTcpRoutes(std::string(base::as_string_view(peer->bytes().span())), true, interface_index);
  return routes && std::ranges::any_of(*routes, [&](const auto& route) { return route.local_address == source->ToString(); });
}
ChannelConnectResult ConnectLanUpgradeChannel(const std::string& address, int port,
                                             base::TimeTicks deadline,
                                             const std::function<bool()>& active) {
  if (!active()) return {nullptr, "cancelled", 0};
  if (base::TimeTicks::Now() >= deadline) return {nullptr, "timed_out", 0};
  if ((address.size() != 4 && address.size() != 16) || port < 1 || port > 65535)
    return {nullptr, "invalid_lan_credentials", 0};
  if (!IsTcpUnicastAddress(address, false)) return {nullptr, "invalid_lan_address", 0};
  std::vector<TcpRoute> routes;
  if (address.size() == 4) {
    // Preserve IPv4 discovery's interface and subnet validation.
    uint32_t peer = 0;
    for (unsigned char byte : address) peer = (peer << 8) | byte;
    auto network = CreateMdnsNetwork();
    const auto interfaces = network->Interfaces();
    if (!interfaces) return {nullptr, "lan_interfaces_unavailable", 0};
    const auto text = [](uint32_t ip) {
      return base::StringPrintf("%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    };
    for (const auto& link : *interfaces)
      if (peer == link.address) return {nullptr, "lan_address_is_local", 0};
    for (const auto& link : *interfaces) {
      if (!link.index || !link.mask || ((peer ^ link.address) & link.mask) ||
          !(peer & ~link.mask) || (peer & ~link.mask) == ~link.mask) continue;
      routes.push_back({text(link.address), link.index, 0});
    }
  } else {
    const auto links = FindTcpRoutes(address, false);
    if (!links) return {nullptr, "lan_interfaces_unavailable", 0};
    routes = *links;
  }
  const net::IPAddress peer(base::as_byte_span(address));
  ChannelConnectResult result{nullptr, "lan_peer_not_on_local_subnet", 0};
  for (const auto& link : routes) {
    result = ConnectTcpChannel(peer.ToString(), static_cast<uint16_t>(port), deadline, active,
                               link.local_address, link.interface_index);
    LOG(INFO) << "crQuickShare: LAN upgrade TCP attempt; family=" << (address.size() == 16 ? "IPv6" : "IPv4")
              << "; interface_index=" << link.interface_index << "; outcome=" << result.outcome
              << "; error=" << result.system_error;
    if (result.channel || !active() || base::TimeTicks::Now() >= deadline) break;
  }
  return result;
}
}  // namespace crquickshare
