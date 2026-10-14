// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/bluetooth_peripheral_win.h"
#include <windows.devices.bluetooth.h>
#include <windows.devices.bluetooth.genericattributeprofile.h>
#include <windows.storage.streams.h>
#include <wrl/client.h>
#include <utility>
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/win/core_winrt_util.h"
#include "base/win/post_async_results.h"
#include "base/win/scoped_hstring.h"
#include "base/win/winrt_storage_util.h"
namespace crquickshare {
namespace {
namespace gatt = ABI::Windows::Devices::Bluetooth::GenericAttributeProfile;
using ABI::Windows::Devices::Bluetooth::BluetoothError;
using ABI::Windows::Devices::Bluetooth::BluetoothError_Success;
using ABI::Windows::Foundation::IAsyncOperation;
using ABI::Windows::Storage::Streams::IBuffer;
using Microsoft::WRL::ComPtr;
constexpr GUID kService = {0xfef3, 0, 0x1000, {0x80, 0, 0, 0x80, 0x5f, 0x9b, 0x34, 0xfb}};
constexpr GUID kSlot = {0, 0, 0x3000, {0x80, 0, 0, 0, 0, 0, 0, 0}};
template <typename T>
HRESULT Activate(const wchar_t* name, ComPtr<T>& result) {
  auto id = base::win::ScopedHString::Create(name);
  ComPtr<IInspectable> instance;
  HRESULT hr = base::win::RoActivateInstance(id.get(), &instance);
  return FAILED(hr) ? hr : instance.As(&result);
}
}
struct BluetoothPeripheralWin::Impl {
  ComPtr<gatt::IGattServiceProvider> provider;
  ComPtr<gatt::IGattLocalCharacteristic> slot;
  std::vector<uint8_t> value;
  base::OnceCallback<void(bool)> completion;
  base::RepeatingTimer timer;
  base::TimeTicks deadline;
  base::WeakPtrFactory<Impl> weak{this};
  ~Impl() { Stop(); }
  void Complete(bool success, const char* stage) {
    timer.Stop();
    LOG(INFO) << "crQuickShare: Windows GATT setup; stage=" << stage << "; success=" << success;
    auto callback = std::move(completion);
    if (!success && provider) provider->StopAdvertising();
    if (callback) std::move(callback).Run(success);
  }
  void Stop() {
    weak.InvalidateWeakPtrs(); timer.Stop(); completion.Reset();
    if (provider) provider->StopAdvertising();
    slot.Reset(); provider.Reset(); value.clear();
  }
  void Prepare(const std::vector<uint8_t>& bytes, base::OnceCallback<void(bool)> callback) {
    Stop(); value = bytes; completion = std::move(callback);
    deadline = base::TimeTicks::Now() + base::Seconds(10);
    timer.Start(FROM_HERE, base::Milliseconds(100), base::BindRepeating([](base::WeakPtr<Impl> self) {
      if (self && base::TimeTicks::Now() >= self->deadline) {
        self->weak.InvalidateWeakPtrs(); self->Complete(false, "prepare_timeout");
      }
    }, weak.GetWeakPtr()));
    ComPtr<gatt::IGattServiceProviderStatics> factory;
    HRESULT hr = base::win::GetActivationFactory<gatt::IGattServiceProviderStatics,
        RuntimeClass_Windows_Devices_Bluetooth_GenericAttributeProfile_GattServiceProvider>(&factory);
    ComPtr<IAsyncOperation<gatt::GattServiceProviderResult*>> operation;
    if (SUCCEEDED(hr)) hr = factory->CreateAsync(kService, &operation);
    if (SUCCEEDED(hr)) hr = base::win::PostAsyncResults(std::move(operation),
        base::BindOnce(&Impl::ServiceCreated, weak.GetWeakPtr()));
    if (FAILED(hr)) { LOG(WARNING) << "crQuickShare: Windows GATT create service failed; hr=" << hr; Complete(false, "create_service"); }
  }
  void ServiceCreated(ComPtr<gatt::IGattServiceProviderResult> result) {
    BluetoothError error;
    if (!result || FAILED(result->get_Error(&error)) || error != BluetoothError_Success ||
        FAILED(result->get_ServiceProvider(&provider)) || !provider) {
      Complete(false, "service_result"); return;
    }
    ComPtr<gatt::IGattLocalCharacteristicParameters> parameters;
    ComPtr<IBuffer> buffer;
    ComPtr<gatt::IGattLocalService> service;
    HRESULT hr = Activate(RuntimeClass_Windows_Devices_Bluetooth_GenericAttributeProfile_GattLocalCharacteristicParameters, parameters);
    if (SUCCEEDED(hr)) hr = base::win::CreateIBufferFromData(value, &buffer);
    if (SUCCEEDED(hr)) hr = parameters->put_StaticValue(buffer.Get());
    if (SUCCEEDED(hr)) hr = parameters->put_CharacteristicProperties(gatt::GattCharacteristicProperties_Read);
    if (SUCCEEDED(hr)) hr = parameters->put_ReadProtectionLevel(gatt::GattProtectionLevel_Plain);
    if (SUCCEEDED(hr)) hr = provider->get_Service(&service);
    ComPtr<IAsyncOperation<gatt::GattLocalCharacteristicResult*>> operation;
    if (SUCCEEDED(hr)) hr = service->CreateCharacteristicAsync(kSlot, parameters.Get(), &operation);
    if (SUCCEEDED(hr)) hr = base::win::PostAsyncResults(std::move(operation),
        base::BindOnce(&Impl::SlotCreated, weak.GetWeakPtr()));
    if (FAILED(hr)) { LOG(WARNING) << "crQuickShare: Windows GATT create slot failed; hr=" << hr; Complete(false, "create_slot"); }
  }
  void SlotCreated(ComPtr<gatt::IGattLocalCharacteristicResult> result) {
    BluetoothError error;
    const bool ok = result && SUCCEEDED(result->get_Error(&error)) && error == BluetoothError_Success &&
        SUCCEEDED(result->get_Characteristic(&slot)) && slot;
    Complete(ok, "slot_ready");
  }
  bool Advertising() const {
    gatt::GattServiceProviderAdvertisementStatus status;
    return provider && SUCCEEDED(provider->get_AdvertisementStatus(&status)) &&
        status == gatt::GattServiceProviderAdvertisementStatus_Started;
  }
  void Advertise(const std::vector<uint8_t>& header, base::OnceCallback<void(bool)> callback) {
    completion = std::move(callback);
    if (!provider || !slot) { Complete(false, "not_ready"); return; }
    ComPtr<gatt::IGattServiceProviderAdvertisingParameters> parameters;
    ComPtr<gatt::IGattServiceProviderAdvertisingParameters2> data_parameters;
    ComPtr<IBuffer> buffer;
    HRESULT hr = Activate(RuntimeClass_Windows_Devices_Bluetooth_GenericAttributeProfile_GattServiceProviderAdvertisingParameters, parameters);
    if (SUCCEEDED(hr)) hr = parameters->put_IsConnectable(true);
    if (SUCCEEDED(hr)) hr = parameters->put_IsDiscoverable(true);
    if (SUCCEEDED(hr)) hr = parameters.As(&data_parameters);
    if (SUCCEEDED(hr)) hr = base::win::CreateIBufferFromData(header, &buffer);
    if (SUCCEEDED(hr)) hr = data_parameters->put_ServiceData(buffer.Get());
    if (SUCCEEDED(hr)) hr = provider->StartAdvertisingWithParameters(parameters.Get());
    if (FAILED(hr)) { LOG(WARNING) << "crQuickShare: Windows GATT advertisement failed; hr=" << hr; Complete(false, "start_advertising"); return; }
    deadline = base::TimeTicks::Now() + base::Seconds(5);
    timer.Start(FROM_HERE, base::Milliseconds(100), base::BindRepeating([](base::WeakPtr<Impl> self) {
      if (!self) return;
      gatt::GattServiceProviderAdvertisementStatus status;
      if (self->Advertising()) { self->Complete(true, "advertising"); return; }
      const HRESULT hr = self->provider->get_AdvertisementStatus(&status);
      if (FAILED(hr) || status == gatt::GattServiceProviderAdvertisementStatus_Aborted ||
          status == gatt::GattServiceProviderAdvertisementStatus_StartedWithoutAllAdvertisementData ||
          base::TimeTicks::Now() >= self->deadline) {
        LOG(WARNING) << "crQuickShare: Windows GATT advertisement incomplete; hr=" << hr
                     << "; status=" << (SUCCEEDED(hr) ? static_cast<int>(status) : -1);
        self->Complete(false, "advertising_failed");
      }
    }, weak.GetWeakPtr()));
  }
};
BluetoothPeripheralWin::BluetoothPeripheralWin() : impl_(std::make_unique<Impl>()) {}
BluetoothPeripheralWin::~BluetoothPeripheralWin() = default;
void BluetoothPeripheralWin::Prepare(const std::vector<uint8_t>& value, base::OnceCallback<void(bool)> ready) { impl_->Prepare(value, std::move(ready)); }
void BluetoothPeripheralWin::Advertise(const std::vector<uint8_t>& header, base::OnceCallback<void(bool)> ready) { impl_->Advertise(header, std::move(ready)); }
void BluetoothPeripheralWin::Withdraw() {
  impl_->timer.Stop(); impl_->completion.Reset();
  if (impl_->provider) impl_->provider->StopAdvertising();
}
void BluetoothPeripheralWin::Stop() { impl_->Stop(); }
bool BluetoothPeripheralWin::ready() const { return impl_->slot != nullptr; }
bool BluetoothPeripheralWin::advertising() const { return impl_->Advertising(); }
}  // namespace crquickshare
