// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_BLUETOOTH_RECEIVER_H_
#define CRQUICKSHARE_BLUETOOTH_RECEIVER_H_
#include <memory>
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
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
#endif
