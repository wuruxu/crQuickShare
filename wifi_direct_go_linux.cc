// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/wifi_direct_go.h"
#include <arpa/inet.h>
#include <dbus/dbus.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <pwd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>
#include "base/files/scoped_file.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/uuid.h"
#include "dbus/message.h"
#include "dbus/object_path.h"
namespace crquickshare {
std::unique_ptr<TransportChannel> AdoptTcpChannel(base::ScopedFD socket);
namespace {
using Active = std::function<bool()>;
using Writer = dbus::MessageWriter;
using Reader = dbus::MessageReader;
using Path = dbus::ObjectPath;
constexpr char kNm[] = "org.freedesktop.NetworkManager";
constexpr char kDevice[] = "org.freedesktop.NetworkManager.Device";
constexpr char kP2p[] = "org.freedesktop.NetworkManager.Device.WifiP2P";
constexpr char kConnection[] = "org.freedesktop.NetworkManager.Connection.Active";
constexpr char kRoot[] = "/org/freedesktop/NetworkManager";
void Property(Writer& dictionary, const char* name, const std::function<void(Writer&)>& value) {
  Writer entry(nullptr);
  dictionary.OpenDictEntry(&entry); entry.AppendString(name); value(entry);
  dictionary.CloseContainer(&entry);
}
void StringProperty(Writer& dictionary, const char* name, const std::string& value) {
  Property(dictionary, name, [&](Writer& entry) { entry.AppendVariantOfString(value); });
}
void BoolProperty(Writer& dictionary, const char* name, bool value) {
  Property(dictionary, name, [&](Writer& entry) { entry.AppendVariantOfBool(value); });
}
void Setting(Writer& settings, const char* name, const std::function<void(Writer&)>& fill) {
  Writer entry(nullptr), dictionary(nullptr);
  settings.OpenDictEntry(&entry); entry.AppendString(name); entry.OpenArray("{sv}", &dictionary);
  fill(dictionary); entry.CloseContainer(&dictionary); settings.CloseContainer(&entry);
}
class DirectChannel final : public TransportChannel {
 public:
  explicit DirectChannel(std::unique_ptr<TransportChannel> stream) : stream_(std::move(stream)) {}
  Result Read(base::span<char> bytes) override { return stream_->Read(bytes); }
  Result Write(base::span<const char> bytes) override { return stream_->Write(bytes); }
  Ready Wait(bool read, bool write, int timeout) override { return stream_->Wait(read, write, timeout); }
  void Close() override { stream_->Close(); }
  const char* name() const override { return "WIFI_DIRECT"; }
 private:
  std::unique_ptr<TransportChannel> stream_;
};
// A private bus connection is the NM activation lifetime lease. All calls run
// on the transfer worker and service the old Bluetooth channel while waiting.
class Group final : public WifiDirectGroup {
 public:
  ~Group() override {
    if (activation_started_ && !cleanup_attempted_) Cleanup(base::TimeTicks::Now() + base::Seconds(1), [] { return true; });
    CloseBus();
  }
  bool Open(base::TimeTicks deadline, const Active& active);
  std::vector<WifiDirectInterface> Interfaces(base::TimeTicks deadline, const Active& active);
  const char* stage() const { return stage_; }
  bool Start(const std::string& interface, base::TimeTicks deadline, const Active& active,
             const HotspotCredentials* peer = nullptr);
  ChannelConnectResult Connect(const HotspotCredentials& peer, base::TimeTicks deadline, const Active& active);
  std::unique_ptr<TransportChannel> Accept(base::TimeTicks deadline, const Active& active) override;
  bool Cleanup(base::TimeTicks deadline, const Active& active) override;
 private:
  std::unique_ptr<dbus::Response> Call(const Path& path, const char* api, const char* method,
      const std::function<void(Writer&)>& arguments, base::TimeTicks deadline, const Active& active,
      const char* destination = nullptr);
  bool Get(const Path& path, const char* api, const char* property,
      const std::function<bool(Reader&)>& read, base::TimeTicks deadline, const Active& active);
  bool Current(const Path& device, Path* value, base::TimeTicks deadline, const Active& active) {
    return Get(device, kDevice, "ActiveConnection", [&](Reader& reader) {
      return reader.PopObjectPath(value);
    }, deadline, active);
  }
  bool Healthy(base::TimeTicks deadline, const Active& active) {
    Path station_current, group_current;
    return Current(station_, &station_current, deadline, active) && station_current == previous_ &&
           Current(device_, &group_current, deadline, active) && group_current == connection_;
  }
  bool Listen(base::TimeTicks deadline, const Active& active);
  void CloseBus() {
    if (bus_) { dbus_connection_close(bus_); dbus_connection_unref(bus_); bus_ = nullptr; }
  }
  DBusConnection* bus_ = nullptr;
  std::string owner_, group_interface_;
  Path station_, device_, previous_{"/"}, profile_{"/"}, connection_{"/"};
  base::ScopedFD listener_, listener6_;
  uint32_t local_ = 0, mask_ = 0;
  unsigned group_index_ = 0;
  const char* stage_ = "open_bus";
  bool activation_started_ = false, cleanup_attempted_ = false, cleaned_ = false;
  bool client_ = false;
};
std::unique_ptr<dbus::Response> Group::Call(const Path& path, const char* api, const char* method,
    const std::function<void(Writer&)>& arguments, base::TimeTicks deadline, const Active& active,
    const char* destination) {
  if (!bus_ || !active() || base::TimeTicks::Now() >= deadline) return nullptr;
  dbus::MethodCall call(api, method);
  if (!call.SetDestination(destination ? destination : owner_) || !call.SetPath(path)) return nullptr;
  Writer writer(&call); arguments(writer);
  DBusPendingCall* pending = nullptr;
  const int timeout = static_cast<int>(std::clamp<int64_t>(
      (deadline - base::TimeTicks::Now()).InMilliseconds(), 1, 40000));
  if (!dbus_connection_send_with_reply(bus_, call.raw_message(), &pending, timeout) || !pending)
    return nullptr;
  while (!dbus_pending_call_get_completed(pending)) {
    if (!active() || base::TimeTicks::Now() >= deadline || !dbus_connection_get_is_connected(bus_)) break;
    dbus_connection_read_write_dispatch(bus_, static_cast<int>(std::clamp<int64_t>(
        (deadline - base::TimeTicks::Now()).InMilliseconds(), 0, 50)));
  }
  DBusMessage* reply = dbus_pending_call_get_completed(pending) ? dbus_pending_call_steal_reply(pending) : nullptr;
  dbus_pending_call_cancel(pending); dbus_pending_call_unref(pending);
  if (!reply) return nullptr;
  if (dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_METHOD_RETURN) {
    // Error bodies may contain credentials. Never log the body or settings.
    LOG(WARNING) << "crQuickShare: Direct NM error; method=" << method
                 << "; name=" << dbus_message_get_error_name(reply);
    dbus_message_unref(reply); return nullptr;
  }
  return dbus::Response::FromRawMessage(reply);
}
bool Group::Get(const Path& path, const char* api, const char* property,
    const std::function<bool(Reader&)>& read, base::TimeTicks deadline, const Active& active) {
  auto response = Call(path, DBUS_INTERFACE_PROPERTIES, "Get", [&](Writer& writer) {
    writer.AppendString(api); writer.AppendString(property);
  }, deadline, active);
  if (!response) return false;
  Reader reader(response.get()), variant(nullptr);
  return reader.PopVariant(&variant) && read(variant);
}
bool Group::Open(base::TimeTicks deadline, const Active& active) {
  if (!dbus_threads_init_default()) return false;
  bus_ = dbus_bus_get_private(DBUS_BUS_SYSTEM, nullptr);
  if (!bus_) return false;
  dbus_connection_set_exit_on_disconnect(bus_, false);
  auto owner_reply = Call(Path(DBUS_PATH_DBUS), DBUS_INTERFACE_DBUS, "GetNameOwner", [&](Writer& writer) {
    writer.AppendString(kNm);
  }, deadline, active, DBUS_SERVICE_DBUS);
  if (!owner_reply || !Reader(owner_reply.get()).PopString(&owner_)) return false;
  return true;
}
std::vector<WifiDirectInterface> Group::Interfaces(base::TimeTicks deadline, const Active& active) {
  std::vector<WifiDirectInterface> result;
  auto reply = Call(Path(kRoot), kNm, "GetDevices", [](Writer&) {}, deadline, active);
  std::vector<Path> devices;
  if (!reply || !Reader(reply.get()).PopArrayOfObjectPaths(&devices)) return result;
  std::vector<std::string> free_p2p;
  for (const auto& device : devices) {
    uint32_t type = 0, state = 0;
    std::string name;
    if (!Get(device, kDevice, "DeviceType", [&](Reader& r) { return r.PopUint32(&type); }, deadline, active) ||
        (type != 2 && type != 30) ||
        !Get(device, kDevice, "Interface", [&](Reader& r) { return r.PopString(&name); }, deadline, active)) continue;
    if (type == 30) {
      Path current;
      if (Current(device, &current, deadline, active) && current == Path("/")) free_p2p.push_back(name);
      continue;
    }
    uint32_t mode = 0;
    if (!Get(device, "org.freedesktop.NetworkManager.Device.Wireless", "Mode",
             [&](Reader& r) { return r.PopUint32(&mode); }, deadline, active) ||
        !Get(device, kDevice, "State", [&](Reader& r) { return r.PopUint32(&state); }, deadline, active)) continue;
    // Preserve existing AP/mesh/IBSS connections. Do not assume that a radio
    // supporting STA + P2P can also run AP + P2P concurrently.
    result.push_back({name, mode, (state == 30 || state == 100) && (mode == 0 || mode == 2), mode == 2 && state == 100});
  }
  for (auto& item : result)
    item.available &= std::ranges::find(free_p2p, "p2p-dev-" + item.name) != free_p2p.end();
  std::ranges::sort(result, {}, &WifiDirectInterface::name);
  return result;
}
bool Group::Start(const std::string& interface, base::TimeTicks deadline, const Active& active,
                  const HotspotCredentials* peer) {
  client_ = peer != nullptr;
  if (!Open(deadline, active)) return false;
  const auto interfaces = Interfaces(deadline, active);
  std::string selected = interface;
  if (selected.empty()) {
    for (const auto& item : interfaces) if (item.available) { selected = item.name; break; }
  }
  const auto chosen = std::ranges::find_if(interfaces, [&](const auto& item) { return item.name == selected; });
  if (chosen == interfaces.end() || !chosen->available) {
    stage_ = chosen != interfaces.end() && chosen->mode == 3 ? "ap_mode_not_available" : "interface_unavailable";
    return false;
  }
  // Pin the daemon's unique name: after a restart never operate on reused paths.
  stage_ = "find_p2p_device";
  auto station_reply = Call(Path(kRoot), kNm, "GetDeviceByIpIface", [&](Writer& writer) {
    writer.AppendString(selected);
  }, deadline, active);
  uint32_t type = 0;
  if (!station_reply || !Reader(station_reply.get()).PopObjectPath(&station_) ||
      !Get(station_, kDevice, "DeviceType", [&](Reader& reader) { return reader.PopUint32(&type); }, deadline, active) ||
      type != 2 || !Current(station_, &previous_, deadline, active)) return false;
  auto devices_reply = Call(Path(kRoot), kNm, "GetDevices", [](Writer&) {}, deadline, active);
  std::vector<Path> devices;
  if (!devices_reply || !Reader(devices_reply.get()).PopArrayOfObjectPaths(&devices)) return false;
  bool found = false;
  for (const auto& candidate : devices) {
    std::string name;
    if (!Get(candidate, kDevice, "DeviceType", [&](Reader& reader) { return reader.PopUint32(&type); }, deadline, active)) return false;
    if (type != 30) continue;
    if (!Get(candidate, kDevice, "Interface", [&](Reader& reader) { return reader.PopString(&name); }, deadline, active)) return false;
    // NM 1.58 creates this name from its parent Wi-Fi device (nm-device-wifi.c).
    if (name == "p2p-dev-" + selected) { if (found) return false; device_ = candidate; found = true; }
  }
  Path current;
  if (!found || !Current(device_, &current, deadline, active) || current != Path("/")) return false;
  stage_ = "check_nm_extension";
  auto introspection = Call(device_, DBUS_INTERFACE_INTROSPECTABLE, "Introspect", [](Writer&) {}, deadline, active);
  std::string xml;
  if (!introspection || !Reader(introspection.get()).PopString(&xml) ||
      xml.find("GetGroupCredentials") == std::string::npos) {
    LOG(WARNING) << "crQuickShare: Direct requires downstream NetworkManager P2P extension";
    return false;
  }
  std::vector<char> account_buffer(16384);
  passwd account{}, *account_result = nullptr;
  if (getpwuid_r(geteuid(), &account, account_buffer.data(), account_buffer.size(), &account_result) ||
      !account_result || !account.pw_name || !*account.pw_name) return false;
  const std::string permission = "user:" + std::string(account.pw_name) + ":";
  stage_ = client_ ? "activate_client" : "activate_go";
  activation_started_ = true;
  auto activation = Call(Path(kRoot), kNm, "AddAndActivateConnection2", [&](Writer& writer) {
    Writer settings(nullptr), options(nullptr);
    writer.OpenArray("{sa{sv}}", &settings);
    Setting(settings, "connection", [&](Writer& dictionary) {
      StringProperty(dictionary, "id", "crQuickShare Wi-Fi Direct");
      StringProperty(dictionary, "uuid", base::Uuid::GenerateRandomV4().AsLowercaseString());
      StringProperty(dictionary, "type", "wifi-p2p");
      BoolProperty(dictionary, "autoconnect", false);
      Property(dictionary, "permissions", [&](Writer& entry) {
        Writer variant(nullptr); entry.OpenVariant("as", &variant);
        variant.AppendArrayOfStrings({permission}); entry.CloseContainer(&variant);
      });
    });
    Setting(settings, "wifi-p2p", [&](Writer& dictionary) {
      BoolProperty(dictionary, client_ ? "client" : "autonomous", true);
      Property(dictionary, "frequency", [&](Writer& entry) {
        entry.AppendVariantOfUint32(peer && peer->frequency > 0 ? peer->frequency : 0);
      });
      if (peer) {
        Property(dictionary, "ssid", [&](Writer& entry) {
          Writer variant(nullptr); entry.OpenVariant("ay", &variant);
          variant.AppendArrayOfBytes(base::as_byte_span(peer->ssid)); entry.CloseContainer(&variant);
        });
        StringProperty(dictionary, "psk", peer->password);
        Property(dictionary, "psk-flags", [](Writer& entry) { entry.AppendVariantOfUint32(0); });
      }
    });
    Setting(settings, "ipv4", [&](Writer& dictionary) {
      StringProperty(dictionary, "method", client_ ? "auto" : "shared"); BoolProperty(dictionary, "never-default", true);
      if (client_) {
        BoolProperty(dictionary, "may-fail", false);
        BoolProperty(dictionary, "ignore-auto-dns", true);
        BoolProperty(dictionary, "ignore-auto-routes", true);
      }
    });
    Setting(settings, "ipv6", [](Writer& dictionary) {
      StringProperty(dictionary, "method", "link-local"); BoolProperty(dictionary, "never-default", true);
    });
    writer.CloseContainer(&settings); writer.AppendObjectPath(device_); writer.AppendObjectPath(Path("/"));
    writer.OpenArray("{sv}", &options);
    StringProperty(options, "persist", "volatile"); StringProperty(options, "bind-activation", "dbus-client");
    writer.CloseContainer(&options);
  }, deadline, active);
  if (!activation) return false; // Closing the lease also covers a lost activation reply.
  Reader activation_reader(activation.get());
  if (!activation_reader.PopObjectPath(&profile_) || !activation_reader.PopObjectPath(&connection_)) return false;
  stage_ = "wait_activated";
  base::TimeTicks next_diagnostic;
  uint32_t last_state = 0, last_reason = 0;
  for (;;) {
    uint32_t state = 0;
    Path station_current;
    // The activation reply can precede assignment of Device.ActiveConnection.
    // Check the station immediately, but wait for ACTIVATED before requiring
    // the P2P device to point at our active connection.
    if (!Current(station_, &station_current, deadline, active) || station_current != previous_ ||
        !Get(connection_, kConnection, "State", [&](Reader& reader) {
          return reader.PopUint32(&state);
        }, deadline, active)) return false;
    if (client_ && base::TimeTicks::Now() >= next_diagnostic) {
      uint32_t device_state = 0, reason = 0;
      if (Get(device_, kDevice, "StateReason", [&](Reader& reader) {
        Reader tuple(nullptr);
        return reader.PopStruct(&tuple) && tuple.PopUint32(&device_state) && tuple.PopUint32(&reason);
      }, std::min(deadline, base::TimeTicks::Now() + base::Milliseconds(200)), active) &&
          (device_state != last_state || reason != last_reason)) {
        LOG(INFO) << "crQuickShare: Direct client NM state; device_state=" << device_state
                  << "; reason=" << reason << "; active_state=" << state;
        last_state = device_state; last_reason = reason;
      }
      next_diagnostic = base::TimeTicks::Now() + base::Milliseconds(500);
    }
    if (state == 3 || state == 4) return false;
    if (state == 2) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (client_) {
    stage_ = "read_client_interface";
    if (!Get(device_, kDevice, "IpInterface", [&](Reader& reader) {
      return reader.PopString(&group_interface_);
    }, deadline, active) || group_interface_.empty() || group_interface_ == selected ||
        group_interface_.size() >= IFNAMSIZ) return false;
    group_index_ = if_nametoindex(group_interface_.c_str());
    if (!group_index_ || !Healthy(deadline, active)) return false;
    LOG(INFO) << "crQuickShare: Direct client ready; backend=NetworkManager; interface_index="
              << group_index_ << "; station_preserved=1";
    return true;
  }
  stage_ = "read_credentials";
  auto secrets = Call(device_, kP2p, "GetGroupCredentials", [](Writer&) {}, deadline, active);
  if (!secrets) return false;
  Reader secret_reader(secrets.get()), dictionary(nullptr);
  if (!secret_reader.PopArray(&dictionary)) return false;
  uint32_t frequency = 0;
  while (dictionary.HasMoreData()) {
    Reader entry(nullptr), value(nullptr); std::string key;
    if (!dictionary.PopDictEntry(&entry) || !entry.PopString(&key) || !entry.PopVariant(&value)) return false;
    if (key == "ssid") {
      base::span<const uint8_t> bytes;
      if (!value.PopArrayOfBytes(&bytes)) return false;
      credentials.ssid.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    } else if (key == "passphrase") { if (!value.PopString(&credentials.password)) return false;
    } else if (key == "frequency") { if (!value.PopUint32(&frequency)) return false;
    } else if (key == "interface") { if (!value.PopString(&group_interface_)) return false; }
  }
  if (credentials.ssid.empty() || credentials.ssid.size() > 32 || credentials.password.size() < 8 ||
      credentials.password.size() > 63 || frequency < 2300 || frequency > 7125 ||
      group_interface_.empty() || group_interface_.size() >= IFNAMSIZ || group_interface_ == selected) return false;
  credentials.frequency = static_cast<int>(frequency);
  stage_ = "bind_listener";
  if (!Listen(deadline, active) || !Healthy(deadline, active)) return false;
  LOG(INFO) << "crQuickShare: Direct GO ready; backend=NetworkManager; interface_index=" << group_index_
            << "; port=" << credentials.port << "; frequency=" << credentials.frequency;
  return true;
}
ChannelConnectResult Group::Connect(const HotspotCredentials& peer, base::TimeTicks deadline, const Active& active) {
  stage_ = "client_local_route";
  in_addr gateway{};
  if (inet_pton(AF_INET, peer.gateway.c_str(), &gateway) != 1 || !Healthy(deadline, active))
    return {nullptr, "direct_client_network_changed", 0};
  ifaddrs* addresses = nullptr;
  if (getifaddrs(&addresses)) return {nullptr, "direct_client_addresses_failed", errno};
  std::string source;
  for (auto* item = addresses; item; item = item->ifa_next) {
    if (!item->ifa_addr || !item->ifa_netmask || group_interface_ != item->ifa_name ||
        item->ifa_addr->sa_family != AF_INET) continue;
    const auto local = reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr.s_addr;
    const auto mask = reinterpret_cast<sockaddr_in*>(item->ifa_netmask)->sin_addr.s_addr;
    const auto remote = gateway.s_addr;
    if (!mask || local == remote || (local & mask) != (remote & mask) ||
        (remote & ~mask) == 0 || (remote & ~mask) == ~mask) continue;
    char address[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr, address, sizeof(address))) {
      source = address; break;
    }
  }
  freeifaddrs(addresses);
  if (source.empty()) return {nullptr, "direct_client_local_route_missing", 0};
  stage_ = "client_connect_tcp";
  auto result = ConnectTcpChannel(peer.gateway, static_cast<uint16_t>(peer.port), deadline, active, source, group_index_);
  if (result.channel && !Healthy(deadline, active)) {
    result.channel->Close(); return {nullptr, "direct_client_network_changed", 0};
  }
  if (result.channel) result.channel = std::make_unique<DirectChannel>(std::move(result.channel));
  LOG(INFO) << "crQuickShare: Direct client TCP connect; outcome=" << result.outcome
            << "; errno=" << result.system_error << "; interface_index=" << group_index_;
  return result;
}
bool Group::Listen(base::TimeTicks deadline, const Active& active) {
  std::string ip_interface;
  Path ip_config;
  if (!Get(device_, kDevice, "IpInterface", [&](Reader& reader) { return reader.PopString(&ip_interface); }, deadline, active) ||
      ip_interface != group_interface_ || !Get(device_, kDevice, "Ip4Config", [&](Reader& reader) {
        return reader.PopObjectPath(&ip_config);
      }, deadline, active)) return false;
  // NM shared addressing is dynamic; do not assume 192.168.* or a /24 prefix.
  if (!Get(ip_config, "org.freedesktop.NetworkManager.IP4Config", "AddressData", [&](Reader& reader) {
    Reader addresses(nullptr);
    if (!reader.PopArray(&addresses)) return false;
    while (addresses.HasMoreData()) {
      Reader dictionary(nullptr); std::string address; uint32_t prefix = 0;
      if (!addresses.PopArray(&dictionary)) return false;
      while (dictionary.HasMoreData()) {
        Reader entry(nullptr), value(nullptr); std::string key;
        if (!dictionary.PopDictEntry(&entry) || !entry.PopString(&key) || !entry.PopVariant(&value)) return false;
        if (key == "address" && !value.PopString(&address)) return false;
        if (key == "prefix" && !value.PopUint32(&prefix)) return false;
      }
      in_addr parsed{};
      if (prefix > 0 && prefix <= 30 && inet_pton(AF_INET, address.c_str(), &parsed) == 1) {
        local_ = ntohl(parsed.s_addr); mask_ = 0xffffffffu << (32 - prefix);
        credentials.gateway = address; return true;
      }
    }
    return false;
  }, deadline, active)) return false;
  group_index_ = if_nametoindex(group_interface_.c_str());
  if (!group_index_) return false;
  ifaddrs* addresses = nullptr;
  if (getifaddrs(&addresses)) return false;
  bool attached = false;
  in6_addr link_local{}; bool have6 = false;
  for (auto* item = addresses; item; item = item->ifa_next) {
    if (!item->ifa_addr || group_interface_ != item->ifa_name) continue;
    if (item->ifa_addr->sa_family == AF_INET && item->ifa_netmask) {
      attached |= ntohl(reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr.s_addr) == local_ &&
                  ntohl(reinterpret_cast<sockaddr_in*>(item->ifa_netmask)->sin_addr.s_addr) == mask_;
    } else if (item->ifa_addr->sa_family == AF_INET6) {
      const auto& candidate = reinterpret_cast<sockaddr_in6*>(item->ifa_addr)->sin6_addr;
      if (IN6_IS_ADDR_LINKLOCAL(&candidate)) { link_local = candidate; have6 = true; }
    }
  }
  freeifaddrs(addresses);
  if (!attached) return false;
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(local_);
  listener_.reset(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!listener_.is_valid() || bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
      listen(listener_.get(), 4)) return false;
  socklen_t length = sizeof(address);
  if (getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &length)) return false;
  credentials.port = ntohs(address.sin_port);
  if (have6) {
    sockaddr_in6 address6{}; address6.sin6_family = AF_INET6; address6.sin6_addr = link_local;
    address6.sin6_port = address.sin_port; address6.sin6_scope_id = group_index_;
    listener6_.reset(socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    const int only6 = 1;
    if (listener6_.is_valid() && !setsockopt(listener6_.get(), IPPROTO_IPV6, IPV6_V6ONLY, &only6, sizeof(only6)) &&
        !bind(listener6_.get(), reinterpret_cast<sockaddr*>(&address6), sizeof(address6)) && !listen(listener6_.get(), 4))
      ipv6_address.assign(reinterpret_cast<const char*>(&link_local), sizeof(link_local));
    else listener6_.reset(); // IPv4 remains usable; never advertise an unbound IPv6 address.
  }
  return true;
}
std::unique_ptr<TransportChannel> Group::Accept(base::TimeTicks deadline, const Active& active) {
  base::TimeTicks next_health_check;
  while (active() && base::TimeTicks::Now() < deadline) {
    if (base::TimeTicks::Now() >= next_health_check) {
      if (!Healthy(deadline, active) || if_nametoindex(group_interface_.c_str()) != group_index_) return nullptr;
      next_health_check = base::TimeTicks::Now() + base::Milliseconds(500);
    }
    std::array<pollfd, 2> descriptors{{{listener_.get(), POLLIN, 0}, {listener6_.get(), POLLIN, 0}}};
    if (poll(descriptors.data(), descriptors.size(), 50) < 0) { if (errno == EINTR) continue; return nullptr; }
    int fd = descriptors[0].revents & POLLIN ? listener_.get() : descriptors[1].revents & POLLIN ? listener6_.get() : -1;
    if (fd < 0) continue;
    sockaddr_storage peer{}; socklen_t length = sizeof(peer);
    base::ScopedFD accepted(accept4(fd, reinterpret_cast<sockaddr*>(&peer), &length, SOCK_CLOEXEC | SOCK_NONBLOCK));
    if (!accepted.is_valid()) continue;
    if (peer.ss_family == AF_INET) {
      const uint32_t remote = ntohl(reinterpret_cast<const sockaddr_in*>(&peer)->sin_addr.s_addr);
      if ((remote & mask_) != (local_ & mask_) || remote == local_ ||
          remote == (local_ & mask_) || remote == (local_ | ~mask_)) continue;
    } else if (peer.ss_family == AF_INET6) {
      const auto* remote = reinterpret_cast<const sockaddr_in6*>(&peer);
      if (!IN6_IS_ADDR_LINKLOCAL(&remote->sin6_addr) || remote->sin6_scope_id != group_index_) continue;
    } else continue;
    auto channel = AdoptTcpChannel(std::move(accepted));
    if (channel) return std::make_unique<DirectChannel>(std::move(channel));
  }
  return nullptr;
}
bool Group::Cleanup(base::TimeTicks deadline, const Active& active) {
  listener_.reset(); listener6_.reset();
  if (cleanup_attempted_) return cleaned_;
  cleanup_attempted_ = true;
  bool released = !activation_started_;
  if (bus_ && connection_ != Path("/")) {
    Call(Path(kRoot), kNm, "DeactivateConnection", [&](Writer& writer) {
      writer.AppendObjectPath(connection_);
    }, deadline, active);
    // A successful method return initiates deactivation; wait for actual release.
    while (base::TimeTicks::Now() < deadline && active()) {
      std::vector<Path> connections;
      if (!Get(Path(kRoot), kNm, "ActiveConnections", [&](Reader& reader) {
        return reader.PopArrayOfObjectPaths(&connections);
      }, deadline, active)) break;
      if (std::find(connections.begin(), connections.end(), connection_) == connections.end() &&
          (!group_index_ || if_nametoindex(group_interface_.c_str()) != group_index_)) {
        auto profiles_reply = Call(Path("/org/freedesktop/NetworkManager/Settings"),
            "org.freedesktop.NetworkManager.Settings", "ListConnections", [](Writer&) {}, deadline, active);
        std::vector<Path> profiles;
        if (!profiles_reply || !Reader(profiles_reply.get()).PopArrayOfObjectPaths(&profiles)) break;
        if (std::find(profiles.begin(), profiles.end(), profile_) == profiles.end()) { released = true; break; }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  // Even with a cancelled/lost activation reply, NM's dbus-client binding owns
  // teardown. Closing this bus cannot deactivate the station or another client.
  CloseBus();
  cleaned_ = released;
  LOG(INFO) << "crQuickShare: Direct network cleanup; backend=NetworkManager; role="
            << (client_ ? "client" : "GO") << "; lease_released=1; success=" << released;
  return released;
}
}  // namespace
std::vector<WifiDirectInterface> GetLinuxWifiDirectInterfaces() {
  Group query;
  const auto deadline = base::TimeTicks::Now() + base::Seconds(3);
  const Active active = [] { return true; };
  if (!query.Open(deadline, active)) return {};
  return query.Interfaces(deadline, active);
}
WifiDirectGoFactory MakeLinuxWifiDirectGoFactory(const std::string& interface) {
  if (interface.size() >= IFNAMSIZ || interface.find('/') != std::string::npos) return {};
  return [interface](base::TimeTicks deadline, const Active& active) -> std::unique_ptr<WifiDirectGroup> {
    auto group = std::make_unique<Group>();
    if (group->Start(interface, deadline, active)) return group;
    LOG(WARNING) << "crQuickShare: Direct GO NM setup failed or cancelled; stage=" << group->stage();
    group->Cleanup(base::TimeTicks::Now() + base::Seconds(8), [] { return true; });
    return nullptr;
  };
}
WifiDirectClientConnector MakeLinuxWifiDirectClientConnector(const std::string& interface) {
  if (interface.size() >= IFNAMSIZ || interface.find('/') != std::string::npos) return {};
  return [interface](const HotspotCredentials& peer, base::TimeTicks deadline, const Active& active) -> HotspotUpgradeResult {
    in_addr address{};
    const bool hex_psk = peer.password.size() == 64 &&
        peer.password.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
    if (peer.ssid.empty() || peer.ssid.size() > 32 ||
        (!hex_psk && (peer.password.size() < 8 || peer.password.size() > 63)) ||
        peer.password.find('\0') != std::string::npos || !base::IsStringUTF8(peer.password) ||
        peer.gateway.find('\0') != std::string::npos ||
        peer.port < 1 || peer.port > 65535 ||
        (peer.frequency != -1 && peer.frequency != 0 && (peer.frequency < 2300 || peer.frequency > 7125)) ||
        inet_pton(AF_INET, peer.gateway.c_str(), &address) != 1 ||
        !address.s_addr || (ntohl(address.s_addr) >> 24) == 127 || (ntohl(address.s_addr) >> 28) >= 14)
      return {nullptr, nullptr, "invalid_direct_client_credentials"};
    auto network = std::make_unique<Group>();
    if (!network->Start(interface, deadline, active, &peer)) {
      LOG(WARNING) << "crQuickShare: Direct client NM setup failed or cancelled; stage=" << network->stage();
      const std::string reason = network->stage();
      const char* outcome = reason == "ap_mode_not_available" ? "direct_client_ap_mode_not_available" :
          reason == "interface_unavailable" ? "direct_client_interface_unavailable" : "direct_client_activation_failed";
      return {std::move(network), nullptr, outcome};
    }
    auto connected = network->Connect(peer, deadline, active);
    return {std::move(network), std::move(connected.channel), connected.outcome};
  };
}
}  // namespace crquickshare
