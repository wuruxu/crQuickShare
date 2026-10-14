// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/hotspot_upgrade.h"
#include <arpa/inet.h>
#include <dbus/dbus.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <chrono>
#include <optional>
#include <thread>
#include <algorithm>
#include <utility>
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "dbus/message.h"
#include "dbus/object_path.h"
namespace crquickshare {
namespace {
constexpr char kNm[] = "org.freedesktop.NetworkManager";
constexpr char kNmPath[] = "/org/freedesktop/NetworkManager";
constexpr char kDevice[] = "org.freedesktop.NetworkManager.Device";
constexpr char kActive[] = "org.freedesktop.NetworkManager.Connection.Active";
constexpr char kSettings[] = "org.freedesktop.NetworkManager.Settings.Connection";
using Writer = dbus::MessageWriter;
using Reader = dbus::MessageReader;
using Path = dbus::ObjectPath;
using KeepGoing = std::function<bool()>;

void Property(Writer& dictionary, const char* name,
              const std::function<void(Writer&)>& value) {
  Writer entry(nullptr);
  dictionary.OpenDictEntry(&entry);
  entry.AppendString(name);
  value(entry);
  dictionary.CloseContainer(&entry);
}
void StringProperty(Writer& dictionary, const char* name, const std::string& value) {
  Property(dictionary, name, [&](Writer& entry) { entry.AppendVariantOfString(value); });
}
void BoolProperty(Writer& dictionary, const char* name, bool value) {
  Property(dictionary, name, [&](Writer& entry) { entry.AppendVariantOfBool(value); });
}
void Setting(Writer& settings, const char* name,
             const std::function<void(Writer&)>& properties) {
  Writer entry(nullptr), dictionary(nullptr);
  settings.OpenDictEntry(&entry);
  entry.AppendString(name);
  entry.OpenArray("{sv}", &dictionary);
  properties(dictionary);
  entry.CloseContainer(&dictionary);
  settings.CloseContainer(&entry);
}

const char* DeviceStage(uint32_t state) {
  switch (state) {
    case 40: return "prepare";
    case 50: return "wifi_config";  // Includes scanning, association and authentication.
    case 60: return "need_auth";
    case 70: return "ip_config";
    case 80: return "ip_check";
    case 90: return "secondaries";
    case 100: return "activated";
    case 110: return "deactivating";
    case 120: return "failed";
    default: return "other";
  }
}

// This backend runs on the transfer worker, which has no Chromium message loop.
// Pump a private libdbus connection in bounded slices, using Chromium's message
// codec. Do not block in CallMethodAndBlock while Bluetooth needs servicing.
class Network final : public UpgradeNetwork {
 public:
  ~Network() override;
  bool Open();
  bool Cleanup(base::TimeTicks deadline, const KeepGoing& keep_restoring) override;
  std::unique_ptr<dbus::Response> Call(const Path& path, const char* api,
      const char* method, const std::function<void(Writer&)>& arguments,
      base::TimeTicks deadline, const KeepGoing& active, bool activation = false);
  bool Get(const Path& path, const char* api, const char* property,
      const std::function<bool(Reader&)>& read, base::TimeTicks deadline,
      const KeepGoing& active);
  bool Current(Path* value, base::TimeTicks deadline, const KeepGoing& active);
  bool WaitActivated(const Path& connection, base::TimeTicks deadline, const KeepGoing& active);
  std::optional<bool> ProfilePresent(base::TimeTicks deadline);
  bool ReadActivation(dbus::Response* response);
  Path device, previous{"/"}, profile{"/"}, connection{"/"};
  bool activation_started = false;
 private:
  std::unique_ptr<dbus::Response> Receive(DBusPendingCall* pending,
      base::TimeTicks deadline, const KeepGoing& active);
  void Close();
  DBusConnection* bus_ = nullptr;
  // Cancellation of a pending call does not cancel NM's operation. Keep its
  // reply for cleanup; the private bus is also an activation lifetime lease.
  DBusPendingCall* activation_pending_ = nullptr;
  bool cleanup_attempted_ = false, cleanup_ok_ = true;
};
bool Network::Open() {
  if (!dbus_threads_init_default()) return false;
  bus_ = dbus_bus_get_private(DBUS_BUS_SYSTEM, nullptr);
  if (!bus_) return false;
  dbus_connection_set_exit_on_disconnect(bus_, false);
  return true;
}
void Network::Close() {
  if (activation_pending_) {
    dbus_pending_call_cancel(activation_pending_);
    dbus_pending_call_unref(activation_pending_);
    activation_pending_ = nullptr;
  }
  if (bus_) {
    dbus_connection_close(bus_);
    dbus_connection_unref(bus_);
    bus_ = nullptr;
  }
}
Network::~Network() {
  if (activation_started && !cleanup_attempted_)
    Cleanup(base::TimeTicks::Now() + base::Seconds(1), [] { return false; });
  Close();
}
std::unique_ptr<dbus::Response> Network::Receive(DBusPendingCall* pending,
    base::TimeTicks deadline, const KeepGoing& active) {
  while (!dbus_pending_call_get_completed(pending)) {
    if (!active() || base::TimeTicks::Now() >= deadline ||
        !dbus_connection_get_is_connected(bus_)) return nullptr;
    const int ms = static_cast<int>(std::clamp<int64_t>(
        (deadline - base::TimeTicks::Now()).InMilliseconds(), 0, 50));
    dbus_connection_read_write_dispatch(bus_, ms);
  }
  DBusMessage* reply = dbus_pending_call_steal_reply(pending);
  if (!reply) return nullptr;
  if (dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_METHOD_RETURN) {
    // Do not log the error body: it may echo connection settings or secrets.
    LOG(WARNING) << "crQuickShare: NetworkManager D-Bus error; name="
                 << dbus_message_get_error_name(reply);
    dbus_message_unref(reply);
    return nullptr;
  }
  return dbus::Response::FromRawMessage(reply);
}
std::unique_ptr<dbus::Response> Network::Call(const Path& path, const char* api,
    const char* method, const std::function<void(Writer&)>& arguments,
    base::TimeTicks deadline, const KeepGoing& active, bool activation) {
  if (!bus_ || !active() || base::TimeTicks::Now() >= deadline) return nullptr;
  dbus::MethodCall call(api, method);
  if (!call.SetDestination(kNm) || !call.SetPath(path)) return nullptr;
  Writer writer(&call);
  arguments(writer);
  DBusPendingCall* pending = nullptr;
  const int timeout = static_cast<int>(std::clamp<int64_t>(
      (deadline - base::TimeTicks::Now()).InMilliseconds(), 1, 40000));
  if (!dbus_connection_send_with_reply(bus_, call.raw_message(), &pending, timeout) || !pending)
    return nullptr;
  if (activation) activation_started = true;
  auto response = Receive(pending, deadline, active);
  if (activation && !dbus_pending_call_get_completed(pending)) {
    activation_pending_ = pending;
  } else {
    dbus_pending_call_cancel(pending);
    dbus_pending_call_unref(pending);
  }
  return response;
}
bool Network::Get(const Path& path, const char* api, const char* property,
    const std::function<bool(Reader&)>& read, base::TimeTicks deadline, const KeepGoing& active) {
  auto response = Call(path, DBUS_INTERFACE_PROPERTIES, "Get", [&](Writer& writer) {
    writer.AppendString(api); writer.AppendString(property);
  }, deadline, active);
  if (!response) return false;
  Reader reader(response.get()), variant(nullptr);
  return reader.PopVariant(&variant) && read(variant);
}
bool Network::Current(Path* value, base::TimeTicks deadline, const KeepGoing& active) {
  return Get(device, kDevice, "ActiveConnection", [&](Reader& reader) {
    return reader.PopObjectPath(value);
  }, deadline, active);
}
bool Network::WaitActivated(const Path& target, base::TimeTicks deadline, const KeepGoing& active) {
  const auto started = base::TimeTicks::Now();
  auto next_diagnostic = started;
  uint32_t last_device = 0, last_reason = 0, last_active = 0;
  int64_t last_scan = -2;
  while (active() && base::TimeTicks::Now() < deadline) {
    uint32_t state = 0;
    if (!Get(target, kActive, "State", [&](Reader& reader) {
      return reader.PopUint32(&state);
    }, deadline, active)) return false;
    const auto now = base::TimeTicks::Now();
    if (now >= next_diagnostic || state != last_active) {
      // Diagnostics are best effort and bounded; they must not consume the join
      // deadline or prevent servicing the Bluetooth bootstrap channel.
      const auto diagnostic_deadline = std::min(deadline, now + base::Milliseconds(100));
      uint32_t device_state = 0, reason = 0;
      if (Get(device, kDevice, "StateReason", [&](Reader& reader) {
        Reader tuple(nullptr);
        return reader.PopStruct(&tuple) && tuple.PopUint32(&device_state) && tuple.PopUint32(&reason);
      }, diagnostic_deadline, active) &&
          (device_state != last_device || reason != last_reason || state != last_active)) {
        LOG(INFO) << "crQuickShare: Hotspot NM state; stage=" << DeviceStage(device_state)
                  << "; device_state=" << device_state << "; reason=" << reason
                  << "; active_state=" << state
                  << "; elapsed_ms=" << (base::TimeTicks::Now() - started).InMilliseconds();
        last_device = device_state;
        last_reason = reason;
      }
      int64_t scan = -1;
      if (Get(device, "org.freedesktop.NetworkManager.Device.Wireless", "LastScan",
          [&](Reader& reader) { return reader.PopInt64(&scan); }, diagnostic_deadline, active) &&
          scan != last_scan) {
        LOG(INFO) << "crQuickShare: Hotspot NM scan; last_scan_boot_ms=" << scan
                  << "; initial_sample=" << (last_scan == -2)
                  << "; elapsed_ms=" << (base::TimeTicks::Now() - started).InMilliseconds();
        last_scan = scan;
      }
      last_active = state;
      next_diagnostic = base::TimeTicks::Now() + base::Milliseconds(500);
    }
    if (state == 2) return true;  // NM_ACTIVE_CONNECTION_STATE_ACTIVATED: IP ready.
    if (state != 1) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}
bool Network::ReadActivation(dbus::Response* response) {
  if (!response) return false;
  Reader reader(response);
  return reader.PopObjectPath(&profile) && reader.PopObjectPath(&connection);
}
std::optional<bool> Network::ProfilePresent(base::TimeTicks deadline) {
  auto response = Call(Path("/org/freedesktop/NetworkManager/Settings"),
      "org.freedesktop.NetworkManager.Settings", "ListConnections", [](Writer&) {},
      deadline, [] { return true; });
  Reader array(nullptr);
  if (!response || !Reader(response.get()).PopArray(&array)) return std::nullopt;
  bool found = false;
  while (array.HasMoreData()) {
    Path path;
    if (!array.PopObjectPath(&path)) return std::nullopt;
    if (path == profile) found = true;
  }
  return found;
}
bool Network::Cleanup(base::TimeTicks deadline, const KeepGoing& keep_restoring) {
  if (cleanup_attempted_ || !activation_started) return cleanup_ok_;
  cleanup_attempted_ = true;
  const auto always = [] { return true; };
  if (activation_pending_) {
    auto response = Receive(activation_pending_,
        std::min(deadline, base::TimeTicks::Now() + base::Seconds(1)), always);
    ReadActivation(response.get());
    dbus_pending_call_cancel(activation_pending_);
    dbus_pending_call_unref(activation_pending_);
    activation_pending_ = nullptr;
  }
  bool restored = false, removed = false;
  const auto restore_deadline = deadline - base::Seconds(2);
  Path current;
  if (connection.value() != "/" && Current(&current, restore_deadline, keep_restoring)) {
    if (current != connection && current.value() != "/") {
      restored = true;  // NM or the user has selected another connection.
    } else if (previous.value() != "/") {
      auto response = Call(Path(kNmPath), kNm, "ActivateConnection", [&](Writer& writer) {
        writer.AppendObjectPath(previous);
        writer.AppendObjectPath(device);
        writer.AppendObjectPath(Path("/"));
      }, restore_deadline, keep_restoring);
      Path restored_connection;
      restored = response && Reader(response.get()).PopObjectPath(&restored_connection) &&
          WaitActivated(restored_connection, restore_deadline, keep_restoring);
    } else if (current.value() == "/") {
      restored = true;
    } else {
      restored = !!Call(Path(kNmPath), kNm, "DeactivateConnection", [&](Writer& writer) {
        writer.AppendObjectPath(connection);
      }, restore_deadline, keep_restoring);
      // Method completion only accepts deactivation; wait for device release.
      if (restored) {
        restored = false;
        while (keep_restoring() && base::TimeTicks::Now() < restore_deadline) {
          if (!Current(&current, restore_deadline, keep_restoring)) break;
          if (current != connection) { restored = true; break; }
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
      }
    }
  }
  // Volatile profiles may already have disappeared. Query the settings list
  // before deleting, rather than interpreting all D-Bus errors as "not found".
  if (profile.value() != "/") {
    auto present = ProfilePresent(deadline);
    if (present.has_value()) {
      removed = !*present;
      if (*present) {
        removed = !!Call(profile, kSettings, "Delete", [](Writer&) {}, deadline, always);
        // NM may delete a volatile profile between ListConnections and Delete.
        if (!removed) {
          present = ProfilePresent(deadline);
          removed = present.has_value() && !*present;
        }
      }
    }
  }
  // Closing our private connection also releases an activation whose reply was
  // lost or timed out. Never restore blindly when ownership cannot be verified.
  Close();
  cleanup_ok_ = restored && removed;
  LOG(INFO) << "crQuickShare: Hotspot cleanup; network_restored_or_preserved=" << restored
            << "; profile_removed=" << removed << "; success=" << cleanup_ok_;
  return cleanup_ok_;
}
class HotspotChannel final : public TransportChannel {
 public:
  explicit HotspotChannel(std::unique_ptr<TransportChannel> channel) : channel_(std::move(channel)) {}
  ~HotspotChannel() override;
  Result Read(base::span<char> bytes) override { return channel_->Read(bytes); }
  Result Write(base::span<const char> bytes) override { return channel_->Write(bytes); }
  Ready Wait(bool read, bool write, int ms) override { return channel_->Wait(read, write, ms); }
  void Close() override { channel_->Close(); }
  const char* name() const override { return "WIFI_HOTSPOT"; }
 private:
  std::unique_ptr<TransportChannel> channel_;
};
HotspotChannel::~HotspotChannel() = default;
// Measure upgrade stages within the session deadline. Never log credentials.
class JoinTiming {
 public:
  explicit JoinTiming(base::TimeTicks deadline);
  ~JoinTiming();
  void Stage(const char* stage);
  void Complete();
 private:
  void FinishStage(bool success);
  const base::TimeTicks started_ = base::TimeTicks::Now();
  const base::TimeTicks deadline_;
  base::TimeTicks stage_started_;
  const char* stage_ = nullptr;
  bool complete_ = false;
};
JoinTiming::JoinTiming(base::TimeTicks deadline) : deadline_(deadline) {}
JoinTiming::~JoinTiming() {
  if (stage_) FinishStage(false);
  LOG(INFO) << "crQuickShare: Hotspot join timing; success=" << complete_
            << "; elapsed_ms=" << (base::TimeTicks::Now() - started_).InMilliseconds();
}
void JoinTiming::FinishStage(bool success) {
  const auto now = base::TimeTicks::Now();
  LOG(INFO) << "crQuickShare: Hotspot join stage; stage=" << stage_
            << "; event=finished; success=" << success
            << "; stage_elapsed_ms=" << (now - stage_started_).InMilliseconds()
            << "; elapsed_ms=" << (now - started_).InMilliseconds()
            << "; deadline_expired=" << (now >= deadline_);
  stage_ = nullptr;
}
void JoinTiming::Stage(const char* stage) {
  if (stage_) FinishStage(true);
  stage_ = stage;
  stage_started_ = base::TimeTicks::Now();
  LOG(INFO) << "crQuickShare: Hotspot join stage; stage=" << stage_
            << "; event=started; elapsed_ms=" << (stage_started_ - started_).InMilliseconds()
            << "; remaining_ms=" << (deadline_ - stage_started_).InMilliseconds();
}
void JoinTiming::Complete() {
  FinishStage(true);
  complete_ = true;
}
HotspotUpgradeResult Join(const std::string& interface, const HotspotCredentials& credentials,
                          base::TimeTicks deadline, const std::function<bool()>& active) {
  JoinTiming timing(deadline);
  timing.Stage("validate_credentials");
  in_addr gateway{};
  const auto clean = [](const std::string& value) {
    return std::ranges::all_of(value, [](unsigned char c) { return c >= 32 && c != 127; });
  };
  if (credentials.ssid.empty() || credentials.ssid.size() > 32 || !clean(credentials.ssid) ||
      credentials.password.size() < 8 || credentials.password.size() > 63 || !clean(credentials.password) ||
      credentials.port < 1 || credentials.port > 65535 ||
      inet_pton(AF_INET, credentials.gateway.c_str(), &gateway) != 1 ||
      !gateway.s_addr || (ntohl(gateway.s_addr) >> 24) == 127 || (ntohl(gateway.s_addr) >> 28) >= 14)
    return {nullptr, nullptr, "invalid_hotspot_credentials"};
  LOG(INFO) << "crQuickShare: Hotspot frequency hint; protocol_value=" << credentials.frequency
            << "; applied=0; band=auto; channel=0";
  timing.Stage("check_wifi_device");
  auto network = std::make_unique<Network>();
  if (!active() || base::TimeTicks::Now() >= deadline || !network->Open())
    return {nullptr, nullptr, "network_manager_unavailable"};
  auto device_response = network->Call(Path(kNmPath), kNm, "GetDeviceByIpIface",
      [&](Writer& writer) { writer.AppendString(interface); }, deadline, active);
  uint32_t type = 0;
  if (!device_response || !Reader(device_response.get()).PopObjectPath(&network->device) ||
      !network->Get(network->device, kDevice, "DeviceType", [&](Reader& reader) {
        return reader.PopUint32(&type);
      }, deadline, active) || type != 2)
    return {nullptr, nullptr, "network_manager_wifi_unavailable"};
  timing.Stage("read_previous_connection");
  Path previous_active;
  if (!network->Current(&previous_active, deadline, active) ||
      (previous_active.value() != "/" && !network->Get(previous_active, kActive, "Connection",
          [&](Reader& reader) { return reader.PopObjectPath(&network->previous); }, deadline, active)))
    return {nullptr, nullptr, "previous_connection_unavailable"};
  timing.Stage("create_profile");
  auto response = network->Call(Path(kNmPath), kNm, "AddAndActivateConnection2", [&](Writer& writer) {
    Writer settings(nullptr);
    writer.OpenArray("{sa{sv}}", &settings);
    Setting(settings, "connection", [&](Writer& properties) {
      StringProperty(properties, "id", "crQuickShare upgrade");
      StringProperty(properties, "uuid", base::Uuid::GenerateRandomV4().AsLowercaseString());
      StringProperty(properties, "type", "802-11-wireless");
      StringProperty(properties, "interface-name", interface);
      BoolProperty(properties, "autoconnect", false);
    });
    Setting(settings, "802-11-wireless", [&](Writer& properties) {
      StringProperty(properties, "mode", "infrastructure");
      // Let NetworkManager select the band and channel. The peer frequency
      // is diagnostic only; pinning it can prevent discovery of the hotspot.
      Property(properties, "ssid", [&](Writer& entry) {
        Writer variant(nullptr);
        entry.OpenVariant("ay", &variant);
        variant.AppendArrayOfBytes(base::as_byte_span(credentials.ssid));
        entry.CloseContainer(&variant);
      });
    });
    Setting(settings, "802-11-wireless-security", [&](Writer& properties) {
      StringProperty(properties, "key-mgmt", "wpa-psk");
      StringProperty(properties, "psk", credentials.password);
      // NM owns this supplied secret for the volatile profile's lifetime.
      // NOT_SAVED would request a Secret Agent again; unlike nmcli's passwd-file
      // path, this backend does not register an agent to answer that request.
      // persist=volatile below prevents both profile and secret reaching disk.
      Property(properties, "psk-flags", [](Writer& entry) { entry.AppendVariantOfUint32(0); });
    });
    Setting(settings, "ipv4", [](Writer& properties) {
      StringProperty(properties, "method", "auto");
      BoolProperty(properties, "never-default", true);
      BoolProperty(properties, "ignore-auto-dns", true);
    });
    Setting(settings, "ipv6", [](Writer& properties) { StringProperty(properties, "method", "disabled"); });
    writer.CloseContainer(&settings);
    writer.AppendObjectPath(network->device);
    writer.AppendObjectPath(Path("/"));
    Writer options(nullptr);
    writer.OpenArray("{sv}", &options);
    StringProperty(options, "persist", "volatile");
    StringProperty(options, "bind-activation", "dbus-client");
    writer.CloseContainer(&options);
  }, deadline, active, true);
  if (!network->ReadActivation(response.get()))
    return {std::move(network), nullptr, "temporary_profile_activation_failed"};
  timing.Stage("activate_connection");
  LOG(INFO) << "crQuickShare: Hotspot joining; interface=" << interface;
  if (!network->WaitActivated(network->connection, deadline, active))
    return {std::move(network), nullptr, "hotspot_activation_failed"};
  timing.Stage("check_local_address");
  ifaddrs* raw = nullptr;
  std::string source;
  if (getifaddrs(&raw) == 0) {
    for (auto* link = raw; link; link = link->ifa_next) {
      if (interface != link->ifa_name || !link->ifa_addr || !link->ifa_netmask || link->ifa_addr->sa_family != AF_INET) continue;
      const auto local = reinterpret_cast<const sockaddr_in*>(link->ifa_addr)->sin_addr;
      const auto mask = reinterpret_cast<const sockaddr_in*>(link->ifa_netmask)->sin_addr;
      if ((local.s_addr & mask.s_addr) != (gateway.s_addr & mask.s_addr) || local.s_addr == gateway.s_addr) continue;
      char text[INET_ADDRSTRLEN];
      if (inet_ntop(AF_INET, &local, text, sizeof(text))) { source = text; break; }
    }
    freeifaddrs(raw);
  }
  if (source.empty()) return {std::move(network), nullptr, "hotspot_local_route_missing"};
  timing.Stage("connect_tcp");
  auto connected = ConnectTcpChannel(credentials.gateway, static_cast<uint16_t>(credentials.port), deadline,
                                     active, source, if_nametoindex(interface.c_str()));
  if (!connected.channel) return {std::move(network), nullptr, connected.outcome};
  timing.Complete();
  return {std::move(network), std::make_unique<HotspotChannel>(std::move(connected.channel)), nullptr};
}
}  // namespace
HotspotConnector MakeLinuxHotspotConnector(const std::string& requested_interface) {
  std::string interface = requested_interface;
  if (interface.empty()) {
    // Include a disconnected Wi-Fi interface: BLE bootstrap does not require
    // an existing LAN address. Multiple radios require an explicit selection.
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) return {};
    bool ambiguous = false;
    for (auto* entry = interfaces; entry; entry = entry->ifa_next) {
      // One interface may have multiple address records.
      if (!entry->ifa_name || interface == entry->ifa_name) continue;
      if (!base::PathExists(base::FilePath("/sys/class/net").Append(entry->ifa_name).Append("wireless"))) continue;
      if (!interface.empty()) { ambiguous = true; break; }
      interface = entry->ifa_name;
    }
    freeifaddrs(interfaces);
    if (ambiguous) return {};
  }
  if (interface.empty() || interface.size() >= IFNAMSIZ ||
      !std::ranges::all_of(interface, [](unsigned char c) { return base::IsAsciiAlpha(c) || base::IsAsciiDigit(c) || c == '_' || c == '-' || c == '.'; }) ||
      !if_nametoindex(interface.c_str())) return {};
  return [interface](const HotspotCredentials& credentials, base::TimeTicks deadline, const std::function<bool()>& active) {
    return Join(interface, credentials, deadline, active);
  };
}
}  // namespace crquickshare
