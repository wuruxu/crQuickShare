// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/ble_discovery.h"
#include "crQuickShare/bootstrap_probe.h"
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>
#include "base/containers/span.h"
#include "base/base64url.h"
#include "base/strings/stringprintf.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_view_util.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "crQuickShare/lan_discovery.h"
#include "crypto/hash.h"
#include "device/bluetooth/bluetooth_adapter.h"
#include "device/bluetooth/bluetooth_adapter_factory.h"
#include "device/bluetooth/bluetooth_device.h"
#include "device/bluetooth/bluetooth_discovery_filter.h"
#include "device/bluetooth/bluetooth_discovery_session.h"
#include "device/bluetooth/bluetooth_gatt_connection.h"
#include "device/bluetooth/bluetooth_remote_gatt_service.h"
#include "device/bluetooth/bluetooth_remote_gatt_characteristic.h"

namespace crquickshare {
namespace {
constexpr char kServiceUuid[] = "0000fef3-0000-1000-8000-00805f9b34fb";
constexpr char kDctServiceUuid[] = "0000fc73-0000-1000-8000-00805f9b34fb";
constexpr std::string_view kServiceHash = "\xfc\x9f\x5e";
constexpr auto kLifetime = base::Seconds(120);
constexpr size_t kMaxRecords = 128;

struct AdvertisementDetails {
  uint16_t psm = 0;
  size_t declared_payload = 0, available_payload = 0;
  size_t instant_connect_bytes = 0, unknown_tlvs = 0;
};
struct DctResult {
  Peer peer;
  uint16_t psm = 0;
  uint8_t dedup = 0;
  bool truncated = false;
  unsigned unknown_fields = 0;
};
// Nearby advertisements/dct_advertisement.cc and data_element.cc. DCT has
// no Sharing salt/key hash: keep its local identity separate from FEF3/LAN.
std::optional<DctResult> ParseDct(std::string_view bytes, const char*& reason) {
  reason = "dct_size";
  if (bytes.empty() || bytes.size() > 512) return std::nullopt;
  reason = "dct_version_or_reserved_bits";
  if (static_cast<uint8_t>(bytes.front()) != 0x20) return std::nullopt;
  bytes.remove_prefix(1);
  std::set<unsigned> fields;
  DctResult result;
  while (!bytes.empty()) {
    reason = "dct_element_header";
    const uint8_t header = static_cast<uint8_t>(bytes.front()); bytes.remove_prefix(1);
    size_t length = (header >> 4) & 7;
    unsigned type = header & 15;
    if (header & 0x80) {
      if (bytes.empty()) return std::nullopt;
      length = header & 127;
      type = static_cast<uint8_t>(bytes.front()); bytes.remove_prefix(1);
    }
    reason = "dct_element_bounds_or_duplicate";
    if (!type || length > bytes.size() || fields.size() >= 32 || !fields.insert(type).second) return std::nullopt;
    const auto value = bytes.substr(0, length); bytes.remove_prefix(length);
    if (type == 5) {
      // HKDF-SHA256(secret="NearbySharing", salt="DCT Protocol",
      // info="Service ID Hash", length=2), matching upstream.
      reason = "dct_service_hash";
      if (value != "\x6d\xfd") return std::nullopt;
    } else if (type == 4) {
      reason = "dct_psm";
      if (length != 2) return std::nullopt;
      result.psm = (static_cast<uint8_t>(value[0]) << 8) | static_cast<uint8_t>(value[1]);
      if (!result.psm) return std::nullopt;
    } else if (type == 7) {
      reason = "dct_device_information";
      if (length < 2 || length > 8) return std::nullopt;
      result.truncated = static_cast<uint8_t>(value[0]) & 0x80;
      result.dedup = static_cast<uint8_t>(value[0]) & 0x7f;
      result.peer.name = std::string(value.substr(1));
      if (!base::IsStringUTF8(result.peer.name) || std::ranges::any_of(result.peer.name,
          [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; })) return std::nullopt;
    } else {
      // DE has explicit lengths, unlike unknown FEF3 extra-mask bits.
      ++result.unknown_fields;
    }
  }
  reason = "dct_required_fields";
  if (!fields.contains(5) || !fields.contains(4) || !fields.contains(7)) return std::nullopt;
  result.peer.ble_only = true;
  if (result.truncated) result.peer.name += "…";
  reason = "recognized";
  return result;
}

// Bounded subset of Nearby's mediums/ble_v2/ble_advertisement.cc followed by
// connections/implementation/ble_advertisement.cc and sharing/advertisement.cc.
// FEF3 is shared by Nearby services. UUID presence alone is never sufficient.
// GATT headers are resolved separately; a header, DCT or an unknown format
// alone must not produce a selectable Quick Share device.
std::optional<Peer> ParseAdvertisement(std::string_view bytes, const char*& reason,
                                     AdvertisementDetails* details = nullptr) {
  reason = "medium_size";
  if (bytes.size() < 2 || bytes.size() > 512) return std::nullopt;
  const uint8_t header = static_cast<uint8_t>(bytes.front());
  const unsigned version = header >> 5, socket_version = (header >> 2) & 7;
  reason = "medium_version";
  if (version < 1 || version > 2) return std::nullopt;
  reason = "socket_version";
  if (socket_version < 1 || socket_version > 2) return std::nullopt;
  const bool fast = header & 2;
  bytes.remove_prefix(1);
  size_t length = 0;
  if (fast) {
    // The compact layout flag does not prove this came from a legacy 31-byte
    // advertising packet. BlueZ has reported 92-byte service data with it set.
    // Use the encoded length and the overall cap, not the legacy 27-byte cap.
    length = static_cast<uint8_t>(bytes.front()); bytes.remove_prefix(1);
  } else {
    reason = "regular_medium_service_hash";
    if (bytes.size() < 7 || bytes.substr(0, 3) != kServiceHash) return std::nullopt;
    bytes.remove_prefix(3);
    for (size_t i = 0; i < 4; ++i) length = (length << 8) | static_cast<uint8_t>(bytes[i]);
    bytes.remove_prefix(4);
  }
  reason = "medium_payload_length";
  if (details) { details->declared_payload = length; details->available_payload = bytes.size(); }
  if (length > bytes.size()) return std::nullopt;
  auto connection = bytes.substr(0, length);
  auto extra = bytes.substr(length);
  // Optional two-byte token followed by the known extra-field mask. Validate
  // lengths even though discovery does not use the L2CAP or instant-connect data.
  reason = "medium_extensions";
  if (!extra.empty()) {
    if (extra.size() < 2) return std::nullopt;
    extra.remove_prefix(2);
    if (!extra.empty()) {
      const uint8_t mask = static_cast<uint8_t>(extra.front()); extra.remove_prefix(1);
      reason = "unknown_medium_extension_mask";
      if (mask & ~3) return std::nullopt;
      reason = "medium_extension_bounds";
      if (mask & 1) {
        if (extra.size() < 2) return std::nullopt;
        if (details) details->psm = (static_cast<uint8_t>(extra[0]) << 8) | static_cast<uint8_t>(extra[1]);
        extra.remove_prefix(2);
      }
      if (mask & 2) {
        if (extra.empty()) return std::nullopt;
        const size_t count = static_cast<uint8_t>(extra.front()); extra.remove_prefix(1);
        if (extra.size() < count) return std::nullopt;
        if (details) details->instant_connect_bytes = count;
        extra.remove_prefix(count);
      }
      reason = "medium_extension_trailing_bytes";
      if (!extra.empty()) return std::nullopt;
    }
  }
  // V1, P2P_POINT_TO_POINT is the strategy used by NearbySharing.
  reason = "connection_version_or_strategy";
  if (connection.empty() || static_cast<uint8_t>(connection.front()) != 0x23) return std::nullopt;
  connection.remove_prefix(1);
  if (!fast) {
    reason = "connection_service_hash";
    if (connection.size() < 3 || connection.substr(0, 3) != kServiceHash) return std::nullopt;
    connection.remove_prefix(3);
  }
  reason = "endpoint_header";
  if (connection.size() < 5) return std::nullopt;
  Peer result;
  result.id = std::string(connection.substr(0, 4));
  length = static_cast<uint8_t>(connection[4]); connection.remove_prefix(5);
  reason = "endpoint_info_length";
  // Accept a public name in a larger compact advertisement too. Retain the
  // normal endpoint-info bound and validate the name/TLV structure below.
  if (length < 17 || length > 131 || length > connection.size()) return std::nullopt;
  result.endpoint_info = std::string(connection.substr(0, length)); connection.remove_prefix(length);
  if (!fast) {
    reason = "connection_bluetooth_address";
    if (connection.size() < 6) return std::nullopt;
    const auto mac = connection.substr(0, 6);
    if (std::ranges::any_of(mac, [](char c) { return c != 0; }) &&
        !std::ranges::all_of(mac, [](char c) { return uint8_t(c) == 255; })) {
      const auto hex = base::HexEncode(mac);
      for (size_t i = 0; i < 6; ++i) {
        if (i) result.classic_bluetooth_address += ':';
        result.classic_bluetooth_address += hex.substr(i * 2, 2);
      }
    }
    connection.remove_prefix(6);
  }
  reason = "connection_extensions";
  if (!connection.empty()) {
    length = static_cast<uint8_t>(connection.front()); connection.remove_prefix(1);
    if (length > connection.size()) return std::nullopt;
    connection.remove_prefix(length);
    if (!fast && !connection.empty()) connection.remove_prefix(1);  // WebRTC flag.
    if (!connection.empty()) return std::nullopt;
  }
  const uint8_t info_header = static_cast<uint8_t>(result.endpoint_info.front());
  reason = "sharing_version";
  if ((info_header >> 5) > 1) return std::nullopt;
  size_t metadata_end = 17;
  if (!(info_header & 0x10)) {
    reason = "sharing_public_name";
    result.name = EndpointName(result.endpoint_info);
    if (result.name.empty()) return std::nullopt;
    metadata_end = 18 + static_cast<uint8_t>(result.endpoint_info[17]);
  }
  reason = "sharing_extensions";
  auto extensions = std::string_view(result.endpoint_info).substr(metadata_end);
  while (!extensions.empty()) {
    if (extensions.size() < 2) return std::nullopt;
    const uint8_t type = static_cast<uint8_t>(extensions[0]);
    length = static_cast<uint8_t>(extensions[1]); extensions.remove_prefix(2);
    if (length > extensions.size()) return std::nullopt;
    // sharing/advertisement.cc: QR=1, vendor ID=2 (exactly one byte).
    if (type == 2 && length != 1) { reason = "sharing_vendor_length"; return std::nullopt; }
    if (details && type != 1 && type != 2) ++details->unknown_tlvs;
    extensions.remove_prefix(length);
  }
  reason = "recognized";
  result.ble_only = true;
  return result;
}
// Nearby instant_on_lost_advertisement.cc uses a different header layout:
// type=1, version=1, then a count and four bytes per advertisement hash.
// Classify it before attempting the medium header. It contains no device name.
bool IsOnLostAdvertisement(std::string_view bytes) {
  if (bytes.size() < 2) return false;
  const uint8_t header = static_cast<uint8_t>(bytes[0]);
  const size_t count = static_cast<uint8_t>(bytes[1]) & 7;
  return (header & 0xf0) == 0x10 && (header & 7) == 1 &&
      count >= 1 && count <= 5 && bytes.size() == 2 + count * 4;
}
// Nearby ble_advertisement_header.cc: V2, 10-byte bloom filter, 4-byte
// advertisement hash, optional PSM. Older implementations base64url-encode it.
struct GattHeader { std::string bytes; unsigned slots; };
std::optional<GattHeader> ParseGattHeader(std::string_view bytes) {
  std::string decoded;
  if (bytes.size() != 15 && bytes.size() != 17) {
    if (bytes.size() > 24 || !base::Base64UrlDecode(bytes,
        base::Base64UrlDecodePolicy::IGNORE_PADDING, &decoded)) return std::nullopt;
    bytes = decoded;
  }
  if (bytes.size() != 15 && bytes.size() != 17) return std::nullopt;
  const uint8_t header = static_cast<uint8_t>(bytes[0]);
  const unsigned slots = header & 15;
  if ((header >> 5) != 2 || !slots) return std::nullopt;
  // bloom_filter.cc uses MurmurHash3_x64_128("NearbySharing", seed=0),
  // low64=0x798579c252d5fe64, five combined signed-32 hashes modulo 80.
  // The bloom match is only a connection hint; each read must match both
  // service hashes in the full advertisement before it is exposed to the UI.
  for (unsigned bit : {57u, 56u, 5u, 28u, 33u}) {
    if (!(static_cast<uint8_t>(bytes[1 + bit / 8]) & (1u << (bit % 8)))) return std::nullopt;
  }
  return GattHeader{std::string(bytes), slots};
}
bool SameDiscoveryKey(const Peer& a, const Peer& b) {
  return a.endpoint_info.size() >= 17 && b.endpoint_info.size() >= 17 &&
      a.endpoint_info.compare(1, 16, b.endpoint_info, 1, 16) == 0;
}
bool SameEndpoint(const Peer& a, const Peer& b) {
  // Salt and metadata-key hash occupy bytes 1..16. Bit 4 of the header only
  // indicates whether a public name is present (sharing/advertisement.cc).
  // Keep all other header bits and the endpoint ID in the comparison. These
  // untrusted discovery hints are never account or pairing verification.
  return a.id == b.id && SameDiscoveryKey(a, b) &&
      ((static_cast<uint8_t>(a.endpoint_info[0]) ^
        static_cast<uint8_t>(b.endpoint_info[0])) & 0xef) == 0;
}
bool SameBluetoothRoute(const Peer& a, const Peer& b) {
  // Only coalesce BLE list entries. A protocol-provided address is a routing
  // hint, not authenticated identity, and must not weaken LAN association.
  return a.ble_only && b.ble_only && a.id.size() == 4 && a.id == b.id &&
      !a.classic_bluetooth_address.empty() &&
      a.classic_bluetooth_address == b.classic_bluetooth_address;
}
std::string AdvertisementHash(std::string_view bytes) {
  const auto digest = crypto::hash::Sha256(bytes);
  return std::string(base::as_string_view(base::span(digest).first<4>()));
}
std::string SystemName(device::BluetoothAdapter* adapter, const std::string& address) {
  auto* device = adapter ? adapter->GetDevice(address) : nullptr;
  if (!device) return {};
  const auto name = device->GetName();
  if (!name || name->empty() || name->size() > 248 || !base::IsStringUTF8(*name)) return {};
  if (std::ranges::any_of(*name, [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; })) return {};
  return *name;
}
}  // namespace
struct BleDiscovery::Impl : device::BluetoothAdapter::Observer {
  struct Record { Peer peer; base::TimeTicks seen; std::string advertisement_hash; };
  struct Observation {
    std::string bytes;
    std::optional<int8_t> rssi;
    base::TimeTicks seen;
  };
  std::map<std::string, Observation> observations;
  std::map<std::string, base::TimeTicks> lost_hashes, lost_headers;
  uint64_t lost_removed = 0, lost_suppressed = 0, property_only = 0;
  BootstrapProbe bootstrap;
  bool bootstrap_waiting_for_gatt = false;
  base::TimeTicks discovery_started;
  bool first_candidate_logged = false, first_connectable_logged = false;
  scoped_refptr<device::BluetoothAdapter> adapter;
  std::unique_ptr<device::BluetoothDiscoverySession> session;
  std::map<std::string, Record> records;
  std::map<std::string, Record> dct_records;
  struct ParseCount { uint64_t count = 0; base::TimeTicks next_log; };
  std::map<std::string, ParseCount> parse_counts;
  uint64_t dct_recognized = 0, extended_recognized = 0, parsed_extensions = 0;
  std::set<std::string> starting_devices;
  struct GattEntry {
    GattHeader header;
    base::TimeTicks seen, retry, last_attempt;
    // A fresh header proves presence, not freshness of previously read slots.
    std::map<unsigned, Record> slots;
    base::TimeTicks resolved = {};  // Full read started here; advertising never renews it.
    unsigned failures = 0;
    bool skip_logged = false;
  };
  struct GattRequest {
    uint64_t token;
    std::string address, header;
    unsigned slots = 0, next_slot = 0;
    bool reading = false, services_discovered = false;
    unsigned read_failures = 0;
    std::set<unsigned> short_read_retried;
    base::TimeTicks deadline, started, stage_started;
    const char* stage = "connect_callback";
    std::unique_ptr<device::BluetoothGattConnection> connection;
    // Missing key: read failed; null value: successfully read, no endpoint.
    std::map<unsigned, std::optional<Record>> results;
    std::set<std::string> advertisement_hashes;
  };
  std::map<std::string, GattEntry> gatt_entries;
  std::optional<GattRequest> gatt_request;
  uint64_t next_gatt_token = 0, gatt_attempts = 0, gatt_reads = 0;
  uint64_t gatt_expired_slots = 0, gatt_partial_refreshes = 0;
  uint64_t gatt_ready_skips = 0, gatt_backoffs = 0;
  bool gatt_connect_pending = false;
  base::OnceClosure connection_ready_callback;
  base::TimeTicks connection_wait_started;
  uint64_t pending_gatt_token = 0;
  std::string pending_gatt_address;
  base::TimeTicks pending_gatt_started, next_gatt_blocked_log;
  base::TimeTicks next_gatt_start;
  base::TimeTicks next_association_log;
  State current = State::kIdle;
  bool wanted = false, initializing = false, pending = false;
  bool retain_cache = false;
  base::TimeTicks retry_after, next_ignored_log, next_summary;
  uint64_t ignored = 0, notifications = 0, fef3 = 0, dct = 0, fast_initiation = 0, recognized = 0;
  uint64_t on_lost = 0, named_updates = 0, large_fast = 0;
  base::WeakPtrFactory<Impl> weak{this};
  Impl() {
    if (!device::BluetoothAdapterFactory::IsBluetoothSupported()) return;
    initializing = true;
    device::BluetoothAdapterFactory::Get()->GetAdapter(base::BindOnce(&Impl::OnAdapter, weak.GetWeakPtr()));
  }
  ~Impl() override {
    weak.InvalidateWeakPtrs();
    if (adapter) adapter->RemoveObserver(this);
    gatt_request.reset();
    session.reset();
  }
  void OnAdapter(scoped_refptr<device::BluetoothAdapter> value) {
    initializing = false; adapter = std::move(value);
    adapter->AddObserver(this); Reconcile();
  }
  void Reconcile() {
    const auto now = base::TimeTicks::Now();
    std::erase_if(lost_headers, [&](const auto& entry) { return entry.second <= now; });
    std::erase_if(lost_hashes, [&](const auto& entry) { return entry.second <= now; });
    std::erase_if(records, [&](const auto& entry) { return now - entry.second.seen >= kLifetime; });
    std::erase_if(dct_records, [&](const auto& entry) { return now - entry.second.seen >= kLifetime; });
    for (auto& [address, entry] : gatt_entries) {
      gatt_expired_slots += std::erase_if(entry.slots, [&](const auto& slot) {
        return now - slot.second.seen >= kLifetime;
      });
    }
    std::erase_if(gatt_entries, [&](const auto& entry) { return now - entry.second.seen >= kLifetime; });
    if (!wanted || !adapter || !adapter->IsPresent() || !adapter->IsPowered()) {
      FinishGatt(retain_cache ? "paused" : "stopped", false);
      session.reset(); starting_devices.clear(); observations.clear();
      if (!retain_cache || !adapter || !adapter->IsPresent() || !adapter->IsPowered()) {
        gatt_entries.clear(); records.clear(); dct_records.clear(); lost_hashes.clear(); lost_headers.clear();
      }
      current = !wanted ? State::kIdle : initializing ? State::kStarting : State::kUnavailable;
      return;
    }
    // Service discovery/read callbacks may never arrive after the LE link
    // drops. Release this request promptly instead of consuming its deadline.
    if (gatt_request && gatt_request->connection && !gatt_request->connection->IsConnected())
      FinishGatt("disconnected", false, true);
    if (gatt_request && now >= gatt_request->deadline) FinishGatt("timed_out", false, true);
    if (pending) { current = State::kStarting; return; }
    if (session && session->IsActive()) {
      current = State::kScanning;
      PumpGatt();
      if (now >= next_summary) {
        next_summary = now + base::Seconds(15);
        size_t gatt_endpoints = 0, gatt_named_endpoints = 0;
        for (const auto& [address, entry] : gatt_entries) {
          gatt_endpoints += entry.slots.size();
          gatt_named_endpoints += std::ranges::count_if(entry.slots, [](const auto& slot) { return !slot.second.peer.name.empty(); });
        }
        LOG(INFO) << "crQuickShare: BLE discovery summary; notifications=" << notifications
                  << "; fef3=" << fef3 << "; dct_fc73=" << dct
                  << "; fe2c=" << fast_initiation << "; recognized=" << recognized
                  << "; ignored=" << ignored << "; on_lost=" << on_lost
                  << "; lost_removed=" << lost_removed << "; lost_suppressed=" << lost_suppressed
                  << "; property_only=" << property_only
                  << "; gatt_attempts=" << gatt_attempts << "; gatt_reads=" << gatt_reads
                  << "; gatt_expired_slots=" << gatt_expired_slots << "; gatt_partial_refreshes=" << gatt_partial_refreshes
                  << "; gatt_ready_skips=" << gatt_ready_skips << "; gatt_backoffs=" << gatt_backoffs
                  << "; dct_recognized=" << dct_recognized << "; dct_endpoints=" << dct_records.size()
                  << "; extended_recognized=" << extended_recognized << "; parsed_extensions=" << parsed_extensions
                  << "; gatt_candidates=" << gatt_entries.size()
                  << "; gatt_endpoints=" << gatt_endpoints << "; gatt_named_endpoints=" << gatt_named_endpoints
                  << "; named_updates=" << named_updates << "; large_fast_recognized=" << large_fast
                  << "; cached_endpoints=" << records.size()
                  << "; named_endpoints=" << std::ranges::count_if(records, [](const auto& item) {
                       return !item.second.peer.name.empty();
                     });
      }
      return;
    }
    session.reset();
    if (now < retry_after) { current = State::kUnavailable; return; }
    auto filter = std::make_unique<device::BluetoothDiscoveryFilter>(device::BLUETOOTH_TRANSPORT_LE);
    // Scan LE without an OS UUID constraint. Identify receivers from service
    // data below; merely seeing a device or UUID never adds it to the UI.
    // This also exposes counters for unsupported Nearby formats such as DCT.
    starting_devices.clear();
    pending = true; current = State::kStarting;
    adapter->StartDiscoverySessionWithFilter(std::move(filter), "crQuickShare",
        base::BindOnce(&Impl::OnStarted, weak.GetWeakPtr()), base::BindOnce(&Impl::OnError, weak.GetWeakPtr()));
  }
  void OnStarted(std::unique_ptr<device::BluetoothDiscoverySession> value) {
    pending = false; session = std::move(value);
    discovery_started = base::TimeTicks::Now();
    first_candidate_logged = first_connectable_logged = false;
    auto observed_during_start = std::move(starting_devices);
    starting_devices.clear();
    next_summary = base::TimeTicks::Now() + base::Seconds(15);
    LOG(INFO) << "crQuickShare: BLE discovery session started; filter=le"
              << "; startup_devices=" << observed_during_start.size();
    // Late callbacks must release the session if the dialog has closed.
    Reconcile();
    // Replay only notifications received during this start operation, not
    // BlueZ's arbitrary historical cache. BlueZ can notify before OnStarted.
    for (const auto& address : observed_during_start) {
      if (auto* device = adapter->GetDevice(address)) Observe(device);
    }
  }
  void OnError() {
    pending = false; starting_devices.clear(); retry_after = base::TimeTicks::Now() + base::Seconds(15);
    current = wanted ? State::kUnavailable : State::kIdle;
    LOG(WARNING) << "crQuickShare: BLE discovery unavailable; retry_delay_s=15";
  }
  void RememberLost(std::map<std::string, base::TimeTicks>& cache,
                    const std::string& key, base::TimeTicks now) {
    if (!cache.contains(key) && cache.size() >= kMaxRecords) {
      const auto oldest = std::ranges::min_element(cache, {}, [](const auto& item) { return item.second; });
      cache.erase(oldest);
    }
    cache.insert_or_assign(key, now + base::Seconds(60));
  }
  void DiagnoseParse(const char* format, const char* reason, size_t bytes) {
    const std::string key = std::string(format) + ":" + reason;
    // Keys are parser constants, not raw advertisement content.
    if (!parse_counts.contains(key) && parse_counts.size() >= 64) return;
    auto& counter = parse_counts[key]; ++counter.count;
    const auto now = base::TimeTicks::Now();
    if (now < counter.next_log) return;
    counter.next_log = now + base::Seconds(30);
    LOG(INFO) << "crQuickShare: BLE format diagnostics; format=" << format
              << "; reason=" << reason << "; bytes=" << bytes << "; count=" << counter.count;
  }
  void ObserveDct(device::BluetoothDevice* device, std::string_view data, bool advertisement_event) {
    if (!wanted) return;
    if (pending) {
      if (starting_devices.size() < kMaxRecords) starting_devices.insert(device->GetAddress());
      return;
    }
    if (!session || !session->IsActive()) return;
    ++dct;
    if (data.size() > 512) { DiagnoseParse("dct", "dct_size", data.size()); return; }
    const std::string bytes(data), address = device->GetAddress();
    const std::string key = "dct:" + address;
    const auto now = base::TimeTicks::Now();
    const auto rssi = device->GetInquiryRSSI();
    const auto previous = observations.find(key);
    if (!advertisement_event && previous != observations.end() &&
        previous->second.bytes == bytes && previous->second.rssi == rssi) return;
    if (previous == observations.end() && observations.size() >= kMaxRecords) {
      const auto oldest = std::ranges::min_element(observations, {}, [](const auto& item) { return item.second.seen; });
      observations.erase(oldest);
    }
    observations.insert_or_assign(key, Observation{bytes, rssi, now});
    const char* reason = "unknown";
    auto result = ParseDct(bytes, reason);
    if (!result) { DiagnoseParse("dct", reason, bytes.size()); return; }
    ++dct_recognized;
    // Local per-address key only; never synthesize FEF3 identity material or
    // merge a truncated DCT name into a LAN record.
    result->peer.id = key + ":" + base::NumberToString(result->dedup);
    result->peer.bluetooth_address = address;
    if (!dct_records.contains(address) && dct_records.size() >= kMaxRecords) {
      const auto oldest = std::ranges::min_element(dct_records, {}, [](const auto& item) { return item.second.seen; });
      dct_records.erase(oldest);
    }
    DiagnoseParse("dct", result->truncated ? "recognized_truncated_name" : "recognized_public_name", bytes.size());
    if (result->unknown_fields) DiagnoseParse("dct", "unknown_length_delimited_fields_skipped", bytes.size());
    dct_records.insert_or_assign(address, Record{std::move(result->peer), now, {}});
  }
  void Observe(device::BluetoothDevice* device, bool advertisement_event = false,
               std::optional<std::string_view> received_fef3 = std::nullopt) {
    if (!wanted) return;
    if (pending) {
      if (starting_devices.size() < kMaxRecords) starting_devices.insert(device->GetAddress());
      return;
    }
    if (!session || !session->IsActive()) return;
    ++notifications;
    if (!received_fef3) {
      if (const auto* dct_data = device->GetServiceDataForUUID(device::BluetoothUUID(kDctServiceUuid)))
        ObserveDct(device, base::as_string_view(base::span(*dct_data)), false);
    }
    if (device->GetServiceDataForUUID(device::BluetoothUUID("0000fe2c-0000-1000-8000-00805f9b34fb"))) ++fast_initiation;
    const auto* data = device->GetServiceDataForUUID(device::BluetoothUUID(kServiceUuid));
    if (!received_fef3 && !data) return;
    ++fef3;
    const auto source = received_fef3 ? *received_fef3 : base::as_string_view(base::span(*data));
    if (source.size() > 512) { ++ignored; DiagnoseParse("fef3", "medium_size", source.size()); return; }
    // Own the bytes across GATT teardown, which can notify adapter observers.
    const std::string bytes(source);
    const auto observed_address = device->GetAddress();
    const auto observed_now = base::TimeTicks::Now();
    const auto rssi = device->GetInquiryRSSI();
    const auto previous = observations.find(observed_address);
    // BlueZ DeviceChanged also reports Name, Connected, UUIDs and other
    // properties. Unchanged service data/RSSI is not evidence of a new packet.
    if (!advertisement_event && previous != observations.end() &&
        previous->second.bytes == bytes && previous->second.rssi == rssi) {
      ++property_only;
      return;
    }
    if (previous == observations.end() && observations.size() >= kMaxRecords) {
      const auto oldest = std::ranges::min_element(observations, {}, [](const auto& item) { return item.second.seen; });
      observations.erase(oldest);
    }
    observations.insert_or_assign(observed_address, Observation{std::string(bytes), rssi, observed_now});
    if (IsOnLostAdvertisement(bytes)) {
      ++on_lost;
      size_t removed = 0;
      for (size_t offset = 2; offset < bytes.size(); offset += 4) {
        const std::string hash(bytes.substr(offset, 4));
        // Match the upstream 60-second instant-lost suppression window. The
        // sender address can differ from the address that advertised the data.
        RememberLost(lost_hashes, hash, observed_now);
        removed += std::erase_if(records, [&](const auto& item) { return item.second.advertisement_hash == hash; });
        // GATT header hashes aggregate all slots; OnLost instead hashes each
        // slot's medium advertisement. Never compare these two hash domains.
        if (gatt_request && gatt_request->advertisement_hashes.contains(hash)) {
          RememberLost(lost_headers, gatt_request->header, observed_now);
          FinishGatt("instant_on_lost", false);
        }
        removed += std::erase_if(gatt_entries, [&](const auto& item) {
          const bool matches = std::ranges::any_of(item.second.slots, [&](const auto& slot) {
            return slot.second.advertisement_hash == hash;
          });
          if (!matches && !lost_headers.contains(item.second.header.bytes)) return false;
          RememberLost(lost_headers, item.second.header.bytes, observed_now);
          return true;
        });
      }
      lost_removed += removed;
      if (removed || on_lost == 1)
        LOG(INFO) << "crQuickShare: BLE instant on lost; hashes=" << (bytes.size() - 2) / 4
                  << "; removed_records=" << removed;
      return;
    }
    if (auto header = ParseGattHeader(bytes)) {
      if (auto lost = lost_headers.find(header->bytes);
          lost != lost_headers.end() && lost->second > observed_now) { ++lost_suppressed; return; }
      const auto address = device->GetAddress();
      auto entry = gatt_entries.find(address);
      if (entry == gatt_entries.end() && gatt_entries.size() >= kMaxRecords) return;
      const auto now = base::TimeTicks::Now();
      if (entry == gatt_entries.end()) {
        gatt_entries.emplace(address, GattEntry{std::move(*header), now, {}, {}, {}});
        LOG(INFO) << "crQuickShare: BLE GATT header discovered";
      } else {
        entry->second.seen = now;
        if (entry->second.header.bytes != header->bytes) {
          // Never carry identities across changed headers. Refresh sooner than
          // the normal retry, but rate-limit header rotation per address.
          const size_t cleared_slots = entry->second.slots.size();
          entry->second.header = std::move(*header); entry->second.slots.clear();
          entry->second.resolved = {}; entry->second.failures = 0; entry->second.skip_logged = false;
          entry->second.retry = std::max(now, entry->second.last_attempt + base::Seconds(5));
          LOG(INFO) << "crQuickShare: BLE GATT header changed; cached_slots_cleared=" << cleared_slots << "; refresh_delay_ms="
                    << (entry->second.retry - now).InMilliseconds();
          if (gatt_request && gatt_request->address == address) FinishGatt("header_changed", false);
        }
      }
      return;
    }
    const char* reason = "unknown";
    AdvertisementDetails details;
    auto peer = ParseAdvertisement(bytes, reason, &details);
    const auto now = base::TimeTicks::Now();
    if (!peer) {
      // A peripheral can alternate several advertising sets. An unsupported
      // frame must not evict its last valid record or extend that record's TTL.
      ++ignored;
      DiagnoseParse("fef3", reason, bytes.size());
      if (now >= next_ignored_log) {
        next_ignored_log = now + base::Seconds(30);
        LOG(INFO) << "crQuickShare: BLE service data ignored; reason=unsupported_or_incomplete_advertisement"
                  << "; parse_stage=" << reason
                  << "; medium_version=" << (bytes.empty() ? -1 : static_cast<uint8_t>(bytes.front()) >> 5)
                  << "; socket_version=" << (bytes.empty() ? -1 : (static_cast<uint8_t>(bytes.front()) >> 2) & 7)
                  << "; fast=" << (!bytes.empty() && (bytes.front() & 2))
                  << "; bytes=" << bytes.size() << "; count=" << ignored
                  << "; gatt_resolution_supported=1";
      }
      return;
    }
    const std::string advertisement_hash = AdvertisementHash(bytes);
    if (auto lost = lost_hashes.find(advertisement_hash);
        lost != lost_hashes.end() && lost->second > now) { ++lost_suppressed; return; }
    ++recognized;
    if (!peer->name.empty()) ++named_updates;
    if (bytes.size() > 27 && (bytes.front() & 2)) ++large_fast;
    if (bytes.size() > 27) ++extended_recognized;
    if (details.psm || details.instant_connect_bytes || details.unknown_tlvs) {
      ++parsed_extensions;
      DiagnoseParse("fef3", "recognized_extensions", bytes.size());
      if (details.unknown_tlvs) DiagnoseParse("fef3", "unknown_length_delimited_fields_skipped", bytes.size());
    }
    const auto address = device->GetAddress();
    if (!records.contains(address)) {
      if (records.size() >= kMaxRecords) {
        const auto oldest = std::ranges::min_element(records, {}, [](const auto& item) { return item.second.seen; });
        records.erase(oldest);
      }
      LOG(INFO) << "crQuickShare: BLE endpoint discovered; name_present=" << !peer->name.empty();
    }
    const auto existing = records.find(address);
    if (existing != records.end() && existing->second.peer.name.empty() && !peer->name.empty())
      LOG(INFO) << "crQuickShare: BLE endpoint updated; name_present=1";
    records.insert_or_assign(address, Record{std::move(*peer), now, advertisement_hash});
  }
  void NotifyConnectionReady() {
    if (!wanted && !gatt_request && !gatt_connect_pending && connection_ready_callback) {
      LOG(INFO) << "crQuickShare: BLE send connection ready; gatt_wait_ms="
                << (base::TimeTicks::Now() - connection_wait_started).InMilliseconds();
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, std::move(connection_ready_callback));
    }
  }
  void FinishGatt(const char* outcome, bool commit, bool allow_partial = false) {
    if (!gatt_request) return;
    // Clear active state before releasing the connection (BlueZ can notify).
    auto done = std::move(*gatt_request); gatt_request.reset();
    const auto now = base::TimeTicks::Now();
    next_gatt_start = now + base::Seconds(1);
    auto entry = gatt_entries.find(done.address);
    size_t updated = 0, cleared = 0;
    if (entry != gatt_entries.end() && entry->second.header.bytes == done.header) {
      const bool complete = commit && !done.read_failures;
      const bool failed = commit || std::string_view(outcome) == "connect_failed" ||
          std::string_view(outcome) == "disconnected" || std::string_view(outcome) == "timed_out" ||
          std::string_view(outcome) == "expired_or_timed_out" ||
          std::string_view(outcome) == "service_unavailable";
      if (complete) {
        entry->second.failures = 0;
        entry->second.resolved = done.started;
        entry->second.retry = now + base::Seconds(60);
      } else if (failed) {
        entry->second.failures = std::min(entry->second.failures + 1, 3u);
        const unsigned delay = 30u << (entry->second.failures - 1);
        entry->second.retry = now + base::Seconds(delay);
        ++gatt_backoffs;
        LOG(INFO) << "crQuickShare: BLE GATT retry deferred; request=" << done.token
                  << "; failures=" << entry->second.failures << "; retry_ms=" << delay * 1000
                  << "; reason=" << outcome;
      }
      // Pausing or cancellation is not a device failure. Partial reads cannot
      // establish that every slot is known, even if one endpoint is usable.
      if (!complete) entry->second.resolved = {};
      entry->second.skip_logged = false;
      if ((commit || allow_partial) && wanted && now - entry->second.seen < kLifetime &&
          !lost_headers.contains(done.header)) {
        // Failed slots keep their original expiry. Successful empty/foreign
        // slots remove only that slot, never erase other successfully read ones.
        for (auto& [slot, record] : done.results) {
          if (record) { entry->second.slots.insert_or_assign(slot, std::move(*record)); ++updated; }
          else cleared += entry->second.slots.erase(slot);
        }
        if (!complete && !done.results.empty()) ++gatt_partial_refreshes;
      }
    }
    auto* device = adapter ? adapter->GetDevice(done.address) : nullptr;
    LOG(INFO) << "crQuickShare: BLE GATT resolution finished; request=" << done.token
              << "; address=" << done.address << "; stage=" << done.stage
              << "; elapsed_ms=" << (now - done.started).InMilliseconds()
              << "; stage_elapsed_ms=" << (now - done.stage_started).InMilliseconds()
              << "; connect_pending=" << gatt_connect_pending
              << "; connection_present=" << bool(done.connection)
              << "; connection_connected=" << (done.connection && done.connection->IsConnected())
              << "; services_notified=" << done.services_discovered
              << "; device_present=" << bool(device)
              << "; gatt_connected=" << (device && device->IsGattConnected())
              << "; services_complete=" << (device && device->IsGattServicesDiscoveryComplete())
              << "; outcome=" << outcome << "; slots_visited=" << done.next_slot
              << "; read_failures=" << done.read_failures << "; slots_updated=" << updated << "; slots_cleared=" << cleared;
    done.connection.reset();
    NotifyConnectionReady();
  }
  void GattStage(const char* stage, device::BluetoothDevice* device) {
    if (!gatt_request || std::string_view(gatt_request->stage) == stage) return;
    const auto now = base::TimeTicks::Now();
    LOG(INFO) << "crQuickShare: BLE GATT stage; request=" << gatt_request->token
              << "; address=" << gatt_request->address << "; stage=" << stage
              << "; previous_stage=" << gatt_request->stage
              << "; previous_elapsed_ms=" << (now - gatt_request->stage_started).InMilliseconds()
              << "; elapsed_ms=" << (now - gatt_request->started).InMilliseconds()
              << "; device_present=" << bool(device)
              << "; gatt_connected=" << (device && device->IsGattConnected())
              << "; services_complete=" << (device && device->IsGattServicesDiscoveryComplete());
    gatt_request->stage = stage;
    gatt_request->stage_started = now;
  }
  bool SendReady(const Record& record, base::TimeTicks now) const {
    bool named = false, routed = false;
    auto include = [&](const Record& other) {
      if (now - other.seen >= kLifetime || !SameEndpoint(record.peer, other.peer)) return;
      named |= !EndpointName(other.peer.endpoint_info).empty();
      routed |= !other.peer.classic_bluetooth_address.empty();
    };
    include(record);
    if (named && routed) return true;
    // Associate only protocol discovery keys + endpoint/header, never display
    // names, BLE addresses, or a bloom-filter/header hash alone.
    for (const auto& [address, other] : records) include(other);
    for (const auto& [address, entry] : gatt_entries) {
      if (now - entry.seen >= kLifetime) continue;
      for (const auto& [slot, other] : entry.slots) include(other);
    }
    return named && routed;
  }
  int GattPriority(const GattEntry& entry, base::TimeTicks now) const {
    bool known = false;
    for (const auto& [slot, record] : entry.slots) {
      if (now - record.seen >= kLifetime) continue;
      known = true;
      if (!SendReady(record, now)) return 0;  // Known endpoint missing send metadata.
    }
    return known ? 2 : 1;  // Unknown headers still get their initial resolution.
  }
  void PumpGatt() {
    const auto now = base::TimeTicks::Now();
    std::erase_if(gatt_entries, [&](const auto& item) { return now - item.second.seen >= kLifetime; });
    if (gatt_request) {
      if (!gatt_entries.contains(gatt_request->address) || now >= gatt_request->deadline)
        FinishGatt("expired_or_timed_out", false, true);
      return;
    }
    // A timed-out connect has no reference to release yet. Wait for its OS
    // callback instead of accumulating uncancellable connection attempts.
    if (gatt_connect_pending) {
      if (now >= next_gatt_blocked_log) {
        LOG(INFO) << "crQuickShare: BLE GATT scheduler blocked; request=" << pending_gatt_token
                  << "; address=" << pending_gatt_address << "; reason=connect_callback_pending"
                  << "; elapsed_ms=" << (now - pending_gatt_started).InMilliseconds()
                  << "; candidates=" << gatt_entries.size();
        next_gatt_blocked_log = now + base::Seconds(5);
      }
      return;
    }
    if (bootstrap_waiting_for_gatt || bootstrap.active() || now < next_gatt_start) return;
    auto candidate = gatt_entries.end();
    int selected_priority = 3;
    for (auto it = gatt_entries.begin(); it != gatt_entries.end(); ++it) {
      if (now < it->second.retry || !adapter->GetDevice(it->first)) continue;
      auto& entry = it->second;
      const int priority = GattPriority(entry, now);
      // Refresh before slot expiry so skipping a redundant read does not
      // deliberately create a gap in the visible device cache.
      if (priority == 2 && !entry.resolved.is_null() &&
          now - entry.resolved < kLifetime - base::Seconds(15)) {
        if (!entry.skip_logged) {
          entry.skip_logged = true; ++gatt_ready_skips;
          LOG(INFO) << "crQuickShare: BLE GATT resolution skipped; reason=send_metadata_ready"
                    << "; known_slots=" << entry.slots.size()
                    << "; resolved_age_ms=" << (now - entry.resolved).InMilliseconds();
        }
        continue;
      }
      entry.skip_logged = false;
      if (candidate == gatt_entries.end() || priority < selected_priority ||
          (priority == selected_priority && entry.retry < candidate->second.retry)) {
        candidate = it; selected_priority = priority;
      }
    }
    if (candidate != gatt_entries.end()) {
      auto& [address, entry] = *candidate;
      auto* device = adapter->GetDevice(address);
      entry.retry = now + base::Seconds(30);
      entry.last_attempt = now;
      LOG(INFO) << "crQuickShare: BLE GATT candidate selected; priority="
                << (selected_priority == 0 ? "missing_metadata" : selected_priority == 1 ? "unknown" : "refresh")
                << "; failures=" << entry.failures;
      gatt_request.emplace();
      gatt_request->token = ++next_gatt_token;
      gatt_request->address = address; gatt_request->header = entry.header.bytes;
      gatt_request->slots = entry.header.slots;
      gatt_request->started = now;
      gatt_request->stage_started = now;
      gatt_request->deadline = now + base::Seconds(15);
      gatt_connect_pending = true; ++gatt_attempts;
      pending_gatt_token = next_gatt_token;
      pending_gatt_address = address;
      pending_gatt_started = now;
      next_gatt_blocked_log = now;
      LOG(INFO) << "crQuickShare: BLE GATT connect requested; request=" << next_gatt_token
                << "; address=" << address << "; stage=connect_callback"
                << "; gatt_connected=" << device->IsGattConnected()
                << "; services_complete=" << device->IsGattServicesDiscoveryComplete()
                << "; slots=" << entry.header.slots;
      device->CreateGattConnection(base::BindOnce(&Impl::OnGattConnected, weak.GetWeakPtr(), next_gatt_token, address, now),
                                  device::BluetoothUUID(kServiceUuid));
      return;
    }
  }
  void OnGattConnected(uint64_t token, const std::string& address, base::TimeTicks started,
                       std::unique_ptr<device::BluetoothGattConnection> connection,
                       std::optional<device::BluetoothDevice::ConnectErrorCode> error) {
    if (pending_gatt_token == token) {
      gatt_connect_pending = false;
      pending_gatt_token = 0;
      pending_gatt_address.clear();
    }
    auto* device = adapter ? adapter->GetDevice(address) : nullptr;
    LOG(INFO) << "crQuickShare: BLE GATT connect callback; request=" << token
              << "; address=" << address
              << "; elapsed_ms=" << (base::TimeTicks::Now() - started).InMilliseconds()
              << "; stale=" << (!gatt_request || gatt_request->token != token || !wanted)
              << "; error=" << (error ? static_cast<int>(*error) : -1)
              << "; connection_present=" << bool(connection)
              << "; connection_connected=" << (connection && connection->IsConnected())
              << "; device_present=" << bool(device)
              << "; gatt_connected=" << (device && device->IsGattConnected())
              << "; services_complete=" << (device && device->IsGattServicesDiscoveryComplete());
    if (!gatt_request || gatt_request->token != token || !wanted) {
      connection.reset();
      NotifyConnectionReady();
      return;
    }
    if (error || !connection || !connection->IsConnected()) { FinishGatt("connect_failed", false); return; }
    gatt_request->connection = std::move(connection);
    GattStage("wait_services", device);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&Impl::ReadGattSlot, weak.GetWeakPtr(), token));
  }
  void ReadGattSlot(uint64_t token) {
    if (gatt_request && gatt_request->token == token &&
        lost_headers.contains(gatt_request->header)) {
      FinishGatt("instant_on_lost", false);
      return;
    }
    if (!gatt_request || gatt_request->token != token || gatt_request->reading || !wanted) return;
    if (base::TimeTicks::Now() >= gatt_request->deadline) { FinishGatt("timed_out", false, true); return; }
    if (!gatt_request->connection) return;
    if (!gatt_request->connection->IsConnected()) { FinishGatt("disconnected", false, true); return; }
    auto* device = adapter->GetDevice(gatt_request->address);
    if (!device) { FinishGatt("device_removed", false); return; }
    // BlueZ exposes ServicesResolved on the device; its per-service
    // IsDiscoveryComplete flag is not maintained by this backend.
    if (!gatt_request->services_discovered && !device->IsGattServicesDiscoveryComplete()) return;
    GattStage("find_service", device);
    device::BluetoothRemoteGattService* target = nullptr;
    for (auto* service : device->GetGattServices()) {
      if (service->GetUUID() == device::BluetoothUUID(kServiceUuid)) { target = service; break; }
    }
    if (!target) { FinishGatt("service_unavailable", false); return; }
    while (gatt_request->next_slot < gatt_request->slots) {
      const unsigned slot = gatt_request->next_slot++;
      // bleutils::GenerateAdvertisementUuid(slot), slots are zero-based.
      const device::BluetoothUUID uuid(base::StringPrintf("00000000-0000-3000-8000-%012x", slot));
      const auto characteristics = target->GetCharacteristicsByUUID(uuid);
      if (characteristics.empty()) {
        ++gatt_request->read_failures;
        LOG(INFO) << "crQuickShare: BLE GATT slot unavailable; request=" << token
                  << "; slot=" << slot << "; reason=characteristic_missing";
        continue;
      }
      gatt_request->reading = true;
      GattStage("read_characteristic", device);
      LOG(INFO) << "crQuickShare: BLE GATT read requested; request=" << token
                << "; address=" << gatt_request->address << "; slot=" << slot
                << "; characteristic=" << characteristics.front()->GetIdentifier()
                << "; characteristic_uuid=" << characteristics.front()->GetUUID().canonical_value();
      characteristics.front()->ReadRemoteCharacteristic(
          base::BindOnce(&Impl::OnGattRead, weak.GetWeakPtr(), token));
      return;
    }
    FinishGatt(gatt_request->read_failures ? "partial" : "complete", true);
  }
  void OnGattRead(uint64_t token, std::optional<device::BluetoothGattService::GattErrorCode> error,
                  const std::vector<uint8_t>& value) {
    if (!gatt_request || gatt_request->token != token || !wanted) return;
    LOG(INFO) << "crQuickShare: BLE GATT read callback; request=" << token
              << "; address=" << gatt_request->address << "; slot=" << (gatt_request->next_slot - 1)
              << "; elapsed_ms=" << (base::TimeTicks::Now() - gatt_request->stage_started).InMilliseconds()
              << "; bytes=" << value.size() << "; error=" << (error ? static_cast<int>(*error) : -1);
    gatt_request->reading = false; ++gatt_reads;
    GattStage("process_read", adapter ? adapter->GetDevice(gatt_request->address) : nullptr);
    if (base::TimeTicks::Now() >= gatt_request->deadline) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
          base::BindOnce(&Impl::ReadGattSlot, weak.GetWeakPtr(), token));
      return;
    }
    const char* reason = error ? "read_failed" : value.empty() ? "empty_slot" : "gatt_requires_regular_advertisement";
    const unsigned slot = gatt_request->next_slot - 1;
    if (error) ++gatt_request->read_failures;
    else if (value.empty()) gatt_request->results.emplace(slot, std::nullopt);
    std::optional<Peer> peer;
    AdvertisementDetails details;
    // GATT slots may belong to other Nearby services. Require the regular
    // layout with explicit service hashes; no UUID-only identification.
    if (!error && !value.empty() && !(value.front() & 2))
      peer = ParseAdvertisement(base::as_string_view(base::span(value)), reason, &details);
    if (!error && !peer && std::string_view(reason) == "medium_payload_length" &&
        details.declared_payload <= 504 && details.declared_payload > details.available_payload &&
        gatt_request->short_read_retried.insert(slot).second) {
      LOG(INFO) << "crQuickShare: BLE GATT short value retry; request=" << token << "; slot=" << slot
                << "; bytes=" << value.size() << "; declared_payload=" << details.declared_payload
                << "; available_payload=" << details.available_payload
                << "; read_api=bluez_read_value; retry=whole_value; append=0";
      // Chromium's BlueZ API reads with an empty options dictionary. It does
      // not expose an offset here. Retry the entire value once; never combine
      // different snapshots or accept a prefix as a complete advertisement.
      --gatt_request->next_slot;
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(FROM_HERE,
          base::BindOnce(&Impl::ReadGattSlot, weak.GetWeakPtr(), token), base::Milliseconds(200));
      return;
    }
    if (peer) {
      const auto hash = AdvertisementHash(base::as_string_view(base::span(value)));
      if (auto lost = lost_hashes.find(hash); lost != lost_hashes.end() && lost->second > base::TimeTicks::Now()) {
        ++lost_suppressed;
        RememberLost(lost_headers, gatt_request->header, base::TimeTicks::Now());
        gatt_entries.erase(gatt_request->address);
        // ReadGattSlot observes the revoked header after the native callback.
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
            base::BindOnce(&Impl::ReadGattSlot, weak.GetWeakPtr(), token));
        return;
      }
      gatt_request->advertisement_hashes.insert(hash);
      if (!peer->name.empty()) ++named_updates;
      gatt_request->results.insert_or_assign(slot, Record{*peer, base::TimeTicks::Now(), hash});
      LOG(INFO) << "crQuickShare: BLE GATT endpoint resolved; request=" << token
                << "; address=" << gatt_request->address << "; endpoint_id=" << base::HexEncode(peer->id)
                << "; name_present=" << !peer->name.empty();
    } else {
      // A nonempty invalid read is not evidence that the old slot disappeared.
      // Preserve its original expiry and retry with the partial-refresh delay.
      if (!error && !value.empty()) ++gatt_request->read_failures;
      DiagnoseParse("gatt", reason, value.size());
      LOG(INFO) << "crQuickShare: BLE GATT slot skipped; request=" << token
                << "; slot=" << slot
                << "; bytes=" << value.size() << "; parse_stage=" << reason
                << "; declared_payload=" << details.declared_payload
                << "; available_payload=" << details.available_payload
                << "; medium_header=" << (value.empty() ? -1 : static_cast<int>(value.front()))
                << "; error=" << (error ? static_cast<int>(*error) : -1);
    }
    // Release native read callback frames before reading again or disconnecting.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&Impl::ReadGattSlot, weak.GetWeakPtr(), token));
  }
  void GattServicesDiscovered(device::BluetoothAdapter*, device::BluetoothDevice* device) override {
    if (gatt_request && gatt_request->address == device->GetAddress()) {
      if (!gatt_request->services_discovered)
        LOG(INFO) << "crQuickShare: BLE GATT services discovered; request=" << gatt_request->token
                  << "; address=" << device->GetAddress()
                  << "; elapsed_ms=" << (base::TimeTicks::Now() - gatt_request->started).InMilliseconds()
                  << "; gatt_connected=" << device->IsGattConnected()
                  << "; services_complete=" << device->IsGattServicesDiscoveryComplete();
      gatt_request->services_discovered = true;
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
          base::BindOnce(&Impl::ReadGattSlot, weak.GetWeakPtr(), gatt_request->token));
    }
  }
  void DeviceAdded(device::BluetoothAdapter*, device::BluetoothDevice* device) override { Observe(device); }
  void DeviceChanged(device::BluetoothAdapter*, device::BluetoothDevice* device) override { Observe(device); }
#if BUILDFLAG(IS_CHROMEOS) || BUILDFLAG(IS_LINUX)
  void DeviceAdvertisementReceived(device::BluetoothAdapter*, device::BluetoothDevice* device,
                                   int16_t, const std::vector<uint8_t>& eir) override {
    // Use service data from this event, including extended advertisements.
    // BlueZ's cached service data may refer to a different advertising set.
    auto fields = base::span(eir);
    while (!fields.empty()) {
      const size_t length = fields.front();
      fields = fields.subspan<1>();
      if (!length) return;
      if (length > fields.size()) { DiagnoseParse("eir", "truncated_ad_structure", eir.size()); return; }
      const auto field = fields.first(length);
      if (length >= 3 && field[0] == 0x16) {
        const auto bytes = base::as_string_view(field.subspan<3>());
        if (field[1] == 0xf3 && field[2] == 0xfe) Observe(device, true, bytes);
        else if (field[1] == 0x73 && field[2] == 0xfc) ObserveDct(device, bytes, true);
      }
      fields = fields.subspan(length);
    }
  }
#elif BUILDFLAG(IS_WIN)
  void DeviceAdvertisementReceived(const std::string& address,
      const std::optional<std::string>&, const std::optional<std::string>&,
      std::optional<int8_t>, std::optional<int8_t>, std::optional<uint16_t>,
      const device::BluetoothDevice::UUIDList&,
      const device::BluetoothDevice::ServiceDataMap& service_data,
      const device::BluetoothDevice::ManufacturerDataMap&) override {
    // WinRT creates/updates the device before delivering this parsed event.
    // Use this packet's data and mark it fresh even if bytes/RSSI are unchanged;
    // the following DeviceAdded/DeviceChanged notification is deduplicated.
    auto* peer = adapter ? adapter->GetDevice(address) : nullptr;
    if (!peer) return;
    if (const auto found = service_data.find(device::BluetoothUUID(kServiceUuid)); found != service_data.end())
      Observe(peer, true, base::as_string_view(base::span(found->second)));
    peer = adapter ? adapter->GetDevice(address) : nullptr;
    if (!peer) return;
    if (const auto found = service_data.find(device::BluetoothUUID(kDctServiceUuid)); found != service_data.end())
      ObserveDct(peer, base::as_string_view(base::span(found->second)), true);
  }
#endif
  void DeviceRemoved(device::BluetoothAdapter*, device::BluetoothDevice* device) override {
    const auto address = device->GetAddress(); records.erase(address); dct_records.erase(address); gatt_entries.erase(address); observations.erase(address); observations.erase("dct:" + address);
    if (gatt_request && gatt_request->address == address) FinishGatt("device_removed", false);
  }
  void DeviceAddressChanged(device::BluetoothAdapter*, device::BluetoothDevice* device, const std::string& old_address) override {
    records.erase(old_address); dct_records.erase(old_address); gatt_entries.erase(old_address); observations.erase(old_address); observations.erase("dct:" + old_address);
    if (gatt_request && gatt_request->address == old_address) FinishGatt("address_changed", false);
    Observe(device);
  }
  void AdapterPresentChanged(device::BluetoothAdapter*, bool) override { retry_after = {}; Reconcile(); }
  void AdapterPoweredChanged(device::BluetoothAdapter*, bool) override { retry_after = {}; Reconcile(); }
};
BleDiscovery::BleDiscovery() : impl_(std::make_unique<Impl>()) {}
BleDiscovery::~BleDiscovery() = default;
void BleDiscovery::Update(bool enabled, bool retain_cache) {
  const bool preserve = !enabled && retain_cache;
  if (impl_->retain_cache != preserve) {
    LOG(INFO) << "crQuickShare: BLE discovery cache retention; paused=" << preserve
              << "; advertisement_records=" << impl_->records.size()
              << "; gatt_entries=" << impl_->gatt_entries.size();
  }
  impl_->retain_cache = preserve;
  if (impl_->wanted != enabled) {
    LOG(INFO) << "crQuickShare: BLE discovery requested; enabled=" << enabled;
    impl_->wanted = enabled; impl_->retry_after = {};
    if (enabled) {
      impl_->connection_ready_callback.Reset();
      impl_->notifications = impl_->fef3 = impl_->dct = impl_->fast_initiation = 0;
      impl_->recognized = impl_->ignored = 0;
      impl_->on_lost = impl_->named_updates = impl_->large_fast = 0;
      impl_->gatt_attempts = impl_->gatt_reads = 0;
      impl_->gatt_expired_slots = impl_->gatt_partial_refreshes = 0;
      impl_->gatt_ready_skips = impl_->gatt_backoffs = 0;
      impl_->dct_recognized = impl_->extended_recognized = impl_->parsed_extensions = 0;
      impl_->parse_counts.clear();
      impl_->lost_removed = impl_->lost_suppressed = impl_->property_only = 0;
      impl_->next_ignored_log = {};
      impl_->next_association_log = {};
    }
  }
  if (!enabled) { impl_->bootstrap_waiting_for_gatt = false; impl_->bootstrap.Cancel(); }
  impl_->Reconcile();
}
BleDiscovery::State BleDiscovery::state() const { return impl_->current; }
scoped_refptr<device::BluetoothAdapter> BleDiscovery::adapter() const { return impl_->adapter; }
void BleDiscovery::WhenConnectionReady(base::OnceClosure callback) {
  impl_->connection_wait_started = base::TimeTicks::Now();
  impl_->connection_ready_callback = std::move(callback);
  if (impl_->gatt_request || impl_->gatt_connect_pending)
    LOG(INFO) << "crQuickShare: BLE send waiting for GATT release; connect_pending="
              << impl_->gatt_connect_pending;
  impl_->NotifyConnectionReady();
}
bool BleDiscovery::bootstrap_active() const { return impl_->bootstrap.active(); }
std::vector<Peer> BleDiscovery::MergeWithLan(std::vector<Peer> peers) const {
  struct Candidate {
    Peer peer;
    base::TimeTicks seen;
    const char* source;
    std::string system_name;
  };
  std::vector<Candidate> candidates;
  bool probe_candidate = false;
  const auto now = base::TimeTicks::Now();
  auto append = [&](const Peer& peer, const std::string& address,
                    base::TimeTicks seen, const char* source) {
    if (now - seen >= kLifetime) return;
    if (!impl_->discovery_started.is_null()) {
      const bool connectable = !EndpointName(peer.endpoint_info).empty() &&
          !peer.classic_bluetooth_address.empty();
      if (!impl_->first_candidate_logged || (connectable && !impl_->first_connectable_logged)) {
        LOG(INFO) << "crQuickShare: BLE discovery readiness; source=" << source
                  << "; elapsed_ms=" << (now - impl_->discovery_started).InMilliseconds()
                  << "; record_age_ms=" << (now - seen).InMilliseconds()
                  << "; retained_before_scan=" << (seen < impl_->discovery_started)
                  << "; public_name=" << !EndpointName(peer.endpoint_info).empty()
                  << "; classic_address_present=" << !peer.classic_bluetooth_address.empty()
                  << "; connectable=" << connectable;
        impl_->first_candidate_logged = true;
        impl_->first_connectable_logged |= connectable;
      }
    }
    candidates.push_back({peer, seen, source, SystemName(impl_->adapter.get(), address)});
    candidates.back().peer.bluetooth_address = address;
    if (impl_->bootstrap.CanProbe(peer)) {
      probe_candidate = true;
      if (impl_->gatt_request || impl_->gatt_connect_pending) {
        if (!impl_->bootstrap_waiting_for_gatt)
          LOG(INFO) << "crQuickShare: Bootstrap waiting for GATT; connect_pending=" << impl_->gatt_connect_pending;
        impl_->bootstrap_waiting_for_gatt = true;
      } else {
        impl_->bootstrap_waiting_for_gatt = false;
        impl_->bootstrap.Observe(impl_->adapter, peer);
      }
    }
  };
  for (const auto& [address, record] : impl_->records)
    append(record.peer, address, record.seen, "advertisement");
  for (const auto& [address, entry] : impl_->gatt_entries)
    for (const auto& [slot, record] : entry.slots)
      append(record.peer, address, std::min(entry.seen, record.seen), "gatt");
  if (!probe_candidate) impl_->bootstrap_waiting_for_gatt = false;
  // Retain the most recently observed address, instead of the first address in
  // map order. A protocol name from an older matching record can still help.
  std::ranges::stable_sort(candidates, [](const Candidate& a, const Candidate& b) {
    return a.seen > b.seen;
  });
  if (!candidates.empty() && now >= impl_->next_association_log) {
    impl_->next_association_log = now + base::Seconds(30);
    // Bound diagnostics on crowded networks. Never print names, salts, key
    // hashes or raw advertisements; equality flags suffice for correlation.
    const size_t limit = std::min(candidates.size(), size_t{16});
    LOG(INFO) << "crQuickShare: BLE association snapshot; records=" << candidates.size()
              << "; logged=" << limit << "; lan_peers=" << peers.size();
    for (size_t i = 0; i < std::min(peers.size(), size_t{16}); ++i) {
      const auto& lan = peers[i];
      LOG(INFO) << "crQuickShare: LAN association record; cache_id=" << lan.discovery_cache_id
                << "; interface=" << lan.discovery_interface << "; endpoint_id=" << base::HexEncode(lan.id)
                << "; lan_ipv4=" << lan.address << "; port=" << lan.port << "; named=" << !lan.name.empty();
    }
    for (size_t i = 0; i < limit; ++i) {
      const auto& item = candidates[i];
      const auto& peer = item.peer;
      LOG(INFO) << "crQuickShare: BLE association record; index=" << i
                << "; address=" << peer.bluetooth_address << "; source=" << item.source
                << "; endpoint_id=" << base::HexEncode(peer.id)
                << "; info_header=" << static_cast<unsigned>(static_cast<uint8_t>(peer.endpoint_info[0]))
                << "; name_source=" << (!peer.name.empty() ? "quick_share" :
                    !item.system_name.empty() ? "bluez" : "none")
                << "; classic_address_present=" << !peer.classic_bluetooth_address.empty()
                << "; age_ms=" << (now - item.seen).InMilliseconds()
                << "; lan_match=" << std::ranges::any_of(peers, [&](const Peer& lan) { return SameEndpoint(lan, peer); });
      size_t lan_logged = 0;
      for (const auto& lan : peers) {
        if (lan_logged >= 16) break;
        const bool same_id = lan.id == peer.id;
        const bool same_key = SameDiscoveryKey(lan, peer);
        if (!same_id && !same_key) continue;
        ++lan_logged;
        LOG(INFO) << "crQuickShare: BLE LAN association; ble_index=" << i
                  << "; cache_id=" << lan.discovery_cache_id << "; interface=" << lan.discovery_interface
                  << "; lan_ipv4=" << lan.address << "; port=" << lan.port
                  << "; same_endpoint_id=" << same_id << "; same_discovery_key=" << same_key
                  << "; same_header=" << (((uint8_t(lan.endpoint_info[0]) ^ uint8_t(peer.endpoint_info[0])) & 0xef) == 0)
                  << "; lan_named=" << !lan.name.empty() << "; ble_named=" << !EndpointName(peer.endpoint_info).empty()
                  << "; merge=" << SameEndpoint(lan, peer);
      }
      for (size_t j = 0; j < i; ++j) {
        const auto& other = candidates[j];
        const auto& name = peer.name.empty() ? item.system_name : peer.name;
        const auto& other_name = other.peer.name.empty() ? other.system_name : other.peer.name;
        const bool same_name = !name.empty() && name == other_name;
        const bool same_id = peer.id == other.peer.id;
        const bool same_key = SameDiscoveryKey(peer, other.peer);
        const bool same_endpoint = SameEndpoint(peer, other.peer);
        const bool same_route = SameBluetoothRoute(peer, other.peer);
        if (!same_name && !same_id && !same_key) continue;
        LOG(INFO) << "crQuickShare: BLE association pair; first=" << j << "; second=" << i
                  << "; same_display_name=" << same_name << "; same_endpoint_id=" << same_id
                  << "; same_discovery_key=" << same_key
                  << "; same_header=" << (peer.endpoint_info[0] == other.peer.endpoint_info[0])
                  << "; same_bluetooth_route=" << same_route
                  << "; merge=" << (same_endpoint || same_route)
                  << "; merge_reason=" << (same_endpoint ? "discovery_key" : same_route ? "bluetooth_route" : "distinct");
      }
    }
  }
  for (auto& lan : peers) {
    if (!lan.name.empty()) continue;
    for (const auto& item : candidates) {
      if (!SameEndpoint(lan, item.peer)) continue;
      const auto public_name = EndpointName(item.peer.endpoint_info);
      if (public_name.empty()) continue;
      lan.name = public_name;
      lan.public_name_info = item.peer.endpoint_info;
      lan.bluetooth_address = item.peer.bluetooth_address;
      break;
    }
  }
  std::vector<Peer> nearby;
  for (const auto& item : candidates) {
    const auto& candidate = item.peer;
    if (std::ranges::any_of(peers, [&](const Peer& peer) { return SameEndpoint(peer, candidate); })) continue;
    auto existing = std::ranges::find_if(nearby, [&](const Peer& peer) {
      return SameEndpoint(peer, candidate) || SameBluetoothRoute(peer, candidate);
    });
    if (existing == nearby.end()) nearby.push_back(candidate);
    else if (SameEndpoint(*existing, candidate)) {
      // Candidates are newest first. Across discovery-key rotations retain
      // the latest record intact; only supplement matching discovery data.
      if (existing->name.empty() && !candidate.name.empty()) {
        existing->name = candidate.name;
        existing->endpoint_info = candidate.endpoint_info;
      }
      if (existing->classic_bluetooth_address.empty())
        existing->classic_bluetooth_address = candidate.classic_bluetooth_address;
    }
  }
  // Apply BlueZ names after protocol-name merging; do not alter cached
  // identities, TTLs, or diagnostics about names carried in advertisements.
  for (auto& peer : nearby) {
    if (peer.name.empty()) peer.name = SystemName(impl_->adapter.get(), peer.bluetooth_address);
  }
  std::ranges::sort(nearby, [](const Peer& a, const Peer& b) {
    return std::tie(a.id, a.endpoint_info) < std::tie(b.id, b.endpoint_info);
  });
  // Anonymous LAN candidates remain in the service snapshot for association,
  // but never appear as selectable devices without a matching public name.
  std::erase_if(peers, [](const Peer& peer) { return peer.name.empty(); });
  peers.insert(peers.end(), nearby.begin(), nearby.end());
  // DCT cannot establish SameEndpoint with FEF3/LAN. It deliberately bypasses
  // that identity merge, and carries no fabricated Sharing endpoint_info.
  for (const auto& [address, record] : impl_->dct_records) {
    if (now - record.seen < kLifetime) peers.push_back(record.peer);
  }
  return peers;
}
}  // namespace crquickshare
