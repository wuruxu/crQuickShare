// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_LAN_DISCOVERY_H_
#define CRQUICKSHARE_LAN_DISCOVERY_H_
#include <memory>
#include <string>
#include <vector>
#include "crQuickShare/quick_share_service.h"
#include "crQuickShare/endpoint_info_codec.h"
namespace crquickshare {
class LanDiscovery {
 public:
  virtual ~LanDiscovery();
  virtual bool ready() const = 0;
  // A service-instance collision rotates the endpoint while preserving the
  // Nearby instance-name wire format. The owner synchronizes its handshake ID.
  virtual std::string local_endpoint_id() const = 0;
  // All methods run on the discovery worker. Tick is nonblocking and bounded.
  virtual void Tick(bool visible) = 0;
  virtual void RestartQueries() = 0;
  virtual void RefreshAdvertisement() = 0;
  virtual std::vector<Peer> Peers() const = 0;
  // Numeric IP text; no native address structs or byte-order assumptions.
  virtual bool IsLocalAddress(const std::string& address) const = 0;
};
// Always returns a backend; ready() reports temporary network unavailability.
std::unique_ptr<LanDiscovery> CreateLanDiscovery(
    const std::string& endpoint, const std::string& info, uint16_t port);
}
#endif
