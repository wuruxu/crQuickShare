// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_BLUETOOTH_RECEIVER_H_
#define CRQUICKSHARE_BLUETOOTH_RECEIVER_H_
#include <memory>
#include "build/build_config.h"
#include "crQuickShare/hotspot_upgrade.h"
namespace crquickshare {
class QuickShareService;
// UI-sequence receiver. Update periodically, including while no dialog is open.
// Call Update(nullptr) before destroying/replacing the supplied service.
class BluetoothReceiver {
 public:
  BluetoothReceiver();
  ~BluetoothReceiver();
  void Update(QuickShareService* service, HotspotConnector hotspot = {});
#if BUILDFLAG(IS_MAC)
  // Pause only BLE advertising, retaining the listener and GATT service.
  // Returns true once no receiver advertising/start callback is outstanding.
  // Stop Fast Init before resuming; continue calling Update while paused.
  bool SetAdvertisingPausedForDiscovery(bool paused);
#endif
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
#endif
