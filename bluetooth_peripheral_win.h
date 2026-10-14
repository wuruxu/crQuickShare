// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_BLUETOOTH_PERIPHERAL_WIN_H_
#define CRQUICKSHARE_BLUETOOTH_PERIPHERAL_WIN_H_
#include <memory>
#include <vector>
#include <cstdint>
#include "base/functional/callback.h"
namespace crquickshare {
// UI-sequence Windows GATT server for the Nearby discovery slot. StaticValue
// lets Windows answer read/blob requests, including offsets, without UI races.
class BluetoothPeripheralWin {
 public:
  BluetoothPeripheralWin();
  ~BluetoothPeripheralWin();
  void Prepare(const std::vector<uint8_t>& value, base::OnceCallback<void(bool)> ready);
  void Advertise(const std::vector<uint8_t>& header, base::OnceCallback<void(bool)> ready);
  void Withdraw();
  void Stop();
  bool ready() const;
  bool advertising() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace crquickshare
#endif
