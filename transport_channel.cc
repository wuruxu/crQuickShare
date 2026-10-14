// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/transport_channel.h"
#include <utility>
#include "crQuickShare/connection_listener.h"
#include "crQuickShare/hotspot_upgrade.h"
#include "crQuickShare/mdns_network.h"
#include "base/strings/stringprintf.h"
namespace crquickshare {
TransportChannel::~TransportChannel() = default;
std::string TransportChannel::connection_error() const { return {}; }
ConnectionListener::~ConnectionListener() = default;
UpgradeNetwork::~UpgradeNetwork() = default;
ChannelConnectResult ConnectLanUpgradeChannel(const std::string& address, int port,
                                             base::TimeTicks deadline,
                                             const std::function<bool()>& active) {
  if (address.size() != 4 || port < 1 || port > 65535)
    return {nullptr, "invalid_lan_credentials", 0};
  uint32_t peer = 0;
  for (unsigned char byte : address) peer = (peer << 8) | byte;
  if (!(peer >> 24) || (peer >> 24) == 127 || (peer >> 28) >= 14)
    return {nullptr, "invalid_lan_address", 0};
  auto network = CreateMdnsNetwork();
  auto interfaces = network->Interfaces();
  if (!interfaces) return {nullptr, "lan_interfaces_unavailable", 0};
  const auto text = [](uint32_t ip) {
    return base::StringPrintf("%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
  };
  for (const auto& link : *interfaces)
    if (peer == link.address) return {nullptr, "lan_address_is_local", 0};
  ChannelConnectResult result{nullptr, "lan_peer_not_on_local_subnet", 0};
  for (const auto& link : *interfaces) {
    if (!link.index || !link.mask || ((peer ^ link.address) & link.mask) ||
        !(peer & ~link.mask) || (peer & ~link.mask) == ~link.mask) continue;
    result = ConnectTcpChannel(text(peer), static_cast<uint16_t>(port), deadline, active,
                               text(link.address), link.index);
    if (result.channel || !active() || base::TimeTicks::Now() >= deadline) break;
  }
  return result;
}
}  // namespace crquickshare
