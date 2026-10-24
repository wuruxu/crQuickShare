// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_BLE_DISCOVERY_H_
#define CRQUICKSHARE_BLE_DISCOVERY_H_
#include <memory>
#include "base/functional/callback.h"
#include <vector>
#include "base/memory/scoped_refptr.h"
#include "crQuickShare/quick_share_service.h"
namespace device { class BluetoothAdapter; }
namespace crquickshare {
// Desktop, UI sequence only. Discovery hints only, never authorization or a
// file transport. Reads advertised GATT metadata without pairing or powering
// on the radio; it does not write characteristics or subscribe to notifications.
class BleDiscovery {
 public:
  enum class State { kIdle, kStarting, kScanning, kUnavailable };
  BleDiscovery();
  ~BleDiscovery();
  // retain_cache only applies while disabled for a transfer. It never renews
  // record lifetimes; normal dialog closure must use the default false.
  void Update(bool enabled, bool retain_cache = false);
  State state() const;
  scoped_refptr<device::BluetoothAdapter> adapter() const;
  // Register after Update(false, true). One pending waiter; a new registration
  // replaces the old one. Callback is posted on the UI sequence after GATT release.
  void WhenConnectionReady(base::OnceClosure callback);
  bool bootstrap_active() const;
  // Keeps LAN records intact for Send(). BLE-only records have no IP/port.
  std::vector<Peer> MergeWithLan(std::vector<Peer> peers) const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace crquickshare
#endif
