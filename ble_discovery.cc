// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/ble_discovery.h"
#include "crQuickShare/advertisement_codec.h"
#include "crQuickShare/bootstrap_probe.h"
#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include "base/containers/span.h"
#include "base/strings/stringprintf.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_view_util.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_MAC)
#include "base/command_line.h"
#include "crQuickShare/bluetooth_connector_mac.h"
#include "crQuickShare/mac_fast_init_experiment.h"
#endif
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
constexpr auto kLifetime = base::Seconds(120);
constexpr auto kGattCandidateFreshness = base::Seconds(6);
constexpr size_t kMaxRecords = 128;
constexpr size_t kMaxGattServicesToLog = 32;
constexpr size_t kMaxGattCharacteristicsToLog = 64;

device::BluetoothUUID GattSlotUuid(unsigned slot) {
  // bleutils::GenerateAdvertisementUuid(slot), slots are zero-based.
  return device::BluetoothUUID(base::StringPrintf("00000000-0000-3000-8000-%012x", slot));
}
bool GattReadable(const device::BluetoothRemoteGattCharacteristic* characteristic) {
  return characteristic->GetProperties() & device::BluetoothGattCharacteristic::PROPERTY_READ;
}

bool SameEndpoint(const Peer& a, const Peer& b) {
  return a.id == b.id && a.endpoint_info == b.endpoint_info;
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
  // Snapshot inputs are polled repeatedly. Remember each source's last info
  // so unchanged old records cannot replace a newer endpoint generation.
  std::map<std::string, std::string> discovery_source_info;
  std::map<std::string, std::string> endpoint_info_by_id;
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
    base::TimeTicks last_advertisement = {};
    uint64_t last_token = 0, failed_token = 0;
  };
  struct GattRequest {
    uint64_t token;
    std::string address, header;
    std::string service_id;
    unsigned slots = 0, next_slot = 0;
    bool reading = false, services_discovered = false;
    bool inventory_logged = false;
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
#if BUILDFLAG(IS_MAC)
  // A shared attempt cannot be cancelled by this caller. Keep at most one
  // abandoned waiter per address until its callback or device removal.
  std::map<std::string, uint64_t> abandoned_gatt_connects;
#endif
  base::OnceClosure connection_ready_callback;
  base::TimeTicks connection_wait_started;
  // Only observe links used by our probes; never forcibly disconnect a shared
  // or paired device just to make the classic transport ready.
  std::map<std::string, base::TimeTicks> released_gatt;
  base::OneShotTimer release_poll;
  uint64_t pending_gatt_token = 0;
  std::string pending_gatt_address;
  base::TimeTicks pending_gatt_started, next_gatt_blocked_log;
  base::TimeTicks next_gatt_start;
  State current = State::kIdle;
  bool wanted = false, initializing = false, pending = false;
  bool retain_cache = false;
  base::TimeTicks retry_after, next_ignored_log, next_summary;
  uint64_t ignored = 0, notifications = 0, fef3 = 0, dct = 0, fast_initiation = 0, recognized = 0;
  uint64_t on_lost = 0, named_updates = 0, large_fast = 0;
  base::WeakPtrFactory<Impl> weak{this};
  Impl() {
#if !BUILDFLAG(IS_MAC)
    InitializeAdapter();
#endif
  }
  void InitializeAdapter() {
    if (!device::BluetoothAdapterFactory::IsBluetoothSupported()) return;
    initializing = true;
    device::BluetoothAdapterFactory::Get()->GetAdapter(base::BindOnce(&Impl::OnAdapter, weak.GetWeakPtr()));
  }
  ~Impl() override {
    connection_ready_callback.Reset();
    release_poll.Stop();
    weak.InvalidateWeakPtrs();
    if (adapter) adapter->RemoveObserver(this);
#if BUILDFLAG(IS_MAC)
    FinishGatt("destroyed", false);
#else
    gatt_request.reset();
#endif
    session.reset();
  }
  void OnAdapter(scoped_refptr<device::BluetoothAdapter> value) {
#if BUILDFLAG(IS_MAC)
    adapter = std::move(value);
    adapter->AddObserver(this);
    // The classic API can block on an unresolved permission prompt. Resolve
    // permission asynchronously before inspecting or connecting native devices.
    adapter->RequestSystemPermission(base::BindOnce(&Impl::OnSystemPermission, weak.GetWeakPtr()));
#else
    initializing = false; adapter = std::move(value);
    LOG(INFO) << "crQuickShare: BLE discovery adapter ready; present=" << adapter->IsPresent()
              << "; powered=" << adapter->IsPowered();
    adapter->AddObserver(this); Reconcile();
#endif
  }
#if BUILDFLAG(IS_MAC)
  void OnSystemPermission(device::BluetoothAdapter::PermissionStatus status) {
    initializing = false;
    LOG(INFO) << "crQuickShare: BLE system permission resolved; allowed="
              << (status == device::BluetoothAdapter::PermissionStatus::kAllowed);
    Reconcile();
  }
#endif
  void Reconcile() {
    bool permission_ready = true;
#if BUILDFLAG(IS_MAC)
    // Creating the native central manager can show the Bluetooth permission
    // prompt. Wait until the user opens device discovery before doing that.
    if (wanted && !adapter && !initializing) InitializeAdapter();
    if (initializing) { current = wanted ? State::kStarting : State::kIdle; return; }
    permission_ready = !adapter ||
        adapter->GetOsPermissionStatus() == device::BluetoothAdapter::PermissionStatus::kAllowed;
#endif
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
    if (!wanted || !adapter || !permission_ready || !adapter->IsPresent() || !adapter->IsPowered()) {
      FinishGatt(retain_cache ? "paused" : "stopped", false);
      session.reset(); starting_devices.clear(); observations.clear();
      if (!retain_cache || !adapter || !permission_ready || !adapter->IsPresent() || !adapter->IsPowered()) {
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
#if BUILDFLAG(IS_MAC)
    LOG(INFO) << "crQuickShare: macOS discovery comparison; experiment_enabled="
              << IsMacFastInitExperimentEnabled()
              << "; scan_start_time_ticks_us=" << discovery_started.since_origin().InMicroseconds()
              << "; cached_advertisements=" << records.size() << "; cached_gatt_headers=" << gatt_entries.size()
              << "; timing_is_not_proof_of_peer_response=1";
#endif
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
    LOG(WARNING) << "crQuickShare: BLE discovery unavailable; retry_delay_s=15"
                 << "; adapter_present=" << (adapter && adapter->IsPresent())
                 << "; adapter_powered=" << (adapter && adapter->IsPowered());
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
        entry = gatt_entries.emplace(address, GattEntry{std::move(*header), now, {}, {}, {}}).first;
        LOG(INFO) << "crQuickShare: BLE GATT header discovered; scan_elapsed_ms="
                  << (discovery_started.is_null() ? -1 : (now - discovery_started).InMilliseconds())
                  << "; observation_time_ticks_us=" << now.since_origin().InMicroseconds();
      } else {
        entry->second.seen = now;
        if (entry->second.header.bytes != header->bytes) {
          // Never carry identities across changed headers. Refresh sooner than
          // the normal retry, but rate-limit header rotation per address.
          const size_t cleared_slots = entry->second.slots.size();
          entry->second.header = std::move(*header); entry->second.slots.clear();
          entry->second.resolved = {}; entry->second.failures = 0; entry->second.skip_logged = false;
          entry->second.last_token = 0; entry->second.failed_token = 0;
          entry->second.last_advertisement = {};
          entry->second.retry = std::max(now, entry->second.last_attempt + base::Seconds(5));
          LOG(INFO) << "crQuickShare: BLE GATT header changed; cached_slots_cleared=" << cleared_slots << "; refresh_delay_ms="
                    << (entry->second.retry - now).InMilliseconds();
          if (gatt_request && gatt_request->address == address) FinishGatt("header_changed", false);
        }
      }
      // Property notifications can contain cached service data. Only a received
      // advertisement authorizes a new connection or a retry.
      if (advertisement_event) entry->second.last_advertisement = now;
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
      LOG(INFO) << "crQuickShare: BLE endpoint discovered; name_present=" << !peer->name.empty()
                << "; endpoint_id=" << base::HexEncode(peer->id)
                << "; classic_address_present=" << !peer->classic_bluetooth_address.empty()
                << "; scan_elapsed_ms=" << (discovery_started.is_null() ? -1 : (now - discovery_started).InMilliseconds())
                << "; observation_time_ticks_us=" << now.since_origin().InMicroseconds();
    }
    const auto existing = records.find(address);
    if (existing != records.end() && !peer->name.empty() &&
        (existing->second.peer.name.empty() || (existing->second.peer.classic_bluetooth_address.empty() &&
                                             !peer->classic_bluetooth_address.empty())))
      LOG(INFO) << "crQuickShare: BLE endpoint updated; name_present=1; endpoint_id=" << base::HexEncode(peer->id)
                << "; classic_address_present=" << !peer->classic_bluetooth_address.empty()
                << "; scan_elapsed_ms=" << (discovery_started.is_null() ? -1 : (now - discovery_started).InMilliseconds())
                << "; observation_time_ticks_us=" << now.since_origin().InMicroseconds();
    records.insert_or_assign(address, Record{std::move(*peer), now, advertisement_hash});
  }
  void ReleaseGatt(const std::string& address,
                   std::unique_ptr<device::BluetoothGattConnection> connection) {
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)
    auto* device = adapter ? adapter->GetDevice(address) : nullptr;
#if BUILDFLAG(IS_MAC)
    const bool connected = device && device->IsGattConnected();
#else
    const bool connected = device && device->IsConnected();
#endif
    if (connection || connected) {
      const auto now = base::TimeTicks::Now();
      std::erase_if(released_gatt, [&](const auto& entry) {
        return now - entry.second >= base::Seconds(2);
      });
      released_gatt.insert_or_assign(address, now);
      LOG(INFO) << "crQuickShare: BLE GATT reference release requested; address=" << address
                << "; reference_present=" << bool(connection)
                << "; device_connected=" << connected
                << "; release_time_ticks_us=" << now.since_origin().InMicroseconds();
    }
#endif
    connection.reset();
  }
#if BUILDFLAG(IS_MAC)
  void ReleaseLateGatt(const std::string& address,
                       std::unique_ptr<device::BluetoothGattConnection> connection) {
    ReleaseGatt(address, std::move(connection));
    NotifyConnectionReady();
  }
  void CancelPendingGattConnect(uint64_t token, const std::string& address, const char* reason) {
    if (!gatt_connect_pending || pending_gatt_token != token) return;
    // Retire all local state before cancellation, whose callback can run
    // synchronously. A late result must not clear a newer request's state.
    abandoned_gatt_connects.insert_or_assign(address, token);
    gatt_connect_pending = false;
    pending_gatt_token = 0;
    pending_gatt_address.clear();
    released_gatt.insert_or_assign(address, base::TimeTicks::Now());
    LOG(INFO) << "crQuickShare: BLE GATT connect cancellation requested; request=" << token
              << "; address=" << address << "; reason=" << reason;
    // DeviceRemoved can be dispatched while the adapter still exposes the
    // object. Its destructor cancels native work; do not reenter its teardown.
    const bool removed = std::string_view(reason) == "device_removed";
    const bool present = adapter && adapter->GetDevice(address);
    const bool cancelled = !removed && present && CancelMacPendingGattConnection(adapter, address);
    if (removed || !present) abandoned_gatt_connects.erase(address);
    LOG(INFO) << "crQuickShare: BLE GATT scheduler released; request=" << token
              << "; address=" << address << "; native_cancel_requested=" << cancelled
              << "; waiting_for_shared_callback=" << abandoned_gatt_connects.contains(address);
  }
#endif
  void NotifyConnectionReady() {
    if (wanted || gatt_request || gatt_connect_pending || !connection_ready_callback) return;
    const auto now = base::TimeTicks::Now();
    auto next_check = base::Seconds(2);
    bool waiting = false;
    for (auto it = released_gatt.begin(); it != released_gatt.end();) {
      auto* device = adapter ? adapter->GetDevice(it->first) : nullptr;
#if BUILDFLAG(IS_MAC)
      const bool connected = device && device->IsGattConnected();
#else
      const bool connected = device && device->IsConnected();
#endif
      const auto elapsed = now - it->second;
      // Linux BlueZ exposes one Connected property for LE and classic links.
      // This is a bounded settling heuristic, not proof of LE disconnection.
      // DeviceChanged triggers early checks; the timer covers settling and the
      // deadline even when paired/shared links remain connected indefinitely.
#if BUILDFLAG(IS_MAC)
      const bool connecting = device && device->IsConnecting();
#else
      const bool connecting = false;
#endif
      const bool settling = connected || connecting;
      if (elapsed < base::Milliseconds(200) || (settling && elapsed < base::Seconds(2))) {
        waiting = true;
        const auto remaining = elapsed < base::Milliseconds(200) ?
            base::Milliseconds(200) - elapsed : base::Seconds(2) - elapsed;
        next_check = std::min(next_check, remaining);
        ++it;
      } else {
        LOG(INFO) << "crQuickShare: BLE GATT release wait finished; address=" << it->first
                  << "; device_connected=" << connected
                  << "; device_connecting=" << connecting
                  << "; reason=" << (!device ? "device_unavailable" :
                      settling ? "wait_limit_reached" : "device_disconnected_after_settle")
                  << "; release_time_ticks_us=" << it->second.since_origin().InMicroseconds()
                  << "; elapsed_ms=" << elapsed.InMilliseconds();
        it = released_gatt.erase(it);
      }
    }
    if (waiting) {
      release_poll.Start(FROM_HERE, next_check,
          base::BindOnce(&Impl::NotifyConnectionReady, weak.GetWeakPtr()));
      return;
    }
    release_poll.Stop();
    LOG(INFO) << "crQuickShare: BLE send connection ready; gatt_wait_ms="
              << (now - connection_wait_started).InMilliseconds();
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, std::move(connection_ready_callback));
  }
  void DeferGattRetry(GattEntry& entry, uint64_t token, const char* outcome) {
    if (entry.failed_token != token) {
      entry.failures = std::min(entry.failures + 1, 3u);
      entry.failed_token = token;
    }
    const unsigned delay = 30u << (entry.failures - 1);
    entry.retry = std::max(entry.retry, base::TimeTicks::Now() + base::Seconds(delay));
    ++gatt_backoffs;
    LOG(INFO) << "crQuickShare: BLE GATT retry deferred; request=" << token
              << "; failures=" << entry.failures << "; retry_ms=" << delay * 1000
              << "; reason=" << outcome;
  }
  void FinishGatt(const char* outcome, bool commit, bool allow_partial = false) {
    if (!gatt_request) return;
    if (!gatt_request->inventory_logged && gatt_request->connection)
      LogGattInventory(adapter ? adapter->GetDevice(gatt_request->address) : nullptr, outcome);
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
          std::string_view(outcome) == "service_ambiguous" ||
          std::string_view(outcome) == "service_unavailable";
      if (complete) {
        entry->second.failures = 0;
        entry->second.resolved = done.started;
        entry->second.retry = now + base::Seconds(60);
      } else if (failed) {
        DeferGattRetry(entry->second, done.token, outcome);
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
              << "; service=" << done.service_id
              << "; outcome=" << outcome << "; slots_visited=" << done.next_slot
              << "; read_failures=" << done.read_failures << "; slots_updated=" << updated << "; slots_cleared=" << cleared;
#if BUILDFLAG(IS_MAC)
    if (!done.connection) CancelPendingGattConnect(done.token, done.address, outcome);
#endif
    ReleaseGatt(done.address, std::move(done.connection));
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
    // Backends without request cancellation must wait for the native callback.
    // On macOS, FinishGatt retires the waiter and cancels a sole native attempt.
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
#if BUILDFLAG(IS_MAC)
    if (abandoned_gatt_connects.size() >= kMaxRecords) {
      if (now >= next_gatt_blocked_log) {
        LOG(INFO) << "crQuickShare: BLE GATT scheduler blocked; reason=shared_connect_limit"
                  << "; pending_shared_connects=" << abandoned_gatt_connects.size();
        next_gatt_blocked_log = now + base::Seconds(5);
      }
      return;
    }
#endif
    auto candidate = gatt_entries.end();
    int selected_priority = 3;
    for (auto it = gatt_entries.begin(); it != gatt_entries.end(); ++it) {
      if (now < it->second.retry || !adapter->GetDevice(it->first)) continue;
#if BUILDFLAG(IS_MAC)
      if (abandoned_gatt_connects.contains(it->first)) continue;
#endif
      auto& entry = it->second;
      if (entry.last_advertisement.is_null() ||
          now - entry.last_advertisement >= kGattCandidateFreshness ||
          entry.last_advertisement <= entry.last_attempt) continue;
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
      entry.last_token = gatt_request->token;
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
    bool abandoned = false;
#if BUILDFLAG(IS_MAC)
    auto old = abandoned_gatt_connects.find(address);
    if (old != abandoned_gatt_connects.end() && old->second == token) {
      abandoned = true;
      abandoned_gatt_connects.erase(old);
    }
#endif
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
              << "; abandoned=" << abandoned
              << "; error=" << (error ? static_cast<int>(*error) : -1)
              << "; connection_present=" << bool(connection)
              << "; connection_connected=" << (connection && connection->IsConnected())
              << "; device_present=" << bool(device)
              << "; gatt_connected=" << (device && device->IsGattConnected())
              << "; services_complete=" << (device && device->IsGattServicesDiscoveryComplete());
    if (!gatt_request || gatt_request->token != token || !wanted) {
      // Explicit cancellation is not a second device failure. Other late
      // failures preserve backoff without touching a newer header or request.
      auto entry = gatt_entries.find(address);
      if (!abandoned && entry != gatt_entries.end() && entry->second.last_token == token &&
          (error || !connection || !connection->IsConnected())) {
        DeferGattRetry(entry->second, token, "late_connect_failed");
      }
#if BUILDFLAG(IS_MAC)
      if (connection) {
        // BluetoothDevice fans out shared callbacks synchronously. Let other
        // waiters acquire their references before dropping this stale one.
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
            base::BindOnce(&Impl::ReleaseLateGatt, weak.GetWeakPtr(), address, std::move(connection)));
        return;
      }
#endif
      ReleaseGatt(address, std::move(connection));
      NotifyConnectionReady();
      return;
    }
    if (error || !connection || !connection->IsConnected()) { FinishGatt("connect_failed", false); return; }
    gatt_request->connection = std::move(connection);
    GattStage("wait_services", device);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&Impl::ReadGattSlot, weak.GetWeakPtr(), token));
  }
  void LogGattInventory(device::BluetoothDevice* device, const char* reason) {
    if (!gatt_request || gatt_request->inventory_logged) return;
    gatt_request->inventory_logged = true;
    auto services = device ? device->GetGattServices() : std::vector<device::BluetoothRemoteGattService*>();
    const device::BluetoothUUID service_uuid(kServiceUuid);
    // Log matching services first so unrelated services cannot use up the cap.
    const auto matches = std::stable_partition(services.begin(), services.end(),
        [&](auto* service) { return service->GetUUID() == service_uuid; });
    const auto matching_services = std::distance(services.begin(), matches);
    LOG(INFO) << "crQuickShare: BLE GATT inventory; request=" << gatt_request->token
              << "; address=" << gatt_request->address << "; reason=" << reason
              << "; services_complete=" << (device && device->IsGattServicesDiscoveryComplete())
              << "; service_count=" << services.size() << "; matching_services=" << matching_services
              << "; expected_slots=" << gatt_request->slots
              << "; service_log_limit=" << kMaxGattServicesToLog
              << "; characteristic_log_limit=" << kMaxGattCharacteristicsToLog;
    size_t services_logged = 0, characteristics_logged = 0, matching_characteristics = 0;
    for (auto* service : services) {
      const bool matching = service->GetUUID() == service_uuid;
      const auto characteristics = service->GetCharacteristics();
      if (matching) matching_characteristics += characteristics.size();
      if (services_logged >= kMaxGattServicesToLog) continue;
      ++services_logged;
      LOG(INFO) << "crQuickShare: BLE GATT service inventory; request=" << gatt_request->token
                << "; service=" << service->GetIdentifier()
                << "; uuid=" << service->GetUUID().canonical_value()
                << "; primary=" << service->IsPrimary() << "; matching=" << matching
                << "; discovery_complete=" << service->IsDiscoveryComplete()
                << "; characteristic_count=" << characteristics.size();
      if (!matching) continue;
      for (auto* characteristic : characteristics) {
        if (characteristics_logged >= kMaxGattCharacteristicsToLog) break;
        ++characteristics_logged;
        int expected_slot = -1;
        for (unsigned slot = 0; slot < gatt_request->slots; ++slot) {
          if (characteristic->GetUUID() == GattSlotUuid(slot)) {
            expected_slot = static_cast<int>(slot);
            break;
          }
        }
        LOG(INFO) << "crQuickShare: BLE GATT characteristic inventory; request=" << gatt_request->token
                  << "; service=" << service->GetIdentifier()
                  << "; characteristic=" << characteristic->GetIdentifier()
                  << "; uuid=" << characteristic->GetUUID().canonical_value()
                  << "; properties=" << characteristic->GetProperties()
                  << "; readable=" << GattReadable(characteristic)
                  << "; expected_slot=" << expected_slot;
      }
    }
    LOG(INFO) << "crQuickShare: BLE GATT inventory finished; request=" << gatt_request->token
              << "; services_logged=" << services_logged
              << "; services_omitted=" << (services.size() - services_logged)
              << "; matching_characteristics=" << matching_characteristics
              << "; characteristics_logged=" << characteristics_logged
              << "; characteristics_omitted=" << (matching_characteristics - characteristics_logged);
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
#if BUILDFLAG(IS_MAC)
    // Service invalidation can restart discovery after the first notification.
    if (!device->IsGattServicesDiscoveryComplete()) return;
#endif
    GattStage("find_service", device);
    if (gatt_request->service_id.empty()) {
      LogGattInventory(device, "services_ready");
      std::vector<device::BluetoothRemoteGattService*> matching, readable;
      for (auto* service : device->GetGattServices()) {
        if (service->GetUUID() != device::BluetoothUUID(kServiceUuid)) continue;
        matching.push_back(service);
        for (unsigned slot = 0; slot < gatt_request->slots; ++slot) {
          if (std::ranges::any_of(service->GetCharacteristicsByUUID(GattSlotUuid(slot)), GattReadable)) {
            readable.push_back(service);
            break;
          }
        }
      }
      // A UUID identifies a type, not an instance. Do not pick the first
      // service or combine slots from different instances of the service.
      auto* selected = readable.size() == 1 ? readable.front() :
          matching.size() == 1 ? matching.front() : nullptr;
      const char* reason = readable.size() > 1 ? "service_ambiguous" :
          !selected ? "service_unavailable" : readable.empty() ? "no_readable_slots" : "unique_readable_service";
      LOG(INFO) << "crQuickShare: BLE GATT service selection; request=" << token
                << "; address=" << gatt_request->address << "; matching_services=" << matching.size()
                << "; readable_services=" << readable.size() << "; reason=" << reason
                << "; selected_service=" << (selected ? selected->GetIdentifier() : std::string());
      if (!selected) { FinishGatt(reason, false); return; }
      gatt_request->service_id = selected->GetIdentifier();
    }
    // Resolve the identifier again after each asynchronous read; invalidation
    // may destroy the native service and its characteristic wrappers.
    auto* target = device->GetGattService(gatt_request->service_id);
    if (!target) { FinishGatt("service_unavailable", false); return; }
    while (gatt_request->next_slot < gatt_request->slots) {
      const unsigned slot = gatt_request->next_slot++;
      const auto uuid = GattSlotUuid(slot);
      const auto characteristics = target->GetCharacteristicsByUUID(uuid);
      device::BluetoothRemoteGattCharacteristic* characteristic = nullptr;
      size_t readable_count = 0;
      for (auto* candidate : characteristics) {
        if (!GattReadable(candidate)) continue;
        characteristic = candidate;
        ++readable_count;
      }
      if (readable_count != 1) {
        ++gatt_request->read_failures;
        LOG(INFO) << "crQuickShare: BLE GATT slot unavailable; request=" << token
                  << "; address=" << gatt_request->address << "; slot=" << slot
                  << "; service=" << target->GetIdentifier() << "; expected_uuid=" << uuid.canonical_value()
                  << "; characteristic_count=" << characteristics.size() << "; readable_count=" << readable_count
                  << "; reason=" << (characteristics.empty() ? "characteristic_missing" :
                      !readable_count ? "characteristic_not_readable" : "characteristic_ambiguous");
        continue;
      }
      gatt_request->reading = true;
      GattStage("read_characteristic", device);
      LOG(INFO) << "crQuickShare: BLE GATT read requested; request=" << token
                << "; address=" << gatt_request->address << "; slot=" << slot
                << "; service=" << target->GetIdentifier()
                << "; characteristic=" << characteristic->GetIdentifier()
                << "; characteristic_uuid=" << characteristic->GetUUID().canonical_value();
      characteristic->ReadRemoteCharacteristic(
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
                << "; name_present=" << !peer->name.empty()
                << "; classic_address_present=" << !peer->classic_bluetooth_address.empty()
                << "; scan_elapsed_ms=" << (discovery_started.is_null() ? -1 :
                    (base::TimeTicks::Now() - discovery_started).InMilliseconds())
                << "; observation_time_ticks_us=" << base::TimeTicks::Now().since_origin().InMicroseconds();
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
  void DeviceAdded(device::BluetoothAdapter*, device::BluetoothDevice* device) override {
#if !BUILDFLAG(IS_MAC)
    Observe(device);
#endif
  }
  void DeviceChanged(device::BluetoothAdapter*, device::BluetoothDevice* device) override {
    if (connection_ready_callback && released_gatt.contains(device->GetAddress()))
      NotifyConnectionReady();
#if !BUILDFLAG(IS_MAC)
    Observe(device);
#endif
  }
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
#elif BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  void DeviceAdvertisementReceived(const std::string& address,
      const std::optional<std::string>&, const std::optional<std::string>&,
      std::optional<int8_t>, std::optional<int8_t>, std::optional<uint16_t>,
      const device::BluetoothDevice::UUIDList&,
      const device::BluetoothDevice::ServiceDataMap& service_data,
      const device::BluetoothDevice::ManufacturerDataMap&) override {
#if BUILDFLAG(IS_MAC)
    if (!wanted) return;
    // CoreBluetooth delivers this event before inserting/updating the device.
    // Copy only bounded Nearby data and resolve the device on the next UI task.
    device::BluetoothDevice::ServiceDataMap nearby_data;
    for (const auto& [uuid, data] : service_data) {
      if (uuid != device::BluetoothUUID(kServiceUuid) && uuid != device::BluetoothUUID(kDctServiceUuid)) continue;
      if (data.size() > 512) { DiagnoseParse("service_data", "medium_size", data.size()); continue; }
      nearby_data.emplace(uuid, data);
    }
    if (!nearby_data.empty()) base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&Impl::ObserveParsedAdvertisement, weak.GetWeakPtr(), address, std::move(nearby_data)));
#else
    // WinRT creates/updates the device before delivering this parsed event.
    // Use this packet's data and mark it fresh even if bytes/RSSI are unchanged;
    // the following DeviceAdded/DeviceChanged notification is deduplicated.
    ObserveParsedAdvertisement(address, service_data);
#endif
  }
  void ObserveParsedAdvertisement(const std::string& address,
                                   const device::BluetoothDevice::ServiceDataMap& service_data) {
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
#if BUILDFLAG(IS_MAC)
    abandoned_gatt_connects.erase(address);
#endif
    // DeviceRemoved may run before the adapter removes its device object.
    if (connection_ready_callback && released_gatt.contains(address))
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
          base::BindOnce(&Impl::NotifyConnectionReady, weak.GetWeakPtr()));
  }
  void DeviceAddressChanged(device::BluetoothAdapter*, device::BluetoothDevice* device, const std::string& old_address) override {
    records.erase(old_address); dct_records.erase(old_address); gatt_entries.erase(old_address); observations.erase(old_address); observations.erase("dct:" + old_address);
    if (gatt_request && gatt_request->address == old_address) FinishGatt("address_changed", false);
    auto released = released_gatt.extract(old_address);
    if (!released.empty()) {
      released.key() = device->GetAddress();
      released_gatt.insert(std::move(released));
      NotifyConnectionReady();
    }
    Observe(device);
  }
  void AdapterPresentChanged(device::BluetoothAdapter*, bool) override { retry_after = {}; Reconcile(); }
  void AdapterPoweredChanged(device::BluetoothAdapter*, bool) override { retry_after = {}; Reconcile(); }
};
BleDiscovery::BleDiscovery() : impl_(std::make_unique<Impl>()) {}
BleDiscovery::~BleDiscovery() = default;
void BleDiscovery::Update(bool enabled, bool retain_cache) {
  if (!enabled && !retain_cache) {
    impl_->discovery_source_info.clear();
    impl_->endpoint_info_by_id.clear();
  }
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
      impl_->release_poll.Stop();
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
    std::string source_key;
    std::string system_name;
  };
  std::vector<Candidate> candidates;
  bool probe_candidate = false;
  const auto now = base::TimeTicks::Now();
  auto append = [&](const Peer& peer, const std::string& address,
                    base::TimeTicks seen, const char* source, unsigned slot = 0) {
    if (now - seen >= kLifetime) return;
    if (!impl_->discovery_started.is_null()) {
      const bool connectable = !EndpointName(peer.endpoint_info).empty() &&
          !peer.classic_bluetooth_address.empty();
      if (!impl_->first_candidate_logged || (connectable && !impl_->first_connectable_logged)) {
        LOG(INFO) << "crQuickShare: BLE discovery readiness; source=" << source
                  << "; endpoint_id=" << base::HexEncode(peer.id)
                  << "; elapsed_ms=" << (now - impl_->discovery_started).InMilliseconds()
                  << "; record_time_ticks_us=" << seen.since_origin().InMicroseconds()
                  << "; record_age_ms=" << (now - seen).InMilliseconds()
                  << "; retained_before_scan=" << (seen < impl_->discovery_started)
                  << "; public_name=" << !EndpointName(peer.endpoint_info).empty()
                  << "; classic_address_present=" << !peer.classic_bluetooth_address.empty()
                  << "; connectable=" << connectable;
        impl_->first_candidate_logged = true;
        impl_->first_connectable_logged |= connectable;
      }
    }
    candidates.push_back({peer, seen,
        std::string(source) + ":" + address + ":" + base::NumberToString(slot) +
            ":" + base::HexEncode(peer.id),
        SystemName(impl_->adapter.get(), address)});
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
      append(record.peer, address, std::min(entry.seen, record.seen), "gatt", slot);
  if (!probe_candidate) impl_->bootstrap_waiting_for_gatt = false;
  // Prefer the most recently observed address over map iteration order.
  std::ranges::stable_sort(candidates, [](const Candidate& a, const Candidate& b) {
    return a.seen > b.seen;
  });
  // Match Nearby BasePcpHandler::OnEndpointFound: endpoint ID identifies
  // the device; changing endpoint_info replaces its previous medium records.
  // Keep only routes carrying the selected full info, never splice a new BLE
  // advertisement onto a LAN route from the previous generation.
  std::set<std::string> present_sources;
  std::set<std::string> present_ids;
  const auto observe = [&](const std::string& source, const Peer& peer) {
    if (peer.id.size() != 4 || peer.endpoint_info.empty()) return;
    present_sources.insert(source);
    present_ids.insert(peer.id);
    const auto previous = impl_->discovery_source_info.find(source);
    if (previous != impl_->discovery_source_info.end() &&
        previous->second == peer.endpoint_info) return;
    impl_->discovery_source_info[source] = peer.endpoint_info;
    auto [selected, inserted] = impl_->endpoint_info_by_id.try_emplace(
        peer.id, peer.endpoint_info);
    if (!inserted && selected->second != peer.endpoint_info) {
      LOG(INFO) << "crQuickShare: Discovery endpoint info replaced; endpoint_id="
                << base::HexEncode(peer.id) << "; source=" << source;
      selected->second = peer.endpoint_info;
    }
  };
  // LAN snapshots have no observation timestamp. Process their changes first,
  // then BLE changes oldest first so the newest BLE observation wins a batch.
  for (const auto& lan : peers) {
    observe("lan:" + base::NumberToString(lan.discovery_cache_id) + ":" +
                base::NumberToString(lan.discovery_interface) + ":" +
                base::HexEncode(lan.id), lan);
  }
  for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
    observe(it->source_key, it->peer);
  }
  std::erase_if(impl_->discovery_source_info, [&](const auto& item) {
    return !present_sources.contains(item.first);
  });
  std::erase_if(impl_->endpoint_info_by_id, [&](const auto& item) {
    return !present_ids.contains(item.first);
  });
  const auto current_info = [&](const Peer& peer) {
    const auto selected = impl_->endpoint_info_by_id.find(peer.id);
    return selected != impl_->endpoint_info_by_id.end() &&
        selected->second == peer.endpoint_info;
  };
  std::erase_if(peers, [&](const Peer& peer) { return !current_info(peer); });
  std::set<std::string> emitted_ids;
  std::erase_if(peers, [&](const Peer& peer) {
    return peer.name.empty() || !emitted_ids.insert(peer.id).second;
  });
  for (const auto& item : candidates) {
    if (!current_info(item.peer) || !emitted_ids.insert(item.peer.id).second)
      continue;
    // Candidates are newest first. Keep one complete route, including its
    // protocol name and Bluetooth address, without reviving older GATT info.
    Peer peer = item.peer;
    if (peer.name.empty()) peer.name = item.system_name;
    peers.push_back(std::move(peer));
  }
  // DCT carries no Sharing endpoint_info and uses its separate discovery path.
  for (const auto& [address, record] : impl_->dct_records) {
    if (now - record.seen < kLifetime) peers.push_back(record.peer);
  }
  return peers;
}
}  // namespace crquickshare
