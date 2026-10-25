// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/advertisement_codec.h"
#include "crQuickShare/bluetooth_receiver.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_MAC)
#include "base/command_line.h"
#include "crQuickShare/bluetooth_peripheral_mac.h"
#endif
#if BUILDFLAG(IS_WIN)
#include "crQuickShare/bluetooth_peripheral_win.h"
#include "device/bluetooth/bluetooth_socket_win.h"
#include "device/bluetooth/bluetooth_socket_thread.h"
#endif
#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "crQuickShare/bootstrap_probe.h"
#include "crQuickShare/quick_share_service.h"
#include "device/bluetooth/bluetooth_adapter.h"
#include "device/bluetooth/bluetooth_adapter_factory.h"
#include "device/bluetooth/bluetooth_advertisement.h"
#include "device/bluetooth/bluetooth_local_gatt_characteristic.h"
#include "device/bluetooth/bluetooth_local_gatt_service.h"
#include "device/bluetooth/bluetooth_socket.h"

namespace crquickshare {
namespace {
constexpr char kSharingUuid[] = "a82efa21-ae5c-3dde-9bbc-f16da7b16c5a";
#if BUILDFLAG(IS_MAC)
// Channel 29 passed an incoming phone transfer where channel 1 produced no
// native callback. These are candidates, not channels known to be available.
constexpr std::array<int, 5> kReceiverChannels = {29, 28, 27, 26, 30};
#endif
#if !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_MAC)
constexpr char kDiscoveryUuid[] = "0000fef3-0000-1000-8000-00805f9b34fb";
constexpr char kSlotUuid[] = "00000000-0000-3000-8000-000000000000";
#endif
using Gatt = device::BluetoothLocalGattService;
using Characteristic = device::BluetoothLocalGattCharacteristic;
using Descriptor = device::BluetoothLocalGattDescriptor;
using Device = device::BluetoothDevice;
using Advertisement = device::BluetoothAdvertisement;
#if !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_MAC)
void ReleaseAdvertisement(scoped_refptr<Advertisement> advertisement) {
  advertisement->Unregister(base::BindOnce([](scoped_refptr<Advertisement>) {}, advertisement),
      base::BindOnce([](scoped_refptr<Advertisement>, Advertisement::ErrorCode) {}, advertisement));
}
#endif
}
struct BluetoothReceiver::Impl : device::BluetoothAdapter::Observer,
                                 Gatt::Delegate,
                                 Advertisement::Observer {
  raw_ptr<QuickShareService> service = nullptr;
  HotspotConnector hotspot;
  scoped_refptr<device::BluetoothAdapter> adapter;
  scoped_refptr<device::BluetoothSocket> listener;
  scoped_refptr<Advertisement> advertisement;
  base::WeakPtr<Gatt> gatt;
  base::WeakPtr<Characteristic> slot;
  BootstrapProbe connection{false};
#if BUILDFLAG(IS_WIN)
  BluetoothPeripheralWin peripheral;
#elif BUILDFLAG(IS_MAC)
  BluetoothPeripheralMac peripheral;
  bool initializing = false, permission_pending = false, permission_requested = false;
  std::optional<int> verification_channel;
  bool verification_invalid = false, verification_attempted = false;
  size_t receiver_channel_index = 0;
  bool advertising_paused_for_discovery = false;
#endif
  Peer identity;
  std::vector<uint8_t> value, header;
  bool starting = false, advertising_pending = false, withdrawing = false;
  base::TimeTicks retry_after;
  base::WeakPtrFactory<Impl> lifetime{this};
  base::WeakPtrFactory<Impl> operations{this};

  Impl() {
#if BUILDFLAG(IS_MAC)
    constexpr char kChannelSwitch[] = "quick-share-mac-receiver-rfcomm-channel";
    const auto* command_line = base::CommandLine::ForCurrentProcess();
    if (command_line->HasSwitch(kChannelSwitch)) {
      int channel = 0;
      if (!base::StringToInt(command_line->GetSwitchValueASCII(kChannelSwitch), &channel) ||
          channel < 1 || channel > 30) {
        verification_invalid = true;
        LOG(ERROR) << "crQuickShare: macOS receiver channel verification disabled Bluetooth reception; "
                   << "reason=invalid_channel; expected_range=1..30";
      } else {
        verification_channel = channel;
        LOG(INFO) << "crQuickShare: macOS receiver channel verification configured; requested_channel="
                  << channel << "; listener_attempt_limit=1; restart_required_for_new_attempt=1";
      }
    }
#else
    if (device::BluetoothAdapterFactory::IsBluetoothSupported())
      device::BluetoothAdapterFactory::Get()->GetAdapter(base::BindOnce(&Impl::AdapterReady, lifetime.GetWeakPtr()));
#endif
  }
  ~Impl() override {
#if BUILDFLAG(IS_WIN)
    LOG(INFO) << "crQuickShare shutdown: stage=bluetooth_receiver; event=started; advertisement="
              << !!advertisement << "; listener=" << !!listener;
#endif
    lifetime.InvalidateWeakPtrs();
    Stop(); connection.Cancel();
    if (adapter) adapter->RemoveObserver(this);
#if BUILDFLAG(IS_WIN)
    LOG(INFO) << "crQuickShare shutdown: stage=bluetooth_receiver; event=finished";
#endif
  }
  void AdapterReady(scoped_refptr<device::BluetoothAdapter> result) {
    adapter = std::move(result); adapter->AddObserver(this);
#if BUILDFLAG(IS_MAC)
    initializing = false;
#endif
    Reconcile();
  }
  void Update(QuickShareService* next, HotspotConnector connector) {
    hotspot = std::move(connector);
    if (service != next) {
      Stop(); connection.Cancel(); service = next; retry_after = {};
    }
    Reconcile();
  }
#if BUILDFLAG(IS_MAC)
  bool SetAdvertisingPausedForDiscovery(bool paused) {
    if (advertising_paused_for_discovery != paused) {
      advertising_paused_for_discovery = paused;
      LOG(INFO) << "crQuickShare: macOS receiver advertising arbitration; paused_for_discovery="
                << paused << "; listener_present=" << !!listener
                << "; gatt_ready=" << peripheral.ready()
                << "; advertising_pending=" << advertising_pending;
    }
    // A native start callback carries no request ID. Let it settle before
    // withdrawing, rather than retiring the GATT manager during a handoff.
    if (paused && !advertising_pending) Withdraw();
    return !advertising_pending && !peripheral.advertising();
  }
#endif
  bool Visible() const {
#if BUILDFLAG(IS_MAC)
    if (!adapter || permission_pending ||
        adapter->GetOsPermissionStatus() != device::BluetoothAdapter::PermissionStatus::kAllowed) return false;
#endif
    if (!service || !adapter || !adapter->IsPresent() || !adapter->IsPowered()) return false;
    const auto state = service->GetSnapshot();
    return state.receiving && !state.busy;
  }
  void Withdraw() {
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    peripheral.Withdraw(); advertising_pending = false;
#endif
    if (!advertisement) return;
    advertisement->RemoveObserver(this);
    auto released = std::move(advertisement);
    withdrawing = true;
    LOG(INFO) << "crQuickShare: Bluetooth receiver advertisement unregister requested";
    released->Unregister(base::BindOnce([](base::WeakPtr<Impl> self, scoped_refptr<Advertisement> held) {
          LOG(INFO) << "crQuickShare: Bluetooth receiver advertisement unregister completed; success=1";
          if (self) self->Withdrawn(std::move(held));
        }, lifetime.GetWeakPtr(), released),
        base::BindOnce([](base::WeakPtr<Impl> self, scoped_refptr<Advertisement> held, Advertisement::ErrorCode error) {
          LOG(WARNING) << "crQuickShare: Bluetooth receiver advertisement unregister completed; success=0; error="
                       << static_cast<int>(error);
          if (self) self->Withdrawn(std::move(held));
        }, lifetime.GetWeakPtr(), released));
  }
  void Withdrawn(scoped_refptr<Advertisement>) {
    withdrawing = false;
    // Reconciliation runs on the owner's timer, outside the unregister callback.
  }
  void Stop() {
    operations.InvalidateWeakPtrs();
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    peripheral.Stop();
#endif
    Withdraw(); starting = false; advertising_pending = false;
    slot.reset();
    if (gatt) gatt->Delete();
    gatt.reset();
    if (listener) { auto closed = std::move(listener); closed->Disconnect(base::DoNothing()); }
    value.clear(); header.clear();
  }
  void Failed(const char* stage) {
#if BUILDFLAG(IS_MAC)
    if (verification_channel) {
      verification_attempted = true;
      LOG(WARNING) << "crQuickShare: Bluetooth receiver setup failed; stage=" << stage
                   << "; channel_verification=1; retry_enabled=0; restart_required=1";
      Stop(); return;
    }
#endif
    LOG(WARNING) << "crQuickShare: Bluetooth receiver setup failed; stage=" << stage << "; retry_s=15";
    Stop(); retry_after = base::TimeTicks::Now() + base::Seconds(15);
  }
#if BUILDFLAG(IS_MAC)
  void ListenerFailed() {
    if (!verification_channel) {
      const int failed_channel = kReceiverChannels[receiver_channel_index];
      receiver_channel_index = (receiver_channel_index + 1) % kReceiverChannels.size();
      LOG(WARNING) << "crQuickShare: macOS receiver channel selection failed; requested_channel="
                   << failed_channel << "; next_channel=" << kReceiverChannels[receiver_channel_index]
                   << "; retry_s=15";
    }
    Failed("rfcomm_listener");
  }
#endif
  bool BuildAdvertisement() {
    identity = service->LocalPeer();
    const std::string local_address = adapter->GetAddress();
    std::string hex;
    for (char ch : local_address) if (ch != ':') hex += ch;
    std::string mac;
    if (identity.id.size() != 4 || identity.endpoint_info.size() > 131 ||
        !base::HexStringToString(hex, &mac) || mac.size() != 6) return false;
    const auto bytes = EncodeAdvertisement(identity, mac);
    if (bytes.empty()) return false;
    value.assign(bytes.begin(), bytes.end());
    const auto encoded = EncodeGattHeader();
    header.assign(encoded.begin(), encoded.end());
#if BUILDFLAG(IS_MAC)
    LOG(INFO) << "crQuickShare: macOS receiver advertised route; endpoint_id=" << base::HexEncode(identity.id)
              << "; classic_address=" << local_address << "; classic_address_hex=" << base::HexEncode(mac)
              << "; address_source=local_adapter; service_uuid=" << kSharingUuid
              << "; gatt_bytes=" << value.size();
#endif
    return true;
  }
  void Reconcile() {
#if BUILDFLAG(IS_MAC)
    if (verification_invalid || (verification_attempted && !listener && !starting)) return;
    // Reception can be enabled without opening the outgoing discovery dialog.
    // Resolve OS permission before touching the classic APIs, which may block
    // while the system prompt is outstanding.
    if (service && service->GetSnapshot().receiving && !adapter && !initializing &&
        device::BluetoothAdapterFactory::IsBluetoothSupported()) {
      initializing = true;
      device::BluetoothAdapterFactory::Get()->GetAdapter(
          base::BindOnce(&Impl::AdapterReady, lifetime.GetWeakPtr()));
      return;
    }
    if (initializing || permission_pending) return;
    if (adapter && service && service->GetSnapshot().receiving && !permission_requested &&
        adapter->GetOsPermissionStatus() == device::BluetoothAdapter::PermissionStatus::kUndetermined) {
      permission_pending = permission_requested = true;
      adapter->RequestSystemPermission(base::BindOnce([](base::WeakPtr<Impl> self,
          device::BluetoothAdapter::PermissionStatus status) {
        if (!self) return;
        self->permission_pending = false;
        LOG(INFO) << "crQuickShare: macOS receiver permission; allowed="
                  << (status == device::BluetoothAdapter::PermissionStatus::kAllowed);
        self->Reconcile();
      }, lifetime.GetWeakPtr()));
      return;
    }
    if (adapter && adapter->GetOsPermissionStatus() != device::BluetoothAdapter::PermissionStatus::kAllowed) {
      Stop(); return;
    }
#endif
    if (!service || !adapter || !adapter->IsPresent() || !adapter->IsPowered() ||
        !service->GetSnapshot().receiving) {
      Stop();
      return;
    }
    if (!Visible()) { Withdraw(); return; }
#if BUILDFLAG(IS_MAC)
    if (!peripheral.Poll()) { Failed("peripheral_state"); return; }
    if (advertising_paused_for_discovery) {
      if (!advertising_pending) Withdraw();
      return;
    }
#endif
    const auto local = service->LocalPeer();
    if (listener && (local.id != identity.id || local.endpoint_info != identity.endpoint_info)) Stop();
    if (base::TimeTicks::Now() < retry_after || withdrawing || starting || advertising_pending || advertisement) return;
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    if (peripheral.advertising()) return;
#endif
    if (!listener) {
#if BUILDFLAG(IS_MAC)
      // Stop() may have run above after an identity change. Do not publish a
      // second record in the same controlled verification run.
      if (verification_attempted) return;
#endif
#if BUILDFLAG(IS_WIN)
      // Extended discovery carries the RFCOMM address directly and can work
      // even when connectable GATT advertising is unavailable.
      LOG(INFO) << "crQuickShare: Windows receiver adapter; peripheral_role_supported="
                << adapter->IsPeripheralRoleSupported();
#endif
      if (!BuildAdvertisement()) { Failed("identity"); return; }
      starting = true;
      device::BluetoothAdapter::ServiceOptions options;
      options.name = "NearbySharing"; options.require_authentication = false;
#if BUILDFLAG(IS_MAC)
      options.channel = verification_channel.value_or(kReceiverChannels[receiver_channel_index]);
      if (verification_channel) {
        verification_attempted = true;
        LOG(INFO) << "crQuickShare: macOS receiver channel verification attempt; requested_channel="
                  << *verification_channel << "; service_uuid=" << kSharingUuid;
      } else {
        LOG(INFO) << "crQuickShare: macOS receiver channel selection; requested_channel="
                  << *options.channel << "; candidate_index=" << receiver_channel_index
                  << "; candidate_count=" << kReceiverChannels.size()
                  << "; policy=explicit_candidates; service_uuid=" << kSharingUuid;
      }
#endif
#if BUILDFLAG(IS_WIN)
      auto socket = device::BluetoothSocketWin::CreateBluetoothSocket(
          base::SingleThreadTaskRunner::GetCurrentDefault(), device::BluetoothSocketThread::Get());
      socket->AllowUncachedIncomingPeer();
      socket->Listen(adapter, device::BluetoothUUID(kSharingUuid), options,
          base::BindOnce([](base::WeakPtr<Impl> self, scoped_refptr<device::BluetoothSocket> connected) {
            if (self) self->Listening(std::move(connected));
            else connected->Disconnect(base::DoNothing());
          }, operations.GetWeakPtr(), socket),
          base::BindOnce([](base::WeakPtr<Impl> self, const std::string&) {
            if (self) self->Failed("rfcomm_listener");
          }, operations.GetWeakPtr()));
#else
      adapter->CreateRfcommService(device::BluetoothUUID(kSharingUuid), options,
          base::BindOnce([](base::WeakPtr<Impl> self, scoped_refptr<device::BluetoothSocket> socket) {
            if (self) self->Listening(std::move(socket));
            else socket->Disconnect(base::DoNothing());
          }, operations.GetWeakPtr()),
          base::BindOnce([](base::WeakPtr<Impl> self, const std::string& error) {
            LOG(WARNING) << "crQuickShare: Bluetooth receiver RFCOMM listener failed; reason=" << error;
            if (!self) return;
#if BUILDFLAG(IS_MAC)
            self->ListenerFailed();
#else
            self->Failed("rfcomm_listener");
#endif
          }, operations.GetWeakPtr()));
#endif
      return;
    }
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    if (peripheral.ready()) Advertise();
#else
    if (gatt && gatt->IsRegistered()) Advertise();
#endif
  }
  void Listening(scoped_refptr<device::BluetoothSocket> socket) {
    listener = std::move(socket);
    LOG(INFO) << "crQuickShare: Bluetooth receiver listener ready; service_uuid=" << kSharingUuid;
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    peripheral.Prepare(value, base::BindOnce([](base::WeakPtr<Impl> self, bool success) {
      if (!self) return;
      if (!success) { self->Failed("receiver_prepare"); return; }
      self->starting = false;
      LOG(INFO) << "crQuickShare: Bluetooth receiver prepared";
      self->Accept();
      self->Reconcile();
    }, operations.GetWeakPtr()));
#else
    gatt = adapter->CreateLocalGattService(device::BluetoothUUID(kDiscoveryUuid), true, this);
    if (!gatt) { Failed("gatt_create"); return; }
    // Stock BlueZ grants read access through the "read" property. Chromium's
    // PERMISSION_READ also exports "permission-read", which stock BlueZ rejects.
    slot = gatt->CreateCharacteristic(device::BluetoothUUID(kSlotUuid),
        device::BluetoothGattCharacteristic::PROPERTY_READ,
        device::BluetoothGattCharacteristic::PERMISSION_NONE);
    if (!slot) { Failed("gatt_slot"); return; }
    gatt->Register(base::BindOnce(&Impl::GattReady, operations.GetWeakPtr()),
        base::BindOnce([](base::WeakPtr<Impl> self, device::BluetoothGattService::GattErrorCode) {
          if (self) self->Failed("gatt_register");
        }, operations.GetWeakPtr()));
#endif
  }
  void GattReady() {
    starting = false;
    LOG(INFO) << "crQuickShare: Bluetooth receiver GATT ready; slots=1; bytes=" << value.size();
    Accept();
    Reconcile();
  }
  void Advertise() {
    if (!Visible()) return;
    advertising_pending = true;
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    peripheral.Advertise(header, base::BindOnce([](base::WeakPtr<Impl> self, bool success) {
      if (!self) return;
      self->advertising_pending = false;
      if (!success) { self->Failed("advertisement"); return; }
      LOG(INFO) << "crQuickShare: Bluetooth receiver advertising; transport=BLUETOOTH; lan_client=1; hotspot_client=" << bool(self->hotspot);
#if BUILDFLAG(IS_MAC)
      if (self->advertising_paused_for_discovery) { self->Withdraw(); return; }
#endif
      if (!self->Visible()) self->Withdraw();
    }, operations.GetWeakPtr()));
#else
    auto data = std::make_unique<Advertisement::Data>(Advertisement::ADVERTISEMENT_TYPE_PERIPHERAL);
    data->set_service_uuids(Advertisement::UUIDList{kDiscoveryUuid});
    Advertisement::ServiceData services;
    services.emplace(kDiscoveryUuid, header); data->set_service_data(std::move(services));
    adapter->RegisterAdvertisement(std::move(data),
        base::BindOnce([](base::WeakPtr<Impl> self, scoped_refptr<Advertisement> ad) {
          if (!self) { ReleaseAdvertisement(std::move(ad)); return; }
          self->advertising_pending = false; self->advertisement = std::move(ad);
          self->advertisement->AddObserver(self.get());
          LOG(INFO) << "crQuickShare: Bluetooth receiver advertising; transport=BLUETOOTH; bluetooth_batch_limit_bytes=73728; lan_client=1; hotspot_client=" << bool(self->hotspot);
          if (!self->Visible()) self->Withdraw();
        }, operations.GetWeakPtr()),
        base::BindOnce([](base::WeakPtr<Impl> self, Advertisement::ErrorCode code) {
          if (!self) return;
          LOG(WARNING) << "crQuickShare: Bluetooth receiver advertisement error; code=" << static_cast<int>(code);
          self->Failed("advertisement");
        }, operations.GetWeakPtr()));
#endif
  }
  void Accept() {
    if (!listener) return;
    listener->Accept(base::BindOnce([](base::WeakPtr<Impl> self, const Device* peer, scoped_refptr<device::BluetoothSocket> socket) {
      if (!self) { socket->Disconnect(base::DoNothing()); return; }
#if BUILDFLAG(IS_MAC)
      LOG(INFO) << "crQuickShare: macOS receiver accept callback; peer_present=" << bool(peer)
                << "; address=" << (peer ? peer->GetAddress() : std::string())
                << "; socket_present=" << bool(socket) << "; visible=" << self->Visible();
#endif
      if (!self->Visible() || !self->service || !self->connection.Receive(std::move(socket), *self->service, self->hotspot)) {
        if (socket) socket->Disconnect(base::DoNothing());
        LOG(INFO) << "crQuickShare: Bluetooth incoming connection rejected; reason=disabled_or_busy";
      } else {
        LOG(INFO) << "crQuickShare: Bluetooth receiver incoming connection accepted";
        self->Withdraw();
      }
      // Avoid recursive Accept when BlueZ already has multiple queued clients.
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
          base::BindOnce(&Impl::Accept, self));
    }, operations.GetWeakPtr()), base::BindOnce([](base::WeakPtr<Impl> self, const std::string& error) {
      LOG(WARNING) << "crQuickShare: Bluetooth receiver accept failed; reason=" << error;
      if (self) self->Failed("accept");
    }, operations.GetWeakPtr()));
  }
  void OnCharacteristicReadRequest(const Device*, const Characteristic* characteristic, int offset, ValueCallback callback) override {
    if (!Visible() || characteristic != slot.get() || offset < 0 || static_cast<size_t>(offset) > value.size()) {
      std::move(callback).Run(device::BluetoothGattService::GattErrorCode::kNotPermitted, {}); return;
    }
    LOG(INFO) << "crQuickShare: Bluetooth receiver advertisement read; offset=" << offset;
    std::move(callback).Run(std::nullopt, std::vector<uint8_t>(value.begin() + offset, value.end()));
  }
  void OnCharacteristicWriteRequest(const Device*, const Characteristic*, const std::vector<uint8_t>&, int,
                                   base::OnceClosure, ErrorCallback error) override { std::move(error).Run(); }
  void OnCharacteristicPrepareWriteRequest(const Device*, const Characteristic*, const std::vector<uint8_t>&, int,
                                          bool, base::OnceClosure, ErrorCallback error) override { std::move(error).Run(); }
  void OnDescriptorReadRequest(const Device*, const Descriptor*, int, ValueCallback callback) override {
    std::move(callback).Run(device::BluetoothGattService::GattErrorCode::kNotPermitted, {});
  }
  void OnDescriptorWriteRequest(const Device*, const Descriptor*, const std::vector<uint8_t>&, int,
                               base::OnceClosure, ErrorCallback error) override { std::move(error).Run(); }
  void OnNotificationsStart(const Device*, device::BluetoothGattCharacteristic::NotificationType, const Characteristic*) override {}
  void OnNotificationsStop(const Device*, const Characteristic*) override {}
  void AdapterPresentChanged(device::BluetoothAdapter*, bool) override { Stop(); connection.Cancel(); retry_after = {}; }
  void AdapterPoweredChanged(device::BluetoothAdapter*, bool) override { Stop(); connection.Cancel(); retry_after = {}; }
  void AdvertisementReleased(Advertisement* released) override {
    if (released != advertisement.get()) return;
    advertisement->RemoveObserver(this);
    auto held = std::move(advertisement);
    retry_after = base::TimeTicks::Now() + base::Seconds(15);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce([](scoped_refptr<Advertisement>) {}, std::move(held)));
  }
};
BluetoothReceiver::BluetoothReceiver() : impl_(std::make_unique<Impl>()) {}
BluetoothReceiver::~BluetoothReceiver() = default;
void BluetoothReceiver::Update(QuickShareService* service, HotspotConnector hotspot) { impl_->Update(service, std::move(hotspot)); }
#if BUILDFLAG(IS_MAC)
bool BluetoothReceiver::SetAdvertisingPausedForDiscovery(bool paused) {
  return impl_->SetAdvertisingPausedForDiscovery(paused);
}
#endif
}
