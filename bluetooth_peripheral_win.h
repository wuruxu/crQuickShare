// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_BLUETOOTH_PERIPHERAL_WIN_H_
#define CRQUICKSHARE_BLUETOOTH_PERIPHERAL_WIN_H_
#include <memory>
#include <vector>
#include <cstdint>
#include "base/functional/callback.h"
namespace crquickshare {
// UI-sequence Windows extended discovery publisher and legacy GATT server.
// StaticValue lets Windows answer read/blob requests without UI races.
// Either complete discovery advertisement is sufficient for readiness; partial
// GATT advertising alone is not reported as successful discovery.
class BluetoothPeripheralWin {
 public:
  BluetoothPeripheralWin();
  ~BluetoothPeripheralWin();
  void Prepare(const std::vector<uint8_t>& value, base::OnceCallback<void(bool)> ready);
  void Advertise(const std::vector<uint8_t>& header, base::OnceCallback<void(bool)> ready);
  void Withdraw();
  void Stop();
  // Drain retired advertisements on the UI sequence before WinRT teardown.
  static void DrainPendingStops();
  bool ready() const;
  bool advertising() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace crquickshare
#endif
