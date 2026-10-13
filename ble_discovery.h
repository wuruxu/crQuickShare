// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_BLE_DISCOVERY_H_
#define CRQUICKSHARE_BLE_DISCOVERY_H_
#include <memory>
#include <vector>
#include "crQuickShare/quick_share_service.h"
namespace crquickshare {
// Linux, UI sequence only. Discovery hints only, never authorization or a
// file transport. Reads advertised GATT metadata without pairing or powering
// on the radio; it does not write characteristics or subscribe to notifications.
class BleDiscovery {
 public:
  enum class State { kIdle, kStarting, kScanning, kUnavailable };
  BleDiscovery();
  ~BleDiscovery();
  void Update(bool enabled);
  State state() const;
  // Keeps LAN records intact for Send(). BLE-only records have no IP/port.
  std::vector<Peer> MergeWithLan(std::vector<Peer> peers) const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace crquickshare
#endif
