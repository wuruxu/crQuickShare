// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/bluetooth_connector_mac.h"
#import <IOBluetooth/IOBluetooth.h>
#include <memory>
#include <set>
#include <utility>
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/mac/mac_util.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/sys_string_conversions.h"
#include "crQuickShare/mac_sdp_client.h"
#include "crQuickShare/mac_bluetooth_reconnect.h"
#include "device/bluetooth/bluetooth_adapter.h"
#include "device/bluetooth/bluetooth_low_energy_device_mac.h"
#include "device/bluetooth/bluetooth_socket_mac.h"
#include "device/bluetooth/public/cpp/bluetooth_address.h"
#include "device/bluetooth/public/cpp/bluetooth_uuid.h"
#include "net/base/io_buffer.h"
namespace crquickshare {
namespace {

// UI sequence only. A successful wire SDP response proves this process has
// received the peer's classic channel callbacks. Never cache an SDP route.
std::set<std::string>& InitializedClassicPeers() {
  static base::NoDestructor<std::set<std::string>> addresses;
  return *addresses;
}

// Keep the pending socket cancellable across both live SDP and RFCOMM setup.
// The public BluetoothSocketMac transport remains responsible for data I/O.
class MacQuickShareSocket final : public device::BluetoothSocket {
 public:
  MacQuickShareSocket() : transport_(device::BluetoothSocketMac::CreateSocket()) {}
  void Connect(const std::string& address, const device::BluetoothUUID& uuid,
                base::OnceClosure connected, ErrorCompletionCallback error, bool reconnect_bluetooth) {
    scoped_refptr<MacQuickShareSocket> keep_alive(this);
    if (started_ || disconnected_) {
      std::move(error).Run("Bluetooth connection already started or cancelled");
      return;
    }
    if (MacBluetoothReconnectInProgress(address)) {
      std::move(error).Run("A previous Bluetooth reconnection is still finishing");
      return;
    }
    started_ = true;
    connected_ = std::move(connected);
    error_ = std::move(error);
    peer_ = [IOBluetoothDevice deviceWithAddressString:base::SysUTF8ToNSString(address)];
    if (!peer_) { Failed("Unable to create classic Bluetooth connection target"); return; }
    if (reconnect_bluetooth) {
      InitializedClassicPeers().erase(device::CanonicalizeBluetoothAddress(address));
      cancel_reconnect_ = ReconnectMacBluetoothDevice(address,
          base::BindOnce(&MacQuickShareSocket::Reconnected, weak_.GetWeakPtr(), address, uuid));
    } else Query(address, uuid);
  }
  void Disconnect(base::OnceClosure done) override {
    scoped_refptr<MacQuickShareSocket> keep_alive(this);
    disconnected_ = true;
    weak_.InvalidateWeakPtrs();
    connected_.Reset();
    error_.Reset();
    if (cancel_reconnect_) std::move(cancel_reconnect_).Run();
    sdp_.Cancel();
    transport_->Disconnect(std::move(done));
  }
  void Receive(int size, ReceiveCompletionCallback success, ReceiveErrorCompletionCallback error) override {
    transport_->Receive(size, std::move(success), std::move(error));
  }
  void Send(scoped_refptr<net::IOBuffer> bytes, int size,
             SendCompletionCallback success, ErrorCompletionCallback error) override {
    transport_->Send(std::move(bytes), size, std::move(success), std::move(error));
  }
  void Accept(AcceptCompletionCallback success, ErrorCompletionCallback error) override {
    transport_->Accept(std::move(success), std::move(error));
  }

 private:
  ~MacQuickShareSocket() override {
    weak_.InvalidateWeakPtrs();
    if (cancel_reconnect_) std::move(cancel_reconnect_).Run();
    sdp_.Cancel();
    if (!disconnected_) transport_->Disconnect(base::DoNothing());
  }
  void Query(const std::string& address, const device::BluetoothUUID& uuid) {
    sdp_.Query(address, uuid, base::BindOnce(&MacQuickShareSocket::Resolved,
                                          weak_.GetWeakPtr(), address, uuid));
  }
  void Reconnected(const std::string& address, const device::BluetoothUUID& uuid,
                     bool success, const std::string& error) {
    scoped_refptr<MacQuickShareSocket> keep_alive(this);
    cancel_reconnect_.Reset();
    if (!success) { Failed(error); return; }
    LOG(INFO) << "crQuickShare: macOS bootstrap recovery ready; next=live_sdp";
    Query(address, uuid);
  }
  void Resolved(const std::string& address, const device::BluetoothUUID& uuid, std::optional<MacSdpRoute> route,
                 const std::string& error) {
    scoped_refptr<MacQuickShareSocket> keep_alive(this);
    if (base::mac::MacOSMajorVersion() == 27) {
      const auto canonical = device::CanonicalizeBluetoothAddress(address);
      if (route) InitializedClassicPeers().insert(canonical);
      else InitializedClassicPeers().erase(canonical);
    }
    if (!route) { Failed(error); return; }
    LOG(INFO) << "crQuickShare: macOS bootstrap live route selected; handle=" << route->record_handle
              << "; rfcomm_channel=" << unsigned(route->rfcomm_channel)
              << "; service_uuid=" << uuid.canonical_value();
    transport_->ConnectToRfcommChannel(peer_, uuid, route->rfcomm_channel,
        base::BindOnce(&MacQuickShareSocket::Connected, weak_.GetWeakPtr()),
        base::BindOnce(&MacQuickShareSocket::Failed, weak_.GetWeakPtr()));
  }
  void Connected() {
    scoped_refptr<MacQuickShareSocket> keep_alive(this);
    auto done = std::move(connected_);
    error_.Reset();
    if (done) std::move(done).Run();
  }
  void Failed(const std::string& message) {
    scoped_refptr<MacQuickShareSocket> keep_alive(this);
    auto done = std::move(error_);
    connected_.Reset();
    if (done) std::move(done).Run(message);
  }
  scoped_refptr<device::BluetoothSocketMac> transport_;
  MacSdpClient sdp_;
  IOBluetoothDevice* __strong peer_ = nil;
  base::OnceClosure connected_;
  base::OnceClosure cancel_reconnect_;
  ErrorCompletionCallback error_;
  bool started_ = false;
  bool disconnected_ = false;
  base::WeakPtrFactory<MacQuickShareSocket> weak_{this};
};

}  // namespace

bool CancelMacPendingGattConnection(scoped_refptr<device::BluetoothAdapter> adapter,
                                    const std::string& address) {
  auto* peer = adapter ? adapter->GetDevice(address) : nullptr;
  if (!peer || !peer->IsLowEnergyDevice()) return false;
  return static_cast<device::BluetoothLowEnergyDeviceMac*>(peer)->CancelPendingGattConnection();
}
scoped_refptr<device::BluetoothSocket> CreateMacBluetoothSocket() {
  return base::WrapRefCounted(new MacQuickShareSocket());
}
bool ShouldReconnectMacBluetoothBeforeSend(const std::string& address) {
  const int version = base::mac::MacOSMajorVersion();
  if (version != 27) return false;
  const auto canonical = device::CanonicalizeBluetoothAddress(address);
  if (canonical.empty()) return false;
  IOBluetoothDevice* peer = [IOBluetoothDevice deviceWithAddressString:base::SysUTF8ToNSString(canonical)];
  const bool connected = peer && [peer isConnected];
  if (!connected) InitializedClassicPeers().erase(canonical);
  const bool initialized = InitializedClassicPeers().contains(canonical);
  const bool reconnect = connected && !initialized;
  LOG(INFO) << "crQuickShare: macOS Bluetooth connection preflight; macos_major=" << version
            << "; connected=" << connected << "; callbacks_previously_confirmed=" << initialized
            << "; reconnect=" << reconnect << "; reason="
            << (!connected ? "device_not_connected" : initialized ? "classic_callbacks_previously_confirmed" :
                "initialize_classic_channel_callbacks");
  return reconnect;
}
void ConnectMacBluetoothSocket(scoped_refptr<device::BluetoothSocket> socket,
                               const std::string& address,
                               const device::BluetoothUUID& uuid,
                               base::OnceClosure connected,
                               device::BluetoothSocket::ErrorCompletionCallback error,
                               bool reconnect_bluetooth) {
  if (!socket) {
    std::move(error).Run("Unable to create classic Bluetooth connection target");
    return;
  }
  static_cast<MacQuickShareSocket*>(socket.get())->Connect(
      address, uuid, std::move(connected), std::move(error), reconnect_bluetooth);
}
}  // namespace crquickshare
