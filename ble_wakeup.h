// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_BLE_WAKEUP_H_
#define CRQUICKSHARE_BLE_WAKEUP_H_
#include <memory>
namespace crquickshare {
// UI-sequence-only Fast Initiation advertising. Does not open a Bluetooth
// connection, change radio power, or authorize receipt of files.
class BleWakeup {
 public:
  enum class State { kIdle, kStarting, kAdvertising, kUnavailable };
  BleWakeup();
  ~BleWakeup();
  void Update(bool enabled);
  State state() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
#endif
