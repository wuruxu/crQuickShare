// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_HOTSPOT_UPGRADE_H_
#define CRQUICKSHARE_HOTSPOT_UPGRADE_H_
#include <cstdint>
#include <vector>
#include <functional>
#include <memory>
#include <string>
#include "crQuickShare/transport_channel.h"
namespace crquickshare {
struct WifiUpgradeInterface {
  std::string name;  // Linux device name or Windows interface alias, UTF-8.
  uint32_t mode = 0;  // NM wireless mode: 2 = station, 3 = AP; 0 on Windows.
  bool available = false;
  bool connected = false;
};
// Read-only Native Wi-Fi enumeration. Blocking; call on a worker.
std::vector<WifiUpgradeInterface> GetWindowsWifiInterfaces();
struct HotspotCredentials {
  std::string ssid, password, gateway;
  int port = 0;
  // Protocol value may be a frequency in MHz or a band identifier.
  int frequency = -1;
  // Optional Direct GO IPv6 link-local address, 16 bytes in network order.
  std::string ipv6_address = {};
};
// Explicit worker-only cleanup after closing the upgraded channel.
// Destruction is only a short best-effort fallback.
class UpgradeNetwork {
 public:
  virtual ~UpgradeNetwork();
  virtual bool Cleanup(base::TimeTicks deadline,
                       const std::function<bool()>& keep_restoring) = 0;
};
struct HotspotUpgradeResult {
  std::unique_ptr<UpgradeNetwork> network;
  std::unique_ptr<TransportChannel> channel;
  const char* error = nullptr;
};
using HotspotConnector = std::function<HotspotUpgradeResult(
    const HotspotCredentials&, base::TimeTicks, const std::function<bool()>&)>;
// Linux worker-only NetworkManager client. No network changes during creation.
// Empty interface selects the sole Linux wireless interface, never an
// arbitrary radio when multiple interfaces exist. No network changes here.
HotspotConnector MakeLinuxHotspotConnector(const std::string& interface);
// Windows worker-only Native Wi-Fi client. Empty selects the sole Wi-Fi radio;
// an explicit interface is a Windows interface alias (for example "Wi-Fi").
// Factory creation does not query or change networking.
HotspotConnector MakeWindowsHotspotConnector(const std::string& requested_interface);
// macOS CoreWLAN station client. Empty selects the sole Wi-Fi radio; an explicit
// interface is its BSD name (for example "en0"). RequestCoreWlanAccess() must
// first run on the UI sequence. Factory creation does not change networking.
HotspotConnector MakeMacHotspotConnector(const std::string& requested_interface);
}  // namespace crquickshare
#endif
