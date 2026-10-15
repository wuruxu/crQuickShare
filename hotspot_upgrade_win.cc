// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/hotspot_upgrade.h"
#include "crQuickShare/platform_socket.h"
#include <windows.h>
#include <iphlpapi.h>
#include <wlanapi.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include "base/compiler_specific.h"
#include "base/logging.h"
#include "base/native_library.h"
#include "base/scoped_native_library.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"

namespace crquickshare {
namespace {
using KeepGoing = std::function<bool()>;
void Pause() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
bool Ok(DWORD error, const char* operation) {
  if (error == ERROR_SUCCESS) return true;
  // Never print SSIDs, profile XML or keys, including in OS error messages.
  LOG(WARNING) << "crQuickShare: Windows hotspot error; operation=" << operation << "; code=" << error;
  return false;
}
template <size_t N>
std::wstring Name(const WCHAR (&value)[N]) {
  return std::wstring(std::begin(value), std::find(std::begin(value), std::end(value), L'\0'));
}
std::string XmlText(const std::string& value) {
  std::string result;
  for (char c : value) {
    switch (c) {
      case '&': result += "&amp;"; break;
      case '<': result += "&lt;"; break;
      case '>': result += "&gt;"; break;
      case '\"': result += "&quot;"; break;
      case '\'': result += "&apos;"; break;
      default: result += c;
    }
  }
  return result;
}
// Load only from System32. LAN/Bluetooth remain usable when WLAN is absent.
struct WlanApi {
  base::ScopedNativeLibrary library{base::LoadSystemLibrary(L"wlanapi.dll", nullptr)};
  decltype(&WlanOpenHandle) open = Load<decltype(open)>("WlanOpenHandle");
  decltype(&WlanCloseHandle) close = Load<decltype(close)>("WlanCloseHandle");
  decltype(&WlanEnumInterfaces) enumerate = Load<decltype(enumerate)>("WlanEnumInterfaces");
  decltype(&WlanQueryInterface) query = Load<decltype(query)>("WlanQueryInterface");
  decltype(&WlanFreeMemory) free = Load<decltype(free)>("WlanFreeMemory");
  decltype(&WlanConnect) connect = Load<decltype(connect)>("WlanConnect");
  decltype(&WlanDisconnect) disconnect = Load<decltype(disconnect)>("WlanDisconnect");
  decltype(&WlanRegisterNotification) notify = Load<decltype(notify)>("WlanRegisterNotification");
  template <typename T> T Load(const char* name) {
    return reinterpret_cast<T>(library.GetFunctionPointer(name));
  }
  bool ready() const { return open && close && enumerate && query && free && connect && disconnect && notify; }
};

// All WLAN operations run on the session worker. Notifications only copy state
// under a lock; they never call WLAN APIs or touch the protocol/UI sequence.
class Network final : public UpgradeNetwork {
 public:
  ~Network() override {
    if (requested_ && !cleaned_) Cleanup(base::TimeTicks::Now() + base::Seconds(1), [] { return false; });
    if (client_) {
      api_.notify(client_, WLAN_NOTIFICATION_SOURCE_NONE, FALSE, nullptr, nullptr, nullptr, nullptr);
      api_.close(client_, nullptr);
    }
  }
  bool Open(const std::string& requested);
  bool SavePrevious();
  bool Start(const HotspotCredentials& credentials);
  bool WaitConnected(base::TimeTicks deadline, const KeepGoing& active);
  bool Cleanup(base::TimeTicks deadline, const KeepGoing& keep_restoring) override;
  bool Current(std::optional<WLAN_CONNECTION_ATTRIBUTES>& result);
  bool Owned(const WLAN_CONNECTION_ATTRIBUTES& current) const {
    return current.wlanConnectionMode == wlan_connection_mode_temporary_profile && Name(current.strProfileName) == profile_;
  }
  bool Superseded() {
    std::lock_guard guard(mutex_);
    return superseded_;
  }
  unsigned index() const { return index_; }
 private:
  static void WINAPI Notification(WLAN_NOTIFICATION_DATA* data, void* context);
  WlanApi api_;
  HANDLE client_ = nullptr;
  GUID guid_{};
  unsigned index_ = 0;
  std::wstring profile_ = L"crQuickShare-" + base::UTF8ToWide(base::Uuid::GenerateRandomV4().AsLowercaseString());
  std::wstring previous_;
  std::mutex mutex_;
  bool watching_ = false, superseded_ = false;
  bool completed_ = false;
  DWORD reason_ = ERROR_SUCCESS;
  bool requested_ = false, cleaned_ = false, cleanup_ok_ = true;
};
bool Network::Open(const std::string& requested) {
  DWORD version = 0;
  if (!api_.ready() || !Ok(api_.open(2, nullptr, &version, &client_), "open")) return false;
  WLAN_INTERFACE_INFO_LIST* list = nullptr;
  if (!Ok(api_.enumerate(client_, nullptr, &list), "enumerate")) return false;
  NET_LUID desired{};
  const bool explicit_selection = !requested.empty();
  if (explicit_selection && !Ok(ConvertInterfaceAliasToLuid(base::UTF8ToWide(requested).c_str(), &desired), "interface_alias")) {
    api_.free(list); return false;
  }
  unsigned matches = 0;
  // WlanEnumInterfaces allocates dwNumberOfItems entries in this flexible array.
  auto interfaces = UNSAFE_BUFFERS(base::span(list->InterfaceInfo, static_cast<size_t>(list->dwNumberOfItems)));
  for (const auto& entry : interfaces) {
    NET_LUID luid{};
    NET_IFINDEX index = 0;
    if (ConvertInterfaceGuidToLuid(&entry.InterfaceGuid, &luid) != NO_ERROR ||
        ConvertInterfaceLuidToIndex(&luid, &index) != NO_ERROR) continue;
    if (explicit_selection && luid.Value != desired.Value) continue;
    ++matches; guid_ = entry.InterfaceGuid; index_ = index;
  }
  api_.free(list);
  if (matches != 1) {
    LOG(WARNING) << "crQuickShare: Windows hotspot interface selection failed; matches=" << matches;
    return false;
  }
  return Ok(api_.notify(client_, WLAN_NOTIFICATION_SOURCE_ACM, FALSE, &Notification, this, nullptr, nullptr), "register_notifications");
}
bool Network::Current(std::optional<WLAN_CONNECTION_ATTRIBUTES>& result) {
  result.reset();
  DWORD size = 0;
  void* raw = nullptr;
  const DWORD error = api_.query(client_, &guid_, wlan_intf_opcode_current_connection, nullptr, &size, &raw, nullptr);
  if (error == ERROR_INVALID_STATE) return true;  // Disconnected.
  if (!Ok(error, "current_connection")) return false;
  const bool valid = raw && size >= sizeof(WLAN_CONNECTION_ATTRIBUTES);
  if (valid) result = *static_cast<WLAN_CONNECTION_ATTRIBUTES*>(raw);
  api_.free(raw);
  return valid;
}
bool Network::SavePrevious() {
  // Do not interrupt a connection another client is currently establishing.
  DWORD size = 0;
  void* raw = nullptr;
  if (!Ok(api_.query(client_, &guid_, wlan_intf_opcode_interface_state, nullptr, &size, &raw, nullptr), "interface_state")) return false;
  const bool idle = raw && size >= sizeof(WLAN_INTERFACE_STATE) &&
      (*static_cast<WLAN_INTERFACE_STATE*>(raw) == wlan_interface_state_connected ||
       *static_cast<WLAN_INTERFACE_STATE*>(raw) == wlan_interface_state_disconnected);
  api_.free(raw);
  if (!idle) return false;
  std::optional<WLAN_CONNECTION_ATTRIBUTES> current;
  if (!Current(current)) return false;
  if (!current) return true;
  // A different application's temporary network has no saved profile to restore.
  if (current->wlanConnectionMode != wlan_connection_mode_profile && current->wlanConnectionMode != wlan_connection_mode_auto) return false;
  previous_ = Name(current->strProfileName);
  return !previous_.empty();
}
void WINAPI Network::Notification(WLAN_NOTIFICATION_DATA* data, void* context) {
  auto* self = static_cast<Network*>(context);
  if (data->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM || !IsEqualGUID(data->InterfaceGuid, self->guid_) ||
      !data->pData || data->dwDataSize < offsetof(WLAN_CONNECTION_NOTIFICATION_DATA, strProfileXml)) return;
  if (data->NotificationCode != wlan_notification_acm_connection_start &&
      data->NotificationCode != wlan_notification_acm_connection_complete) return;
  const auto* connection = static_cast<const WLAN_CONNECTION_NOTIFICATION_DATA*>(data->pData);
  std::lock_guard guard(self->mutex_);
  if (!self->watching_) return;
  const bool ours = connection->wlanConnectionMode == wlan_connection_mode_temporary_profile &&
      Name(connection->strProfileName) == self->profile_;
  if (!ours) {
    self->superseded_ = true;
    return;
  }
  if (data->NotificationCode == wlan_notification_acm_connection_complete) {
    self->completed_ = true;
    self->reason_ = connection->wlanReasonCode;
  }
}
bool Network::Start(const HotspotCredentials& credentials) {
  const auto xml = base::UTF8ToWide(
      "<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\"><name>" +
      base::WideToUTF8(profile_) + "</name><SSIDConfig><SSID><hex>" + base::HexEncode(credentials.ssid) +
      "</hex></SSID></SSIDConfig><connectionType>ESS</connectionType><connectionMode>manual</connectionMode>"
      "<MSM><security><authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption>"
      "<useOneX>false</useOneX></authEncryption><sharedKey><keyType>passPhrase</keyType><protected>false</protected>"
      "<keyMaterial>" + XmlText(credentials.password) + "</keyMaterial></sharedKey></security></MSM></WLANProfile>");
  WLAN_CONNECTION_PARAMETERS parameters{};
  parameters.wlanConnectionMode = wlan_connection_mode_temporary_profile;
  parameters.strProfile = xml.c_str();
  parameters.dot11BssType = dot11_BSS_type_infrastructure;
  {
    std::lock_guard guard(mutex_);
    watching_ = true;
  }
  requested_ = Ok(api_.connect(client_, &guid_, &parameters, nullptr), "connect");
  return requested_;
}
bool Network::WaitConnected(base::TimeTicks deadline, const KeepGoing& active) {
  while (active() && base::TimeTicks::Now() < deadline) {
    {
      std::lock_guard guard(mutex_);
      if (superseded_) return false;
      if (completed_ && reason_ != WLAN_REASON_CODE_SUCCESS) {
        LOG(WARNING) << "crQuickShare: Windows hotspot association failed; reason=" << reason_;
        return false;
      }
    }
    std::optional<WLAN_CONNECTION_ATTRIBUTES> current;
    if (!Current(current)) return false;
    if (current && current->isState == wlan_interface_state_connected && Owned(*current)) return true;
    Pause();
  }
  return false;
}
bool Network::Cleanup(base::TimeTicks deadline, const KeepGoing& keep_restoring) {
  if (cleaned_ || !requested_) return cleanup_ok_;
  cleaned_ = true;
  bool restored = false;
  std::optional<WLAN_CONNECTION_ATTRIBUTES> current;
  if (Current(current)) {
    if (Superseded()) {
      // Do not interrupt a newer request, but do not claim cleanup succeeded
      // while our temporary network is still the current connection either.
      while (current && Owned(*current) && keep_restoring() && base::TimeTicks::Now() < deadline) {
        Pause();
        if (!Current(current)) return cleanup_ok_ = false;
      }
      restored = !current || !Owned(*current);
    } else if (current && !Owned(*current) && Name(current->strProfileName) != previous_) {
      restored = true;  // Preserve the user's/AutoConfig's newer selection.
    } else {
      // Also cancel an in-flight WlanConnect; closing its client handle alone
      // does not mean the outstanding connection request was cancelled.
      const DWORD error = api_.disconnect(client_, &guid_, nullptr);
      if (error == ERROR_INVALID_STATE || Ok(error, "disconnect")) {
        while (base::TimeTicks::Now() < deadline) {
          if (!Current(current)) break;
          if (Superseded()) { restored = !current || !Owned(*current); break; }
          if (!current) {
            restored = previous_.empty();
            if (!restored && keep_restoring()) {
              {
                std::lock_guard guard(mutex_);
                watching_ = false;  // The following notification is our restore.
              }
              WLAN_CONNECTION_PARAMETERS parameters{};
              parameters.wlanConnectionMode = wlan_connection_mode_profile;
              parameters.strProfile = previous_.c_str();
              parameters.dot11BssType = dot11_BSS_type_infrastructure;
              if (!Ok(api_.connect(client_, &guid_, &parameters, nullptr), "restore")) break;
              while (keep_restoring() && base::TimeTicks::Now() < deadline) {
                if (!Current(current)) break;
                if (current && current->isState == wlan_interface_state_connected && !Owned(*current)) {
                  restored = true; break;
                }
                Pause();
              }
            }
            break;
          }
          if (!Owned(*current) && Name(current->strProfileName) != previous_) { restored = true; break; }
          if (!keep_restoring()) break;
          Pause();
        }
      }
    }
  }
  cleanup_ok_ = restored;
  LOG(INFO) << "crQuickShare: Hotspot cleanup; backend=NativeWifi; network_restored_or_preserved=" << restored
            << "; persistent_profile_created=0; success=" << cleanup_ok_;
  return cleanup_ok_;
}
class HotspotChannel final : public TransportChannel {
 public:
  explicit HotspotChannel(std::unique_ptr<TransportChannel> channel) : channel_(std::move(channel)) {}
  Result Read(base::span<char> bytes) override { return channel_->Read(bytes); }
  Result Write(base::span<const char> bytes) override { return channel_->Write(bytes); }
  Ready Wait(bool read, bool write, int ms) override { return channel_->Wait(read, write, ms); }
  void Close() override { channel_->Close(); }
  const char* name() const override { return "WIFI_HOTSPOT"; }
 private:
  std::unique_ptr<TransportChannel> channel_;
};
HotspotUpgradeResult Join(const std::string& requested_interface, const HotspotCredentials& credentials,
                         base::TimeTicks deadline, const KeepGoing& active) {
  const auto started = base::TimeTicks::Now();
  auto stage_started = started;
  const char* stage = "validate_credentials";
  auto network = std::make_unique<Network>();
  auto failure = [&](const char* error) -> HotspotUpgradeResult {
    LOG(WARNING) << "crQuickShare: Hotspot join timing; backend=NativeWifi; success=0; stage=" << stage
                 << "; reason=" << error << "; stage_elapsed_ms=" << (base::TimeTicks::Now() - stage_started).InMilliseconds()
                 << "; elapsed_ms=" << (base::TimeTicks::Now() - started).InMilliseconds();
    return {std::move(network), nullptr, error};
  };
  auto next = [&](const char* value) {
    const auto now = base::TimeTicks::Now();
    LOG(INFO) << "crQuickShare: Hotspot join stage; backend=NativeWifi; stage=" << stage
              << "; event=finished; stage_elapsed_ms=" << (now - stage_started).InMilliseconds();
    stage = value; stage_started = now;
    LOG(INFO) << "crQuickShare: Hotspot join stage; backend=NativeWifi; stage=" << stage << "; event=started";
  };
  in_addr gateway{};
  if (credentials.ssid.empty() || credentials.ssid.size() > 32 ||
      credentials.password.size() < 8 || credentials.password.size() > 63 ||
      !std::ranges::all_of(credentials.password, [](unsigned char c) { return c >= 32 && c < 127; }) ||
      credentials.port < 1 || credentials.port > 65535 ||
      inet_pton(AF_INET, credentials.gateway.c_str(), &gateway) != 1 ||
      (ntohl(gateway.s_addr) >> 24) == 0 || (ntohl(gateway.s_addr) >> 24) == 127 || (ntohl(gateway.s_addr) >> 28) >= 14)
    return failure("invalid_hotspot_credentials");
  auto running = [&] { return active() && base::TimeTicks::Now() < deadline; };
  next("check_wifi_device");
  if (!running() || !network->Open(requested_interface)) return failure("windows_wifi_unavailable");
  next("read_previous_connection");
  if (!running() || !network->SavePrevious()) return failure("previous_connection_unavailable");
  next("activate_connection");
  if (!running() || !network->Start(credentials) || !network->WaitConnected(deadline, active))
    return failure("hotspot_activation_failed");
  next("check_local_address");
  std::string source;
  while (running() && !network->Superseded()) {
    std::optional<WLAN_CONNECTION_ATTRIBUTES> current;
    if (!network->Current(current) || !current || !network->Owned(*current)) break;
    const auto links = platform::Interfaces();
    if (!links) return failure("interface_check_failed");
    for (const auto& link : *links) {
      if (link.index != network->index() || !link.mask.s_addr || !link.address.s_addr ||
          link.address.s_addr == gateway.s_addr || ((link.address.s_addr ^ gateway.s_addr) & link.mask.s_addr)) continue;
      char address[INET_ADDRSTRLEN];
      if (inet_ntop(AF_INET, &link.address, address, sizeof(address))) source = address;
      break;
    }
    if (!source.empty()) break;
    Pause();
  }
  if (source.empty()) return failure("hotspot_local_route_missing");
  next("connect_tcp");
  auto connected = ConnectTcpChannel(credentials.gateway, static_cast<uint16_t>(credentials.port), deadline,
      [&] { return running() && !network->Superseded(); }, source, network->index());
  if (!connected.channel) return failure(connected.outcome);
  LOG(INFO) << "crQuickShare: Hotspot join timing; backend=NativeWifi; success=1; interface_index=" << network->index()
            << "; elapsed_ms=" << (base::TimeTicks::Now() - started).InMilliseconds();
  return {std::move(network), std::make_unique<HotspotChannel>(std::move(connected.channel)), nullptr};
}
}  // namespace
HotspotConnector MakeWindowsHotspotConnector(const std::string& requested_interface) {
  return [requested_interface](const HotspotCredentials& credentials, base::TimeTicks deadline, const KeepGoing& active) {
    return Join(requested_interface, credentials, deadline, active);
  };
}
}  // namespace crquickshare
