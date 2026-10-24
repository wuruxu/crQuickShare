// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_BLUETOOTH_CONNECTOR_MAC_H_
#define CRQUICKSHARE_BLUETOOTH_CONNECTOR_MAC_H_
#include <string>
#include "device/bluetooth/bluetooth_socket.h"
namespace device { class BluetoothAdapter; class BluetoothUUID; }
namespace crquickshare {
// UI sequence only. Resolves the service over live SDP before RFCOMM setup.
// Install the socket in the owner's pending state before
// ConnectMacBluetoothSocket(), whose callbacks may run synchronously.
scoped_refptr<device::BluetoothSocket> CreateMacBluetoothSocket();
// UI sequence only, after discovery releases GATT. macOS 27 can omit classic
// channel callbacks for an already-connected device until its link reconnects.
bool ShouldReconnectMacBluetoothBeforeSend(const std::string& address);
// Only accepts sockets from CreateMacBluetoothSocket(). The address must come
// from the peer's protocol advertisement, never CoreBluetooth's synthetic MAC.
// reconnect_bluetooth performs the approved preflight or manual recovery.
void ConnectMacBluetoothSocket(scoped_refptr<device::BluetoothSocket> socket,
                               const std::string& address,
                               const device::BluetoothUUID& uuid,
                               base::OnceClosure connected,
                               device::BluetoothSocket::ErrorCompletionCallback error,
                               bool reconnect_bluetooth = false);
// The caller must own an outstanding GATT connection request for this address.
// Cancels only a sole pending request, never a shared or established connection.
bool CancelMacPendingGattConnection(scoped_refptr<device::BluetoothAdapter> adapter,
                                    const std::string& address);
}  // namespace crquickshare
#endif
