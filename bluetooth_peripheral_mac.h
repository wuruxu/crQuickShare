// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_BLUETOOTH_PERIPHERAL_MAC_H_
#define CRQUICKSHARE_BLUETOOTH_PERIPHERAL_MAC_H_
#include <cstdint>
#include <memory>
#include <vector>
#include "base/functional/callback.h"
namespace crquickshare {
// UI-sequence FEF3 GATT server and Nearby Apple local-name advertiser.
// Advertising success proves only local OS readiness, not Android discovery.
class BluetoothPeripheralMac {
 public:
  BluetoothPeripheralMac();
  ~BluetoothPeripheralMac();
  void Prepare(const std::vector<uint8_t>& value, base::OnceCallback<void(bool)> ready);
  void Advertise(const std::vector<uint8_t>& header, base::OnceCallback<void(bool)> ready);
  void Withdraw();
  void Stop();
  // Drive bounded setup/advertising waits and detect native state loss.
  bool Poll();
  bool ready() const;
  bool advertising() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace crquickshare
#endif
