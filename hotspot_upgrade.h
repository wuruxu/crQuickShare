// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_HOTSPOT_UPGRADE_H_
#define CRQUICKSHARE_HOTSPOT_UPGRADE_H_
#include <functional>
#include <memory>
#include <string>
#include "crQuickShare/transport_channel.h"
namespace crquickshare {
struct HotspotCredentials {
  std::string ssid, password, gateway;
  int port = 0;
  // Protocol value may be a frequency in MHz or a band identifier.
  int frequency = -1;
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
}  // namespace crquickshare
#endif
