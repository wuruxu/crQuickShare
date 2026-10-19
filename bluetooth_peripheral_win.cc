// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/bluetooth_peripheral_win.h"
#include <windows.devices.bluetooth.h>
#include <windows.devices.bluetooth.advertisement.h>
#include <windows.foundation.collections.h>
#include <windows.devices.bluetooth.genericattributeprofile.h>
#include <windows.storage.streams.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <optional>
#include <string>
#include <utility>
#include "base/functional/bind.h"
#include "base/base64url.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/win/core_winrt_util.h"
#include "base/win/post_async_results.h"
#include "base/win/scoped_hstring.h"
#include "base/win/winrt_storage_util.h"
namespace crquickshare {
namespace {
namespace bt = ABI::Windows::Devices::Bluetooth;
namespace ad = ABI::Windows::Devices::Bluetooth::Advertisement;
namespace gatt = ABI::Windows::Devices::Bluetooth::GenericAttributeProfile;
using ABI::Windows::Devices::Bluetooth::BluetoothError;
using ABI::Windows::Devices::Bluetooth::BluetoothError_Success;
using ABI::Windows::Foundation::IAsyncOperation;
using ABI::Windows::Storage::Streams::IBuffer;
using Microsoft::WRL::ComPtr;
const char* BluetoothErrorName(BluetoothError error) {
  using namespace ABI::Windows::Devices::Bluetooth;
  switch (error) {
    case BluetoothError_Success: return "success";
    case BluetoothError_RadioNotAvailable: return "radio_not_available";
    case BluetoothError_ResourceInUse: return "resource_in_use";
    case BluetoothError_DeviceNotConnected: return "device_not_connected";
    case BluetoothError_OtherError: return "other_error";
    case BluetoothError_DisabledByPolicy: return "disabled_by_policy";
    case BluetoothError_NotSupported: return "not_supported";
    case BluetoothError_DisabledByUser: return "disabled_by_user";
    case BluetoothError_ConsentRequired: return "consent_required";
    case BluetoothError_TransportNotSupported: return "transport_not_supported";
    default: return "unknown";
  }
}
const char* AdvertisementStatusName(gatt::GattServiceProviderAdvertisementStatus status) {
  switch (status) {
    case gatt::GattServiceProviderAdvertisementStatus_Created: return "created";
    case gatt::GattServiceProviderAdvertisementStatus_Stopped: return "stopped";
    case gatt::GattServiceProviderAdvertisementStatus_Started: return "started";
    case gatt::GattServiceProviderAdvertisementStatus_Aborted: return "aborted";
    case gatt::GattServiceProviderAdvertisementStatus_StartedWithoutAllAdvertisementData: return "started_without_all_data";
    default: return "unknown";
  }
}
// UI-sequence retirement keeps COM objects and their status handlers alive
// after the receiver is destroyed. Event callbacks never capture the owner.
int pending_stops = 0;
struct AdvertisementStop : std::enable_shared_from_this<AdvertisementStop> {
  ComPtr<ad::IBluetoothLEAdvertisementPublisher> publisher;
  ComPtr<gatt::IGattServiceProvider> provider;
  std::optional<EventRegistrationToken> token;
  base::TimeTicks started = base::TimeTicks::Now();
  bool done = false;
  bool confirmed = false;
  void Poll() {
    int status = -1;
    HRESULT hr;
    if (publisher) {
      ad::BluetoothLEAdvertisementPublisherStatus state;
      hr = publisher->get_Status(&state);
      if (SUCCEEDED(hr)) {
        status = static_cast<int>(state);
        confirmed = state == ad::BluetoothLEAdvertisementPublisherStatus_Stopped ||
            state == ad::BluetoothLEAdvertisementPublisherStatus_Aborted ||
            state == ad::BluetoothLEAdvertisementPublisherStatus_Created;
      }
    } else {
      gatt::GattServiceProviderAdvertisementStatus state;
      hr = provider->get_AdvertisementStatus(&state);
      if (SUCCEEDED(hr)) {
        status = static_cast<int>(state);
        confirmed = state == gatt::GattServiceProviderAdvertisementStatus_Stopped ||
            state == gatt::GattServiceProviderAdvertisementStatus_Aborted ||
            state == gatt::GattServiceProviderAdvertisementStatus_Created;
      }
    }
    const auto elapsed = base::TimeTicks::Now() - started;
    if (!confirmed && elapsed < base::Seconds(2)) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE, base::BindOnce([](std::shared_ptr<AdvertisementStop> self) { self->Poll(); },
                                   shared_from_this()), base::Milliseconds(50));
      return;
    }
    LOG(INFO) << "crQuickShare: Windows advertisement stop finished; kind="
              << (publisher ? "extended" : "gatt") << "; confirmed=" << confirmed
              << "; timed_out=" << !confirmed << "; status=" << status
              << "; status_hr=" << hr << "; elapsed_ms=" << elapsed.InMilliseconds();
    if (token) {
      const HRESULT remove_hr = publisher ? publisher->remove_StatusChanged(*token) :
          provider->remove_AdvertisementStatusChanged(*token);
      LOG(INFO) << "crQuickShare: Windows advertisement status handler removed; hr=" << remove_hr;
    }
    token.reset(); publisher.Reset(); provider.Reset();
    done = true;
    --pending_stops;
  }
  void Start() {
    ++pending_stops;
    const HRESULT hr = publisher ? publisher->Stop() : provider->StopAdvertising();
    LOG(INFO) << "crQuickShare: Windows advertisement stop requested; kind="
              << (publisher ? "extended" : "gatt") << "; hr=" << hr;
    Poll();
  }
};
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
  std::optional<EventRegistrationToken> advertisement_status_token;
  ComPtr<ad::IBluetoothLEAdvertisementPublisher> publisher;
  std::optional<EventRegistrationToken> publisher_status_token;
  std::shared_ptr<AdvertisementStop> publisher_stop, gatt_stop;
  bool extended_supported = false;
  bool gatt_attempted = false;
  bool gatt_start_requested = false;
  std::vector<uint8_t> value, pending_header;
  base::TimeTicks gatt_started;
  base::OnceCallback<void(bool)> completion;
  base::RepeatingTimer timer;
  base::TimeTicks deadline;
  base::WeakPtrFactory<Impl> weak{this};
  ~Impl() { Stop(); }
  void Complete(bool success, const char* stage) {
    timer.Stop();
    LOG(INFO) << "crQuickShare: Windows GATT setup; stage=" << stage << "; success=" << success;
    auto callback = std::move(completion);
    if (!success) StopGatt();
    if (callback) std::move(callback).Run(success);
  }
  void StopPublisher() {
    if (!publisher) return;
    publisher_stop = std::make_shared<AdvertisementStop>();
    publisher_stop->publisher = std::move(publisher);
    publisher_stop->token = std::exchange(publisher_status_token, std::nullopt);
    ad::BluetoothLEAdvertisementPublisherStatus status;
    if (SUCCEEDED(publisher_stop->publisher->get_Status(&status)) &&
        (status == ad::BluetoothLEAdvertisementPublisherStatus_Created ||
         status == ad::BluetoothLEAdvertisementPublisherStatus_Stopped ||
         status == ad::BluetoothLEAdvertisementPublisherStatus_Aborted)) {
      ++pending_stops;
      publisher_stop->Poll();
    } else {
      publisher_stop->Start();
    }
  }
  void StopGatt() {
    if (!provider || !gatt_start_requested) return;
    gatt_start_requested = false;
    gatt_stop = std::make_shared<AdvertisementStop>();
    gatt_stop->provider = provider;
    gatt_stop->token = std::exchange(advertisement_status_token, std::nullopt);
    gatt::GattServiceProviderAdvertisementStatus status;
    if (SUCCEEDED(provider->get_AdvertisementStatus(&status)) &&
        (status == gatt::GattServiceProviderAdvertisementStatus_Stopped ||
         status == gatt::GattServiceProviderAdvertisementStatus_Aborted)) {
      ++pending_stops;
      gatt_stop->Poll();
    } else {
      gatt_stop->Start();
    }
  }
  bool StopsPending() const {
    return (publisher_stop && !publisher_stop->done) || (gatt_stop && !gatt_stop->done);
  }
  void Withdraw() {
    timer.Stop(); completion.Reset();
    StopPublisher(); StopGatt();
    pending_header.clear(); gatt_attempted = false;
  }
  void Stop() {
    weak.InvalidateWeakPtrs(); Withdraw();
    extended_supported = false;
    if (provider && advertisement_status_token)
      provider->remove_AdvertisementStatusChanged(*advertisement_status_token);
    advertisement_status_token.reset();
    slot.Reset(); provider.Reset(); value.clear();
    LOG(INFO) << "crQuickShare: Windows peripheral resources retired; stop_pending=" << StopsPending();
  }
  void Prepare(const std::vector<uint8_t>& bytes, base::OnceCallback<void(bool)> callback) {
    Stop();
    // A new provider can retry after a retired generation timed out. Pending
    // retirements still serialize the first advertisement of this generation.
    if (publisher_stop && publisher_stop->done) publisher_stop.reset();
    if (gatt_stop && gatt_stop->done) gatt_stop.reset();
    value = bytes; completion = std::move(callback);
    deadline = base::TimeTicks::Now() + base::Seconds(10);
    timer.Start(FROM_HERE, base::Milliseconds(100), base::BindRepeating([](base::WeakPtr<Impl> self) {
      if (self && base::TimeTicks::Now() >= self->deadline) {
        auto* impl = self.get();
        impl->weak.InvalidateWeakPtrs(); impl->FinishPrepare(false, "prepare_timeout");
      }
    }, weak.GetWeakPtr()));
    ComPtr<bt::IBluetoothAdapterStatics> adapter_factory;
    HRESULT hr = base::win::GetActivationFactory<bt::IBluetoothAdapterStatics,
        RuntimeClass_Windows_Devices_Bluetooth_BluetoothAdapter>(&adapter_factory);
    ComPtr<IAsyncOperation<bt::BluetoothAdapter*>> operation;
    if (SUCCEEDED(hr)) hr = adapter_factory->GetDefaultAsync(&operation);
    if (SUCCEEDED(hr)) hr = base::win::PostAsyncResults(std::move(operation),
        base::BindOnce(&Impl::AdapterReady, weak.GetWeakPtr()));
    if (FAILED(hr)) {
      LOG(WARNING) << "crQuickShare: Windows advertising capability query failed; hr=" << hr;
      PrepareGatt();
    }
  }
  void AdapterReady(ComPtr<bt::IBluetoothAdapter> adapter) {
    ComPtr<bt::IBluetoothAdapter3> extended_adapter;
    boolean supported = false;
    HRESULT hr = adapter ? adapter.As(&extended_adapter) : E_POINTER;
    if (SUCCEEDED(hr)) hr = extended_adapter->get_IsExtendedAdvertisingSupported(&supported);
    extended_supported = SUCCEEDED(hr) && supported;
    LOG(INFO) << "crQuickShare: Windows advertising capabilities; extended_supported="
              << extended_supported << "; hr=" << hr;
    PrepareGatt();
  }
  void FinishPrepare(bool gatt_ready, const char* stage) {
    LOG(INFO) << "crQuickShare: Windows receiver preparation; stage=" << stage
              << "; gatt_ready=" << gatt_ready << "; extended_available=" << extended_supported;
    if (!gatt_ready) slot.Reset();
    Complete(gatt_ready || extended_supported, "receiver_ready");
  }
  void PrepareGatt() {
    ComPtr<gatt::IGattServiceProviderStatics> factory;
    HRESULT hr = base::win::GetActivationFactory<gatt::IGattServiceProviderStatics,
        RuntimeClass_Windows_Devices_Bluetooth_GenericAttributeProfile_GattServiceProvider>(&factory);
    ComPtr<IAsyncOperation<gatt::GattServiceProviderResult*>> operation;
    if (SUCCEEDED(hr)) hr = factory->CreateAsync(kService, &operation);
    if (SUCCEEDED(hr)) hr = base::win::PostAsyncResults(std::move(operation),
        base::BindOnce(&Impl::ServiceCreated, weak.GetWeakPtr()));
    if (FAILED(hr)) { LOG(WARNING) << "crQuickShare: Windows GATT create service failed; hr=" << hr; FinishPrepare(false, "create_service"); }
  }
  void ServiceCreated(ComPtr<gatt::IGattServiceProviderResult> result) {
    BluetoothError error;
    if (!result || FAILED(result->get_Error(&error)) || error != BluetoothError_Success ||
        FAILED(result->get_ServiceProvider(&provider)) || !provider) {
      FinishPrepare(false, "service_result"); return;
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
    if (FAILED(hr)) { LOG(WARNING) << "crQuickShare: Windows GATT create slot failed; hr=" << hr; FinishPrepare(false, "create_slot"); }
  }
  void SlotCreated(ComPtr<gatt::IGattLocalCharacteristicResult> result) {
    BluetoothError error;
    const bool ok = result && SUCCEEDED(result->get_Error(&error)) && error == BluetoothError_Success &&
        SUCCEEDED(result->get_Characteristic(&slot)) && slot;
    FinishPrepare(ok, "slot_ready");
  }
  bool ExtendedAdvertising() const {
    ad::BluetoothLEAdvertisementPublisherStatus status;
    return publisher && SUCCEEDED(publisher->get_Status(&status)) &&
        status == ad::BluetoothLEAdvertisementPublisherStatus_Started;
  }
  bool GattAdvertising() const {
    gatt::GattServiceProviderAdvertisementStatus status;
    return provider && slot && SUCCEEDED(provider->get_AdvertisementStatus(&status)) &&
        status == gatt::GattServiceProviderAdvertisementStatus_Started;
  }
  bool Advertising() const { return ExtendedAdvertising() || GattAdvertising(); }
  void StartExtended() {
    if (!extended_supported || value.empty()) return;
    ComPtr<ad::IBluetoothLEAdvertisement> advertisement;
    HRESULT hr = Activate(RuntimeClass_Windows_Devices_Bluetooth_Advertisement_BluetoothLEAdvertisement, advertisement);
    ComPtr<ABI::Windows::Foundation::Collections::IVector<ad::BluetoothLEAdvertisementDataSection*>> sections;
    if (SUCCEEDED(hr)) hr = advertisement->get_DataSections(&sections);
    ComPtr<ad::IBluetoothLEAdvertisementDataSectionFactory> section_factory;
    if (SUCCEEDED(hr)) hr = base::win::GetActivationFactory<ad::IBluetoothLEAdvertisementDataSectionFactory,
        RuntimeClass_Windows_Devices_Bluetooth_Advertisement_BluetoothLEAdvertisementDataSection>(&section_factory);
    // Nearby Windows emits AD type 0x16: little-endian FEF3 followed by the
    // complete medium advertisement, not the short GATT discovery header.
    std::vector<uint8_t> data{0xf3, 0xfe};
    data.insert(data.end(), value.begin(), value.end());
    ComPtr<IBuffer> buffer;
    ComPtr<ad::IBluetoothLEAdvertisementDataSection> section;
    if (SUCCEEDED(hr)) hr = base::win::CreateIBufferFromData(data, &buffer);
    if (SUCCEEDED(hr)) hr = section_factory->Create(0x16, buffer.Get(), &section);
    if (SUCCEEDED(hr)) hr = sections->Append(section.Get());
    ComPtr<ad::IBluetoothLEAdvertisementPublisherFactory> publisher_factory;
    if (SUCCEEDED(hr)) hr = base::win::GetActivationFactory<ad::IBluetoothLEAdvertisementPublisherFactory,
        RuntimeClass_Windows_Devices_Bluetooth_Advertisement_BluetoothLEAdvertisementPublisher>(&publisher_factory);
    if (SUCCEEDED(hr)) hr = publisher_factory->Create(advertisement.Get(), &publisher);
    ComPtr<ad::IBluetoothLEAdvertisementPublisher2> extended_publisher;
    if (SUCCEEDED(hr)) hr = publisher.As(&extended_publisher);
    if (SUCCEEDED(hr)) hr = extended_publisher->put_UseExtendedAdvertisement(true);
    if (SUCCEEDED(hr)) {
      EventRegistrationToken token{};
      hr = publisher->add_StatusChanged(
          Microsoft::WRL::Callback<ABI::Windows::Foundation::ITypedEventHandler<
              ad::BluetoothLEAdvertisementPublisher*, ad::BluetoothLEAdvertisementPublisherStatusChangedEventArgs*>>(
              [](ad::IBluetoothLEAdvertisementPublisher*, ad::IBluetoothLEAdvertisementPublisherStatusChangedEventArgs* args) -> HRESULT {
                BluetoothError error = BluetoothError_Success;
                ad::BluetoothLEAdvertisementPublisherStatus status = ad::BluetoothLEAdvertisementPublisherStatus_Created;
                const HRESULT status_hr = args ? args->get_Status(&status) : E_POINTER;
                const HRESULT error_hr = args ? args->get_Error(&error) : E_POINTER;
                LOG(INFO) << "crQuickShare: Windows extended advertisement status changed; status="
                          << (SUCCEEDED(status_hr) ? static_cast<int>(status) : -1)
                          << "; error=" << (SUCCEEDED(error_hr) ? static_cast<int>(error) : -1)
                          << "; reason=" << (SUCCEEDED(error_hr) ? BluetoothErrorName(error) : "error_query_failed")
                          << "; status_hr=" << status_hr << "; error_hr=" << error_hr;
                return S_OK;
              }).Get(), &token);
      if (SUCCEEDED(hr)) publisher_status_token = token;
    }
    if (SUCCEEDED(hr)) hr = publisher->Start();
    LOG(INFO) << "crQuickShare: Windows extended advertising requested; service_data_bytes="
              << value.size() << "; hr=" << hr;
    if (FAILED(hr)) StopPublisher();
  }
  void Advertise(const std::vector<uint8_t>& header, base::OnceCallback<void(bool)> callback) {
    Withdraw();
    pending_header = header;
    completion = std::move(callback);
    deadline = base::TimeTicks::Now() + base::Seconds(5);
    timer.Start(FROM_HERE, base::Milliseconds(50), this, &Impl::WaitForStop);
    WaitForStop();
  }
  void WaitForStop() {
    if (StopsPending()) return;
    if ((publisher_stop && !publisher_stop->confirmed) ||
        (gatt_stop && !gatt_stop->confirmed)) {
      Complete(false, "previous_advertisement_stop_unconfirmed");
      return;
    }
    publisher_stop.reset(); gatt_stop.reset();
    StartExtended();
    deadline = base::TimeTicks::Now() + base::Seconds(5);
    timer.Start(FROM_HERE, base::Milliseconds(100), this, &Impl::PollAdvertising);
  }
  void PollAdvertising() {
    const auto now = base::TimeTicks::Now();
    if (!gatt_attempted) {
      ad::BluetoothLEAdvertisementPublisherStatus status = ad::BluetoothLEAdvertisementPublisherStatus_Created;
      const bool extended_terminal = !publisher || FAILED(publisher->get_Status(&status)) ||
          status == ad::BluetoothLEAdvertisementPublisherStatus_Started ||
          status == ad::BluetoothLEAdvertisementPublisherStatus_Aborted ||
          status == ad::BluetoothLEAdvertisementPublisherStatus_Stopped;
      if (!extended_terminal && now < deadline) return;
      if (!ExtendedAdvertising()) StopPublisher();
      gatt_attempted = true;
      gatt_started = now;
      deadline = now + base::Seconds(5);
      // Advertise extended support only after the publisher actually started.
      std::string decoded;
      const std::string encoded(pending_header.begin(), pending_header.end());
      if (ExtendedAdvertising() && base::Base64UrlDecode(encoded,
          base::Base64UrlDecodePolicy::IGNORE_PADDING, &decoded) && !decoded.empty()) {
        decoded[0] |= 0x10;
        std::string updated;
        base::Base64UrlEncode(decoded, base::Base64UrlEncodePolicy::OMIT_PADDING, &updated);
        pending_header.assign(updated.begin(), updated.end());
      }
      StartGatt();
    }
    gatt::GattServiceProviderAdvertisementStatus status = gatt::GattServiceProviderAdvertisementStatus_Created;
    const HRESULT hr = provider ? provider->get_AdvertisementStatus(&status) : E_POINTER;
    const bool complete_gatt = gatt_start_requested && SUCCEEDED(hr) &&
        status == gatt::GattServiceProviderAdvertisementStatus_Started;
    const bool partial_gatt = gatt_start_requested && SUCCEEDED(hr) &&
        status == gatt::GattServiceProviderAdvertisementStatus_StartedWithoutAllAdvertisementData;
    // Allow a short settling window for Windows' asynchronous state changes.
    const bool terminal_gatt = !gatt_start_requested || FAILED(hr) || complete_gatt || partial_gatt ||
        (status == gatt::GattServiceProviderAdvertisementStatus_Aborted && now - gatt_started >= base::Milliseconds(500));
    if (!terminal_gatt && now < deadline) return;
    const bool extended = ExtendedAdvertising();
    LOG(INFO) << "crQuickShare: Windows receiver advertising result; extended=" << extended
              << "; gatt=" << complete_gatt << "; gatt_partial=" << partial_gatt
              << "; gatt_status=" << (SUCCEEDED(hr) ? static_cast<int>(status) : -1)
              << "; gatt_hr=" << hr << "; success=" << (extended || complete_gatt);
    // A partial GATT advertisement cannot prove the discovery header is on air.
    // It is useful as a companion to the full extended advertisement only.
    if (!complete_gatt && !(partial_gatt && extended)) StopGatt();
    if (!extended) StopPublisher();
    Complete(extended || complete_gatt, "advertising");
  }
  void StartGatt() {
    if (!provider || !slot) return;
    const auto& header = pending_header;
    if (!advertisement_status_token) {
      EventRegistrationToken token{};
      // Log directly in the event callback so teardown after a polling failure
      // cannot discard the error. Capture no owner state: WinRT may invoke this
      // on another thread, including while the provider is being stopped.
      const HRESULT event_hr = provider->add_AdvertisementStatusChanged(
          Microsoft::WRL::Callback<ABI::Windows::Foundation::ITypedEventHandler<
              gatt::GattServiceProvider*, gatt::GattServiceProviderAdvertisementStatusChangedEventArgs*>>(
              [](gatt::IGattServiceProvider*, gatt::IGattServiceProviderAdvertisementStatusChangedEventArgs* args) -> HRESULT {
                BluetoothError error = BluetoothError_Success;
                gatt::GattServiceProviderAdvertisementStatus status = gatt::GattServiceProviderAdvertisementStatus_Created;
                const HRESULT status_hr = args ? args->get_Status(&status) : E_POINTER;
                const HRESULT error_hr = args ? args->get_Error(&error) : E_POINTER;
                LOG(INFO) << "crQuickShare: Windows GATT advertisement status changed; status="
                          << (SUCCEEDED(status_hr) ? static_cast<int>(status) : -1)
                          << "; status_name=" << (SUCCEEDED(status_hr) ? AdvertisementStatusName(status) : "unavailable")
                          << "; error=" << (SUCCEEDED(error_hr) ? static_cast<int>(error) : -1)
                          << "; reason=" << (SUCCEEDED(error_hr) ? BluetoothErrorName(error) : "error_query_failed")
                          << "; status_hr=" << status_hr << "; error_hr=" << error_hr;
                return S_OK;
              }).Get(), &token);
      if (SUCCEEDED(event_hr)) advertisement_status_token = token;
      else LOG(WARNING) << "crQuickShare: Windows GATT status handler registration failed; hr=" << event_hr;
    }
    LOG(INFO) << "crQuickShare: Windows GATT advertising requested; service_data_bytes="
              << header.size() << "; connectable=1; discoverable=1";
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
    gatt_start_requested = SUCCEEDED(hr);
    if (FAILED(hr)) LOG(WARNING) << "crQuickShare: Windows GATT advertisement failed; hr=" << hr;
  }
};
void BluetoothPeripheralWin::DrainPendingStops() {
  if (!pending_stops) return;
  LOG(INFO) << "crQuickShare shutdown: stage=advertisement_stops; event=started; pending=" << pending_stops;
  base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
  base::RepeatingTimer poll;
  const auto deadline = base::TimeTicks::Now() + base::Milliseconds(2500);
  poll.Start(FROM_HERE, base::Milliseconds(50), base::BindRepeating(
      [](base::TimeTicks stop_deadline, base::RepeatingClosure quit) {
        if (!pending_stops || base::TimeTicks::Now() >= stop_deadline) quit.Run();
      }, deadline, loop.QuitClosure()));
  loop.Run();
  LOG(INFO) << "crQuickShare shutdown: stage=advertisement_stops; event=finished; pending=" << pending_stops;
}
BluetoothPeripheralWin::BluetoothPeripheralWin() : impl_(std::make_unique<Impl>()) {}
BluetoothPeripheralWin::~BluetoothPeripheralWin() = default;
void BluetoothPeripheralWin::Prepare(const std::vector<uint8_t>& value, base::OnceCallback<void(bool)> ready) { impl_->Prepare(value, std::move(ready)); }
void BluetoothPeripheralWin::Advertise(const std::vector<uint8_t>& header, base::OnceCallback<void(bool)> ready) { impl_->Advertise(header, std::move(ready)); }
void BluetoothPeripheralWin::Withdraw() { impl_->Withdraw(); }
void BluetoothPeripheralWin::Stop() { impl_->Stop(); }
bool BluetoothPeripheralWin::ready() const { return !impl_->value.empty() && (impl_->slot != nullptr || impl_->extended_supported); }
bool BluetoothPeripheralWin::advertising() const { return impl_->Advertising(); }
}  // namespace crquickshare
