// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/ble_wakeup.h"
#include <utility>
#include <vector>
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "device/bluetooth/bluetooth_adapter.h"
#include "device/bluetooth/bluetooth_adapter_factory.h"
#include "device/bluetooth/bluetooth_advertisement.h"

namespace crquickshare {
namespace {
constexpr char kServiceUuid[] = "0000fe2c-0000-1000-8000-00805f9b34fb";
}
struct BleWakeup::Impl : device::BluetoothAdapter::Observer,
                         device::BluetoothAdvertisement::Observer {
  scoped_refptr<device::BluetoothAdapter> adapter;
  scoped_refptr<device::BluetoothAdvertisement> advertisement;
  State current = State::kIdle;
  bool wanted = false, initializing = false, pending = false;
  bool observing_advertisement = false;
  base::TimeTicks retry_after;
  base::WeakPtrFactory<Impl> weak_factory{this};

  Impl() {
    if (!device::BluetoothAdapterFactory::IsBluetoothSupported()) {
      LOG(INFO) << "crQuickShare: BLE unavailable; reason=platform_unsupported";
      return;
    }
    initializing = true;
    device::BluetoothAdapterFactory::Get()->GetAdapter(
        base::BindOnce(&Impl::OnAdapter, weak_factory.GetWeakPtr()));
  }
  ~Impl() override {
    weak_factory.InvalidateWeakPtrs();
    if (observing_advertisement) advertisement->RemoveObserver(this);
    // BlueZ's advertisement destructor unregisters our exported advertisement.
    advertisement.reset();
    if (adapter) adapter->RemoveObserver(this);
  }
  void OnAdapter(scoped_refptr<device::BluetoothAdapter> value) {
    initializing = false; adapter = std::move(value);
    LOG(INFO) << "crQuickShare: BLE adapter ready; present=" << adapter->IsPresent()
              << "; powered=" << adapter->IsPowered();
    adapter->AddObserver(this); Reconcile();
  }
  void Update(bool enabled) {
    if (enabled != wanted) {
      LOG(INFO) << "crQuickShare: BLE request changed; enabled=" << enabled
                << "; adapter_present=" << (adapter && adapter->IsPresent())
                << "; adapter_powered=" << (adapter && adapter->IsPowered())
                << "; pending=" << pending;
      wanted = enabled; retry_after = {};
    }
    Reconcile();
  }
  void Reconcile() {
    if (pending) {
      current = wanted ? State::kStarting : State::kIdle;
      return;
    }
    bool available = adapter && adapter->IsPresent() && adapter->IsPowered();
    if (!wanted || !available) {
      current = !wanted ? State::kIdle : initializing ? State::kStarting : State::kUnavailable;
      if (advertisement) Stop();
      return;
    }
    if (advertisement) { current = State::kAdvertising; return; }
    if (base::TimeTicks::Now() < retry_after) { current = State::kUnavailable; return; }
    LOG(INFO) << "crQuickShare: BLE registration requested; mode=notify";
    current = State::kStarting; pending = true;
    auto data = std::make_unique<device::BluetoothAdvertisement::Data>(
        device::BluetoothAdvertisement::ADVERTISEMENT_TYPE_BROADCAST);
    data->set_service_uuids(device::BluetoothAdvertisement::UUIDList{kServiceUuid});
    // Match Chromium FastInitiationAdvertiser: model FC128E, V1=0,
    // notify type=0 in bits 4..2, no UWB, adjusted TX power encoded as +66.
    // NearbySharingServiceImpl::StartFastInitiationAdvertising uses notify.
    // No device name, file metadata or user identifiers are broadcast.
    device::BluetoothAdvertisement::ServiceData service_data;
    service_data.emplace(kServiceUuid, std::vector<uint8_t>{0xfc, 0x12, 0x8e, 0x00, 0x42});
    data->set_service_data(std::move(service_data));
    adapter->RegisterAdvertisement(std::move(data),
        base::BindOnce(&Impl::OnRegistered, weak_factory.GetWeakPtr()),
        base::BindOnce(&Impl::OnRegisterError, weak_factory.GetWeakPtr()));
  }
  void OnRegistered(scoped_refptr<device::BluetoothAdvertisement> value) {
    pending = false; advertisement = std::move(value);
    advertisement->AddObserver(this);
    observing_advertisement = true;
    LOG(INFO) << "crQuickShare: BLE Fast Initiation registered; mode=notify; service_data=fc128e0042; waiting for LAN discovery";
    // Selection, visibility or connection state may have changed while BlueZ
    // was registering. A late success must not restart an unwanted broadcast.
    Reconcile();
  }
  void OnRegisterError(device::BluetoothAdvertisement::ErrorCode code) {
    pending = false; retry_after = base::TimeTicks::Now() + base::Seconds(15);
    current = wanted ? State::kUnavailable : State::kIdle;
    LOG(WARNING) << "crQuickShare: BLE Fast Initiation registration failed; code=" << static_cast<int>(code);
  }
  void Stop() {
    LOG(INFO) << "crQuickShare: BLE unregister requested";
    pending = true;
    advertisement->RemoveObserver(this);
    observing_advertisement = false;
    advertisement->Unregister(
        base::BindOnce(&Impl::OnStopped, weak_factory.GetWeakPtr()),
        base::BindOnce(&Impl::OnStopError, weak_factory.GetWeakPtr()));
  }
  void OnStopped() {
    LOG(INFO) << "crQuickShare: BLE unregister finished";
    advertisement.reset(); pending = false; Reconcile();
  }
  void OnStopError(device::BluetoothAdvertisement::ErrorCode code) {
    LOG(WARNING) << "crQuickShare: BLE Fast Initiation unregister failed; code=" << static_cast<int>(code);
    retry_after = base::TimeTicks::Now() + base::Seconds(15);
    OnStopped();
  }
  void AdvertisementReleased(device::BluetoothAdvertisement* released) override {
    if (released != advertisement.get()) return;
    LOG(INFO) << "crQuickShare: BLE advertisement released by system; retry_delay_s=15";
    // Keep the advertisement alive until BlueZ finishes notifying observers.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&Impl::OnReleased, weak_factory.GetWeakPtr(), advertisement));
  }
  void OnReleased(scoped_refptr<device::BluetoothAdvertisement> released) {
    if (released != advertisement || pending) return;
    advertisement->RemoveObserver(this); advertisement.reset();
    observing_advertisement = false;
    retry_after = base::TimeTicks::Now() + base::Seconds(15);
    current = wanted ? State::kUnavailable : State::kIdle;
  }
  void AdapterPresentChanged(device::BluetoothAdapter*, bool present) override {
    LOG(INFO) << "crQuickShare: BLE adapter presence changed; present=" << present;
    retry_after = {}; Reconcile();
  }
  void AdapterPoweredChanged(device::BluetoothAdapter*, bool powered) override {
    LOG(INFO) << "crQuickShare: BLE adapter power changed; powered=" << powered;
    retry_after = {}; Reconcile();
  }
};
BleWakeup::BleWakeup() : impl_(std::make_unique<Impl>()) {}
BleWakeup::~BleWakeup() = default;
void BleWakeup::Update(bool enabled) { impl_->Update(enabled); }
BleWakeup::State BleWakeup::state() const { return impl_->current; }
}
