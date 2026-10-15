// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_WIFI_DIRECT_GO_H_
#define CRQUICKSHARE_WIFI_DIRECT_GO_H_
#include "crQuickShare/hotspot_upgrade.h"
#include <vector>
namespace crquickshare {
struct WifiDirectInterface {
  std::string name;
  uint32_t mode = 0;  // NM wireless mode: 2 = station, 3 = AP.
  bool available = false;
  bool connected = false;
};
// Read-only NM enumeration. Blocking; call on a worker.
std::vector<WifiDirectInterface> GetLinuxWifiDirectInterfaces();
// Owns a dedicated P2P group, never the station connection. Worker only.
class WifiDirectGroup : public UpgradeNetwork {
 public:
  HotspotCredentials credentials;
  std::string ipv6_address;  // Optional 16-byte link-local address, network order.
  virtual std::unique_ptr<TransportChannel> Accept(
      base::TimeTicks deadline, const std::function<bool()>& active) = 0;
};
using WifiDirectGoFactory = std::function<std::unique_ptr<WifiDirectGroup>(
    base::TimeTicks, const std::function<bool()>&)>;
// Credential-based client on an independent NM P2P interface. No STA changes.
using WifiDirectClientConnector = HotspotConnector;
WifiDirectClientConnector MakeLinuxWifiDirectClientConnector(
    const std::string& interface_name);
// Explicit opt-in. NetworkManager owns addresses/DHCP/group lifetime.
WifiDirectGoFactory MakeLinuxWifiDirectGoFactory(
    const std::string& interface_name);
}  // namespace crquickshare
#endif
