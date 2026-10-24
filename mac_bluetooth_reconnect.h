// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_MAC_BLUETOOTH_RECONNECT_H_
#define CRQUICKSHARE_MAC_BLUETOOTH_RECONNECT_H_

#include <string>
#include "base/functional/callback.h"

namespace crquickshare {
// UI sequence only. Also covers a cancelled transfer's bounded link restoration.
bool MacBluetoothReconnectInProgress(const std::string& address);
// UI sequence only; requires explicit user consent to interrupt this device's
// other Bluetooth services. Completion is asynchronous. Never changes pairing.
// The returned closure cancels delivery of the result. Once close is requested,
// the operation still makes its bounded reconnect attempt, even after cancel.
base::OnceClosure ReconnectMacBluetoothDevice(
    const std::string& address,
    base::OnceCallback<void(bool, const std::string&)> done);
}  // namespace crquickshare
#endif
