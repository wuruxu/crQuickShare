// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/quick_share_service.h"
#include "build/build_config.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <thread>
#include <utility>
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "crQuickShare/lan_discovery.h"
#include "crQuickShare/connection_listener.h"
#include "crQuickShare/file_storage.h"
#include "crQuickShare/transport_channel.h"
#include "third_party/nearby/connections/implementation/proto/offline_wire_formats.pb.h"
#include "third_party/nearby/sharing/proto/wire_format.pb.h"
#include "securegcm/ukey2_handshake.h"

namespace crquickshare {
namespace {
namespace nc = location::nearby::connections;
namespace qs = nearby::sharing::service::proto;
using Header = nc::PayloadTransferFrame::PayloadHeader;
constexpr size_t kMaxRecord = 1024 * 1024;
constexpr size_t kChunk = 64 * 1024;
constexpr int64_t kMaxTotal = kMaxTransferBytes;
constexpr size_t kMaxFiles = kMaxTransferFiles;
int64_t PayloadId() { return base::RandUint64() & INT64_MAX; }
nc::OfflineFrame Frame(nc::V1Frame::FrameType type) {
  nc::OfflineFrame f; f.set_version(nc::OfflineFrame::V1); f.mutable_v1()->set_type(type); return f;
}
qs::Frame Sharing(qs::V1Frame::FrameType type) {
  qs::Frame f; f.set_version(qs::Frame::V1); f.mutable_v1()->set_type(type); return f;
}
// Log only protocol capabilities, never endpoint identities or credentials.
template <typename Media>
std::string UpgradeMedia(const Media& media) {
  std::string result;
  for (auto medium : media) {
    if (!result.empty()) result += ",";
    result += base::NumberToString(static_cast<int>(medium));
    if (result.size() > 256) { result += ",..."; break; }
  }
  return result;
}
void LogUpgradeRoles(uint64_t session, const char* source, bool has_metadata,
                     const nc::MediumMetadata& metadata) {
  const auto& role = metadata.medium_role();
  LOG(INFO) << "crQuickShare: Upgrade role diagnostics; session=" << session
            << "; source=" << source << "; has_metadata=" << has_metadata
            << "; has_role=" << metadata.has_medium_role()
            << "; has_hotspot_host=" << role.has_support_wifi_hotspot_host()
            << "; hotspot_host=" << role.support_wifi_hotspot_host()
            << "; has_hotspot_client=" << role.has_support_wifi_hotspot_client()
            << "; hotspot_client=" << role.support_wifi_hotspot_client()
            << "; has_direct_go=" << role.has_support_wifi_direct_group_owner()
            << "; direct_go=" << role.support_wifi_direct_group_owner()
            << "; has_direct_client=" << role.has_support_wifi_direct_group_client()
            << "; direct_client=" << role.support_wifi_direct_group_client();
}
std::string Code(const std::string& token) {
  int hash = 0, multiplier = 1;
  for (unsigned char c : token) { hash = (hash + static_cast<int8_t>(c) * multiplier) % 9973; multiplier = (multiplier * 31) % 9973; }
  return base::StringPrintf("%04d", std::abs(hash));
}
}
struct QuickShareService::Impl {
  mutable std::mutex mutex;
  Snapshot state;
  std::deque<std::pair<base::TimeTicks, uint64_t>> progress_samples;
  void ResetProgress() {  // Called with mutex held.
    state.current_file.clear(); state.current_file_index = 0;
    state.current_file_transferred = state.current_file_total = state.completed_files = 0;
    state.bytes_per_second = 0; state.remaining_seconds = -1;
    state.network_cleanup_failed = false;
    progress_samples.clear();
  }
  std::atomic<bool> stop{false}, cancel{false}, busy{false}, receiving{false};
  std::atomic<int> decision{0};
  std::atomic<bool> refresh_discovery{false}, refresh_advertisement{false};
  std::thread discovery_thread, session_thread;
  std::string name, endpoint, endpoint_info;
  base::FilePath destination;
  struct Request {
    Peer peer;
    std::vector<base::FilePath> paths;
    std::unique_ptr<TransportChannel> bluetooth_channel = {};
    std::function<uint64_t()> socket_sent_bytes = {};
    HotspotConnector hotspot = {};
    std::function<bool()> cancelled = {};
    std::function<void()> finished = {};
    bool incoming = false;
  };
  std::optional<Request> request;
  WifiDirectGoFactory direct_go_factory;
  WifiDirectClientConnector direct_client_connector;
  bool prefer_direct_go = false;
  bool prefer_hotspot_upgrade = false;
  const bool receive_text;
  const TransferJournalFactory journal_factory;
  std::vector<QuickShareService::ReceivedText> received_texts;
  base::TimeTicks visible_until;
  struct Session;
  Impl(std::string n, base::FilePath d, bool allow_text, bool start_discovery = true, TransferJournalFactory journal = {}) : name(std::move(n)), destination(std::move(d)), receive_text(allow_text), journal_factory(std::move(journal)) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int i = 0; i < 4; ++i) endpoint += alphabet[base::RandGenerator(36)];
    endpoint_info = MakeEndpointInfo(name);
    if (start_discovery) discovery_thread = std::thread([this] { Discover(); });
  }
  ~Impl() {
    stop = true; cancel = true;
    if (discovery_thread.joinable()) {
      const auto started = base::TimeTicks::Now();
      LOG(INFO) << "crQuickShare shutdown: stage=discovery_thread; event=started";
      discovery_thread.join();
      LOG(INFO) << "crQuickShare shutdown: stage=discovery_thread; event=finished; elapsed_ms="
                << (base::TimeTicks::Now() - started).InMilliseconds();
    }
    if (session_thread.joinable()) {
      const auto started = base::TimeTicks::Now();
      LOG(INFO) << "crQuickShare shutdown: stage=session_thread; event=started";
      session_thread.join();
      LOG(INFO) << "crQuickShare shutdown: stage=session_thread; event=finished; elapsed_ms="
                << (base::TimeTicks::Now() - started).InMilliseconds();
    }
  }
  template<class F> void Update(F f) { std::lock_guard guard(mutex); f(state); ++state.revision; }
  void PhaseChanged(Phase phase) {
    Update([&](Snapshot& s) {
      s.phase = phase;
      if (phase == Phase::kTransferring) {
        // Retire the display code atomically with transfer start. UI polling
        // can skip this phase for small files and next observe kWaiting.
        s.code.clear();
        progress_samples.clear();
        progress_samples.emplace_back(base::TimeTicks::Now(), s.transferred);
      }
    });
  }
  void Discover();
  void Start(std::unique_ptr<TransportChannel> transport, std::optional<Request> outgoing,
             std::string initial_record = {});
};

// One session worker owns the transport, crypto context and file descriptors.
// UI communicates only through snapshots and atomic decisions. No network IO
// or waiting for a peer occurs on the Views thread.
struct QuickShareService::Impl::Session {
  Impl& owner;
  uint64_t diagnostic_session = 0;
  const char* diagnostic_stage = "created";
  base::TimeTicks diagnostic_started = base::TimeTicks::Now();
  void Stage(const char* value) {
    diagnostic_stage = value;
    LOG(INFO) << "crQuickShare: Connection stage; session=" << diagnostic_session
              << "; outgoing=" << outgoing << "; stage=" << diagnostic_stage
              << "; elapsed_ms=" << (base::TimeTicks::Now() - diagnostic_started).InMilliseconds();
  }
  std::unique_ptr<TransportChannel> channel;
  std::unique_ptr<FileStorage> storage = CreateFileStorage();
  struct JournalEntry {
    std::unique_ptr<TransferJournal> record;
    std::string relative_path;
  };
  std::vector<JournalEntry> journals;
  base::FilePath journal_saved_directory;
  bool BeginJournal(const base::FilePath& local, const std::string& remote,
                    const std::string& relative = {}) {
    if (!owner.journal_factory) return true;
    std::string peer;
    { std::lock_guard guard(owner.mutex); peer = owner.state.peer_name; }
    auto record = owner.journal_factory(!outgoing, peer, local, remote);
    if (!record) return Fail("Unable to record file transfer history", Failure::kStorage);
    journals.push_back({std::move(record), relative});
    return true;
  }
  void FinishJournal(bool success) {
    const bool cancelled = owner.cancel || owner.stop || peer_cancelled || rejected ||
        (externally_cancelled && externally_cancelled());
    for (auto& entry : journals) {
      const auto saved = journal_saved_directory.empty() ? base::FilePath() :
          journal_saved_directory.Append(base::FilePath::FromUTF8Unsafe(entry.relative_path));
      entry.record->Finish(success, cancelled, saved);
    }
    journals.clear();  // Persist terminal rows before publishing the result to UI.
  }
  std::unique_ptr<securegcm::D2DConnectionContextV1> crypto;
  std::string buffer;
  bool failed = false, remote_closed = false, rejected = false, confirmed_delivery = true;
  bool empty_incoming_connection = false;
  bool wire_broken = false, peer_cancelled = false, timed_out = false;
  bool finishing = false, safe_request_sent = false, safe_request_received = false;
  bool safe_ack_received = false, terminal_sent = false, terminal_received = false;
  bool outgoing = false, offer_sent = false, offer_received = false, accepted = false;
  int peer_disconnect_version = 0;
  unsigned ignored_extensions = 0;
  unsigned certificate_frames = 0, certificate_bodies = 0;
  uint64_t certificate_count = 0;
  bool pairing_result_received = false;
  unsigned ignored_connection_extensions = 0;
  unsigned upgrade_retry_count = 0;
  uint64_t transport_written_bytes = 0;
  bool bootstrap_probe = false, upgrade_response_received = false;
  bool bluetooth_direct = false;
  bool lan_upgrade_enabled = false, lan_upgrade_attempted = false;
  std::function<uint64_t()> bootstrap_sent_bytes;
  std::string local_endpoint;
  std::string remote_endpoint;
  WifiDirectGoFactory direct_go;
  WifiDirectClientConnector direct_client;
  bool direct_go_preferred = false;
  bool hotspot_preferred = false;
  bool peer_direct_client_supported = false;
  bool peer_requested_direct_go = false;
  bool local_go_attempted = false;
  base::TimeTicks upgrade_negotiation_deadline;
  bool upgrade_path_received = false, upgrade_path_rejected = false;
  nc::BandwidthUpgradeNegotiationFrame::UpgradePathInfo::Medium requested_upgrade_medium =
      nc::BandwidthUpgradeNegotiationFrame::UpgradePathInfo::UNKNOWN_MEDIUM;
  std::vector<nc::BandwidthUpgradeNegotiationFrame::UpgradePathInfo::Medium> requested_upgrade_media;
  std::set<nc::BandwidthUpgradeNegotiationFrame::UpgradePathInfo::Medium> rejected_upgrade_media;
  HotspotConnector hotspot_connector;
  std::function<bool()> externally_cancelled;
  std::optional<nc::BandwidthUpgradeNegotiationFrame::UpgradePathInfo> upgrade_path;
  std::optional<nc::BandwidthUpgradeNegotiationFrame::UpgradePathInfo> joining_peer_path;
  std::unique_ptr<UpgradeNetwork> upgrade_network;
  bool upgrade_in_progress = false, upgrade_completed = false;
  // Selected from the encrypted upgrade path and applied only after migration.
  // Keep the UKEY2 context for the old channel; never infer mode from input.
  bool channel_plaintext = false;
  bool upgrade_local_last = false, upgrade_peer_last = false, upgrade_peer_safe = false;
  base::TimeTicks upgrade_tcp_ready;
  bool upgrade_tcp_received = false;
  // Diagnostics only: count traffic after channel replacement, excluding the
  // introduction and old Bluetooth channel. Never gate transfer on these values.
  struct UpgradeTraffic {
    uint64_t written = 0, received = 0, records = 0, decrypted = 0;
    uint64_t decode_failures = 0, valid_frames = 0, read_would_block = 0;
    uint64_t heartbeat_written = 0, heartbeat_replies_written = 0;
    uint64_t heartbeat_received = 0, heartbeat_acks = 0;
    uint64_t responses_written = 0;
    unsigned heartbeat_logs = 0;
    base::TimeTicks started, last_bytes;
  } upgrade_traffic;
  void LogUpgradeTraffic(bool success) {
    if (!upgrade_completed) return;
    uint32_t declared = 0;
    if (buffer.size() >= 4)
      for (int i = 0; i < 4; ++i) declared = (declared << 8) | uint8_t(buffer[i]);
    const auto now = base::TimeTicks::Now();
    const auto& d = upgrade_traffic;
    LOG(INFO) << "crQuickShare: Post-upgrade traffic summary; session=" << diagnostic_session
              << "; outgoing=" << outgoing << "; medium=" << channel->name()
              << "; stage=" << diagnostic_stage << "; success=" << success
              << "; plaintext=" << channel_plaintext
              << "; elapsed_ms=" << (now - d.started).InMilliseconds()
              << "; written_bytes=" << d.written << "; received_bytes=" << d.received
              << "; records=" << d.records << "; decrypted_records=" << d.decrypted
              << "; decode_failures=" << d.decode_failures << "; valid_frames=" << d.valid_frames
              << "; read_would_block=" << d.read_would_block
              << "; sharing_responses_written=" << d.responses_written
              << "; heartbeat_requests_written=" << d.heartbeat_written
              << "; heartbeat_replies_written=" << d.heartbeat_replies_written
              << "; heartbeat_requests_received=" << d.heartbeat_received
              << "; heartbeat_acks_received=" << d.heartbeat_acks
              << "; buffered_bytes=" << buffer.size() << "; declared_record_bytes=" << declared
              << "; pending_bytes=" << queued_bytes << "; pending_records=" << writes.size()
              << "; valid_frame_idle_ms=" << (now - last_received).InMilliseconds()
              << "; byte_idle_ms=" << (d.last_bytes.is_null() ? -1 : (now - d.last_bytes).InMilliseconds())
              << "; remote_closed=" << remote_closed << "; wire_broken=" << wire_broken;
  }
  unsigned upgrade_events = 0;
  base::TimeTicks upgrade_retry_window;
  qs::V1Frame::FrameType expected_sharing = qs::V1Frame::PAIRED_KEY_ENCRYPTION;
  base::TimeTicks finish_deadline, response_deadline;
  base::TimeTicks handshake_deadline;
  base::TimeTicks last_received = base::TimeTicks::Now();
  base::TimeDelta keepalive_interval = base::Seconds(5);
  base::TimeDelta keepalive_timeout = base::Seconds(30);
  uint32_t keepalive_sequence = 0;
  struct SentPayload {
    int64_t size = 0, offset = 0;
    bool last_sent = false, acknowledged = false, retired = false;
    int32_t next_chunk_index = 0;
    std::string file_name, parent_folder;
    uint64_t file_index = 0;
  };
  std::map<int64_t, SentPayload> sent_payloads, sent_controls;
  struct TextPayload { int64_t size = 0; std::string body; bool complete = false; };
  std::map<int64_t, TextPayload> text_payloads;
  struct ChunkSequence {
    std::optional<bool> indexed;
    int64_t next = 0;
    bool empty_final = false;
    std::optional<int32_t> final_index;
  };
  std::map<int64_t, ChunkSequence> received_sequences;
  unsigned duplicate_messages = 0, late_controls = 0;
  bool batch_committed = false;
  std::map<int, std::string> sharing_messages;
  std::map<int64_t, std::pair<int64_t, int64_t>> received_payloads;
  std::map<int64_t, Header> received_file_headers;
  struct PendingWrite {
    std::string bytes;
    size_t offset = 0;
    bool heartbeat = false, heartbeat_ack = false;
    uint32_t heartbeat_sequence = 0;
    std::optional<int> sharing_response;
    size_t sharing_response_bytes = 0;
    std::optional<int64_t> file_id;
    bool control_payload = false;
    int64_t file_offset = 0;
    bool last = false;
  };
  std::deque<PendingWrite> writes;
  std::deque<nc::OfflineFrame> pending_files;
  size_t pending_file_bytes = 0;
  size_t queued_bytes = 0;
  base::TimeTicks write_progress;
  std::set<int64_t> local_ids, completed_bytes;
  bool allow_initial_plain_keepalive = true;
  std::string error = "Connection closed before transfer completed";
  Failure failure = Failure::kNetwork;
  base::TimeTicks last_keepalive = base::TimeTicks::Now();
  std::map<int64_t, std::string> byte_payloads;
  std::map<int64_t, int64_t> byte_sizes;
  std::deque<qs::Frame> sharing_queue;
  Session(Impl& o, std::unique_ptr<TransportChannel> transport) : owner(o), channel(std::move(transport)) {
    static std::atomic<uint64_t> next_session{0};
    diagnostic_session = ++next_session;
    std::lock_guard guard(owner.mutex); local_endpoint = owner.endpoint;
    direct_go = owner.direct_go_factory;
    direct_client = owner.direct_client_connector;
    direct_go_preferred = owner.prefer_direct_go;
    hotspot_preferred = owner.prefer_hotspot_upgrade;
  }
  ~Session() {
    LOG(INFO) << "crQuickShare: Certificate diagnostics summary; outgoing=" << outgoing
              << "; session=" << diagnostic_session << "; pairing_result_received=" << pairing_result_received
              << "; certificate_frames=" << certificate_frames
              << "; certificate_bodies=" << certificate_bodies
              << "; certificates=" << certificate_count
              << "; metadata_decryption_attempted=0; avatar_url_checked=0";
  }
  // This legacy frame is deprecated upstream. Inspect structure only; do not
  // log, retain, decrypt or trust certificate contents. Limit diagnostics using
  // the existing extension-message budget before calling this method.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  void DiagnoseCertificates(const qs::V1Frame& message) {
    ++certificate_frames;
    const bool body_present = message.has_certificate_info();
    unsigned secret_ids = 0, authenticity_keys = 0, public_keys = 0;
    unsigned validity_windows = 0, encrypted_metadata = 0, metadata_tags = 0;
    unsigned complete_fields = 0;
    int count = 0;
    if (body_present) {
      ++certificate_bodies;
      const auto& info = message.certificate_info();
      count = info.public_certificate_size();
      certificate_count += count;
      for (const auto& cert : info.public_certificate()) {
        const bool id_present = !cert.secret_id().empty();
        const bool authenticity_present = !cert.authenticity_key().empty();
        const bool public_present = !cert.public_key().empty();
        const bool times_present = cert.has_start_time() && cert.has_end_time();
        const bool metadata_present = !cert.encrypted_metadata_bytes().empty();
        const bool tag_present = !cert.metadata_encryption_key_tag().empty();
        secret_ids += id_present; authenticity_keys += authenticity_present;
        public_keys += public_present; validity_windows += times_present;
        encrypted_metadata += metadata_present; metadata_tags += tag_present;
        complete_fields += id_present && authenticity_present && public_present &&
            times_present && metadata_present && tag_present;
      }
    }
    LOG(INFO) << "crQuickShare: Certificate diagnostics; outgoing=" << outgoing
              << "; session=" << diagnostic_session << "; body_present=" << body_present << "; certificates=" << count
              << "; with_secret_id=" << secret_ids
              << "; with_authenticity_key=" << authenticity_keys
              << "; with_public_key=" << public_keys
              << "; with_validity_times=" << validity_windows
              << "; with_encrypted_metadata=" << encrypted_metadata
              << "; with_metadata_key_tag=" << metadata_tags
              << "; with_all_fields=" << complete_fields
              << "; accepted=" << accepted << "; expected_sharing=" << static_cast<int>(expected_sharing);
  }
#pragma clang diagnostic pop
  bool Active() const { return !failed && !owner.stop && !owner.cancel && !peer_cancelled && !remote_closed && !rejected && !timed_out &&
      (!externally_cancelled || !externally_cancelled()); }
  bool IoActive() const {
    return !wire_broken && !owner.stop && (finishing ? base::TimeTicks::Now() < finish_deadline : Active());
  }
  bool WireFail(const char* message, int system_error = 0, Failure category = Failure::kNetwork) {
    if (system_error || upgrade_in_progress || upgrade_completed) LOG(WARNING) << "crQuickShare: Transport error; session=" << diagnostic_session
        << "; medium=" << (channel ? channel->name() : "unconnected")
        << "; stage=" << diagnostic_stage << "; errno=" << system_error
        << "; reason=" << message << "; buffered_bytes=" << buffer.size()
        << "; pending_bytes=" << queued_bytes;
    wire_broken = true; return Fail(message, category);
  }
  bool SupportsAck() const { return peer_disconnect_version >= 6; }
  bool SupportsSafeDisconnect() const { return peer_disconnect_version >= 1; }
  int64_t NewId() {
    int64_t id;
    do { id = PayloadId(); } while (received_payloads.contains(id) || text_payloads.contains(id) || byte_sizes.contains(id) || !local_ids.insert(id).second);
    return id;
  }
  void ConfigureKeepalive(int interval_ms, int timeout_ms) {
    // Bound untrusted settings, while preserving interval < timeout.
    keepalive_timeout = base::Milliseconds(std::clamp(timeout_ms, 15000, 120000));
    keepalive_interval = base::Milliseconds(std::clamp(interval_ms, 1000,
        static_cast<int>(keepalive_timeout.InMilliseconds() / 3)));
  }
  bool Fail(const std::string& message, Failure category = Failure::kProtocol) {
    if (!failed) {
      error = message;
      failure = category;
      LOG(WARNING) << "crQuickShare: Connection failure; session=" << diagnostic_session
                   << "; stage=" << diagnostic_stage << "; reason=" << message
                   << "; elapsed_ms=" << (base::TimeTicks::Now() - diagnostic_started).InMilliseconds();
    }
    failed = true; return false;
  }
  bool WriteRaw(const std::string& data) {
    if (data.size() > kMaxRecord) return Fail("Oversized outgoing frame");
    std::string bytes(4, '\0');
    for (int i = 0; i < 4; ++i) bytes[i] = char(data.size() >> (24 - 8 * i));
    bytes += data;
    size_t sent = 0; auto deadline = base::TimeTicks::Now() + base::Seconds(15);
    if (!handshake_deadline.is_null()) deadline = std::min(deadline, handshake_deadline);
    while (Active() && sent < bytes.size() && base::TimeTicks::Now() < deadline) {
      const auto result = channel->Write(base::span<const char>(std::string_view(bytes).substr(sent)));
      if (result.status == TransportChannel::Status::kOk && result.bytes) { sent += result.bytes; transport_written_bytes += result.bytes; continue; }
      if (result.status == TransportChannel::Status::kWouldBlock) {
        if (!WaitWritable(100)) return false;
        continue;
      }
      return WireFail("Transport write failed", result.system_error);
    }
    if (sent != bytes.size()) {
      if (!handshake_deadline.is_null() && base::TimeTicks::Now() >= handshake_deadline) {
        timed_out = true;
        return Fail("Handshake timed out", Failure::kConnection);
      }
      return WireFail("Transport write cancelled or timed out");
    }
    return true;
  }
  bool WaitWritable(int wait_ms) {
    const bool draining_bluetooth = bluetooth_direct && !writes.empty() &&
        writes.front().offset == writes.front().bytes.size();
    const auto ready = channel->Wait(false, !draining_bluetooth, wait_ms);
    if (ready.system_error) return WireFail("Transport wait failed", ready.system_error);
    return true;
  }
  bool PumpOutput() {
    size_t budget = 256 * 1024;
    while (IoActive() && !writes.empty() && budget) {
      auto& w = writes.front();
      if (w.offset < w.bytes.size()) {
        auto remaining = std::string_view(w.bytes).substr(w.offset);
        const auto result = channel->Write(base::span<const char>(remaining.substr(0, budget)));
        if (result.status == TransportChannel::Status::kWouldBlock) break;
        if (result.status != TransportChannel::Status::kOk || !result.bytes)
          return WireFail("Transport write failed", result.system_error);
        w.offset += result.bytes; budget -= result.bytes; write_progress = base::TimeTicks::Now();
        transport_written_bytes += result.bytes;
        if (upgrade_completed) upgrade_traffic.written += result.bytes;
      }
      if (w.offset == w.bytes.size()) {
        // The Bluetooth bridge queues asynchronously. Retire records and
        // report payload progress only after the socket's send callback.
        if (bluetooth_direct && bootstrap_sent_bytes() < transport_written_bytes) break;
        if (upgrade_completed && w.heartbeat) {
          if (w.heartbeat_ack) ++upgrade_traffic.heartbeat_replies_written;
          else ++upgrade_traffic.heartbeat_written;
          if (upgrade_traffic.heartbeat_logs++ < 8)
            LOG(INFO) << "crQuickShare: Post-upgrade heartbeat written; session=" << diagnostic_session
                      << "; ack=" << w.heartbeat_ack << "; sequence=" << w.heartbeat_sequence
                      << "; bytes=" << w.bytes.size() << "; peer_receipt_confirmed=0";
        }
        if (upgrade_completed && w.sharing_response) {
          ++upgrade_traffic.responses_written;
          LOG(INFO) << "crQuickShare: Post-upgrade sharing response written; session=" << diagnostic_session
                    << "; status=" << *w.sharing_response << "; response_bytes=" << w.sharing_response_bytes
                    << "; final_record_bytes=" << w.bytes.size()
                    << "; channel_written_bytes=" << upgrade_traffic.written
                    << "; peer_receipt_confirmed=0";
        }
        if (w.file_id) {
          auto& payload = (w.control_payload ? sent_controls : sent_payloads).at(*w.file_id);
          if (!w.control_payload) {
            owner.Update([&](Snapshot& s) {
              s.current_file = RelativeName(payload.parent_folder, payload.file_name); s.current_file_index = payload.file_index;
              s.current_file_total = payload.size; s.current_file_transferred = w.file_offset;
              s.transferred += w.file_offset - payload.offset;
              if (w.last && !payload.last_sent) ++s.completed_files;
            });
          }
          payload.offset = w.file_offset; payload.last_sent = w.last;
        }
        queued_bytes -= w.bytes.size(); writes.pop_front();
      }
    }
    if (!writes.empty() && base::TimeTicks::Now() - write_progress >= base::Seconds(15))
      return WireFail("Transport write stalled");
    return IoActive();
  }
  // Reads and writes are serviced together; a blocked writer never prevents
  // receiving a cancellation or heartbeat. Partial records survive polls.
  int ReadRaw(std::string& out, int wait_ms) {
    auto extract = [&]() -> int {
      if (buffer.size() < 4) return 0;
      uint32_t n = 0; for (int i = 0; i < 4; ++i) n = (n << 8) | uint8_t(buffer[i]);
      if (!n || n > kMaxRecord) { WireFail("Invalid frame length", 0, Failure::kProtocol); return -1; }
      if (buffer.size() < n + 4) return 0;
      out = buffer.substr(4, n); buffer.erase(0, n + 4);
      return 1;
    };
    if (!IoActive()) return -1;
    int result = extract(); if (result) return result;
    const bool draining_bluetooth = bluetooth_direct && !writes.empty() &&
        writes.front().offset == writes.front().bytes.size();
    const auto ready = channel->Wait(true, !writes.empty() && !draining_bluetooth, wait_ms);
    if (ready.system_error) { WireFail("Transport wait failed", ready.system_error); return -1; }
    if ((ready.writable || draining_bluetooth) && !PumpOutput()) return -1;
    if (!ready.readable) return 0;
    std::array<char, kChunk> bytes;
    const auto read = channel->Read(base::span(bytes));
    if (read.status == TransportChannel::Status::kClosed) {
      LOG(INFO) << "crQuickShare: Peer transport closed; medium=" << channel->name()
                << "; session=" << diagnostic_session << "; stage=" << diagnostic_stage;
      remote_closed = true; wire_broken = true; return -1;
    }
    if (read.status == TransportChannel::Status::kWouldBlock) {
      if (upgrade_completed) ++upgrade_traffic.read_would_block;
      return 0;
    }
    if (read.status != TransportChannel::Status::kOk || !read.bytes) {
      WireFail("Transport read failed", read.system_error); return -1;
    }
    if (upgrade_completed) {
      if (!upgrade_traffic.received)
        LOG(INFO) << "crQuickShare: Post-upgrade first receive; session=" << diagnostic_session
                  << "; bytes=" << read.bytes << "; plaintext=" << channel_plaintext
                  << "; elapsed_ms=" << (base::TimeTicks::Now() - upgrade_traffic.started).InMilliseconds();
      upgrade_traffic.received += read.bytes;
      upgrade_traffic.last_bytes = base::TimeTicks::Now();
    }
    buffer.append(bytes.data(), read.bytes); return extract();
  }
  bool AwaitRaw(std::string& out) {
    auto deadline = base::TimeTicks::Now() + base::Seconds(15);
    if (!handshake_deadline.is_null()) deadline = std::min(deadline, handshake_deadline);
    while (Active() && base::TimeTicks::Now() < deadline) { int r = ReadRaw(out, 100); if (r) return r > 0; }
    if (!Active()) return false;
    timed_out = true; return Fail("Handshake timed out");
  }
  bool Write(const nc::OfflineFrame& frame) {
    if (!crypto) return WriteRaw(frame.SerializeAsString());
    if (!IoActive()) return false;
    std::string encoded;
    if (channel_plaintext) encoded = frame.SerializeAsString();
    else {
      auto encrypted = crypto->EncodeMessageToPeer(frame.SerializeAsString());
      if (!encrypted) return WireFail("Encryption failed", 0, Failure::kProtocol);
      encoded = std::move(*encrypted);
    }
    if (encoded.empty() || encoded.size() > kMaxRecord || queued_bytes + encoded.size() + 4 > 2 * kMaxRecord)
      return WireFail("Encryption failed or output queue exhausted", 0, Failure::kProtocol);
    std::string record(4, '\0');
    for (int i = 0; i < 4; ++i) record[i] = char(encoded.size() >> (24 - 8 * i));
    record += encoded;
    if (writes.empty()) write_progress = base::TimeTicks::Now();
    queued_bytes += record.size();
    PendingWrite pending; pending.bytes = std::move(record);
    if (frame.v1().type() == nc::V1Frame::KEEP_ALIVE && frame.v1().has_keep_alive()) {
      pending.heartbeat = true;
      pending.heartbeat_ack = frame.v1().keep_alive().ack();
      pending.heartbeat_sequence = frame.v1().keep_alive().seq_num();
    }
    writes.push_back(std::move(pending));
    return true;
  }
  bool TickConnection() {
    if (!IoActive()) return false;
    auto now = base::TimeTicks::Now();
    if (crypto && !finishing && !upgrade_local_last) {
      if (now - last_received >= keepalive_timeout) {
        timed_out = true; return Fail("Peer heartbeat timed out");
      }
      if (now - last_keepalive >= keepalive_interval) {
        auto keep = Frame(nc::V1Frame::KEEP_ALIVE);
        keep.mutable_v1()->mutable_keep_alive()->set_ack(false);
        keep.mutable_v1()->mutable_keep_alive()->set_seq_num(++keepalive_sequence);
        if (!Write(keep)) return false;
        last_keepalive = now;
      }
    }
    return PumpOutput();
  }
  int ReadWire(nc::OfflineFrame& frame, int wait_ms = 100) {
    if (!TickConnection()) return -1;
    std::string data; int result = ReadRaw(data, wait_ms); if (result != 1) return result;
    if (upgrade_completed) ++upgrade_traffic.records;
    if (crypto && !channel_plaintext) {
      auto decoded = crypto->DecodeMessageFromPeer(data);
      if (!decoded) {
        if (upgrade_completed) ++upgrade_traffic.decode_failures;
        nc::OfflineFrame early;
        if (!upgrade_completed && allow_initial_plain_keepalive && early.ParseFromString(data) &&
            early.version() == nc::OfflineFrame::V1 && early.has_v1() &&
            early.v1().type() == nc::V1Frame::KEEP_ALIVE && early.v1().has_keep_alive()) {
          allow_initial_plain_keepalive = false; return 0;
        }
        WireFail("Invalid encrypted frame or sequence", 0, Failure::kProtocol); return -1;
      }
      if (upgrade_completed) ++upgrade_traffic.decrypted;
      allow_initial_plain_keepalive = false; data = *decoded;
    }
    if (!frame.ParseFromString(data) || frame.version() != nc::OfflineFrame::V1 || !frame.has_v1()) { WireFail("Invalid Nearby frame", 0, Failure::kProtocol); return -1; }
    if (frame.v1().type() == nc::V1Frame::PAYLOAD_TRANSFER && !ValidatePayload(frame)) return -1;
    if (upgrade_completed && ++upgrade_traffic.valid_frames == 1)
      LOG(INFO) << "crQuickShare: Post-upgrade first valid frame; session=" << diagnostic_session
                << "; type=" << static_cast<int>(frame.v1().type())
                << "; decrypted=" << (crypto && !channel_plaintext);
    last_received = base::TimeTicks::Now();
    const auto& v = frame.v1();
    // These are Nearby Connections extensions, not Sharing's paired-key
    // messages carried inside BYTES payloads. Upstream EndpointManager skips
    // frames without a registered processor. This anonymous implementation
    // does not interpret them as identity verification or user consent.
    if (v.type() == nc::V1Frame::PAIRED_KEY_ENCRYPTION ||
        v.type() == nc::V1Frame::AUTHENTICATION_MESSAGE ||
        v.type() == nc::V1Frame::AUTHENTICATION_RESULT) {
      bool has_body = (v.type() == nc::V1Frame::PAIRED_KEY_ENCRYPTION && v.has_paired_key_encryption()) ||
          (v.type() == nc::V1Frame::AUTHENTICATION_MESSAGE && v.has_authentication_message()) ||
          (v.type() == nc::V1Frame::AUTHENTICATION_RESULT && v.has_authentication_result());
      if (!has_body || ++ignored_connection_extensions > 16) {
        Fail("Invalid or excessive connection authentication extensions"); return -1;
      }
      LOG(WARNING) << "crQuickShare: Skipping optional connection extension type="
                   << static_cast<int>(v.type());
      return 0;
    }
    if (v.type() == nc::V1Frame::KEEP_ALIVE) {
      if (!v.has_keep_alive()) { Fail("Missing heartbeat fields"); return -1; }
      if (upgrade_completed) {
        if (v.keep_alive().ack()) ++upgrade_traffic.heartbeat_acks;
        else ++upgrade_traffic.heartbeat_received;
        if (upgrade_traffic.heartbeat_logs++ < 8)
          LOG(INFO) << "crQuickShare: Post-upgrade heartbeat received; session=" << diagnostic_session
                    << "; ack=" << v.keep_alive().ack()
                    << "; has_sequence=" << v.keep_alive().has_seq_num()
                    << "; sequence=" << v.keep_alive().seq_num();
      }
      if (!v.keep_alive().ack() && !upgrade_local_last) {
        auto reply = Frame(nc::V1Frame::KEEP_ALIVE);
        reply.mutable_v1()->mutable_keep_alive()->set_ack(true);
        if (v.keep_alive().has_seq_num()) reply.mutable_v1()->mutable_keep_alive()->set_seq_num(v.keep_alive().seq_num());
        if (!Write(reply)) return -1;
      }
      return 0;
    }
    if (v.type() == nc::V1Frame::DISCONNECTION) {
      if (!v.has_disconnection()) { Fail("Missing disconnect fields"); return -1; }
      const auto& d = v.disconnection();
      if (d.request_safe_to_disconnect() && SupportsSafeDisconnect()) {
        if (d.ack_safe_to_disconnect()) {
          if (!safe_request_sent) { Fail("Unsolicited disconnect acknowledgement"); return -1; }
          safe_ack_received = true;
        } else {
          auto reply = Frame(nc::V1Frame::DISCONNECTION);
          reply.mutable_v1()->mutable_disconnection()->set_request_safe_to_disconnect(true);
          reply.mutable_v1()->mutable_disconnection()->set_ack_safe_to_disconnect(true);
          if (!Write(reply)) return -1;
          safe_request_received = true;
        }
      }
      remote_closed = true; return -1;
    }
    if (v.type() == nc::V1Frame::BANDWIDTH_UPGRADE_RETRY) {
      if (!v.has_bandwidth_upgrade_retry()) { Fail("Missing bandwidth upgrade retry fields"); return -1; }
      auto now = base::TimeTicks::Now();
      if (upgrade_retry_window.is_null() || now - upgrade_retry_window >= base::Minutes(1)) {
        upgrade_retry_window = now; upgrade_retry_count = 0;
      }
      if (++upgrade_retry_count > 32) { Fail("Too many bandwidth upgrade retry messages"); return -1; }
      const auto& retry = v.bandwidth_upgrade_retry();
      if (bootstrap_probe) {
        if (retry.supported_medium_size() > 32) { Fail("Too many upgrade media"); return -1; }
        if (!retry.is_request()) upgrade_response_received = true;
        LOG(INFO) << "crQuickShare: Bootstrap upgrade capabilities; request=" << retry.is_request()
                  << "; medium_count=" << retry.supported_medium_size();
        for (auto medium : retry.supported_medium())
          LOG(INFO) << "crQuickShare: Bootstrap peer medium; medium=" << static_cast<int>(medium);
      }
      if (retry.is_request()) {
        // This frame exchanges available media; it does not approve a file
        // transfer or require replacing the current encrypted LAN channel.
        auto response = Frame(nc::V1Frame::BANDWIDTH_UPGRADE_RETRY);
        auto* reply = response.mutable_v1()->mutable_bandwidth_upgrade_retry();
        reply->set_is_request(false);
        reply->add_supported_medium(std::string_view(channel->name()) == "BLUETOOTH" ?
            nc::BandwidthUpgradeRetryFrame::BLUETOOTH : nc::BandwidthUpgradeRetryFrame::WIFI_LAN);
        if (lan_upgrade_enabled && !upgrade_completed) reply->add_supported_medium(nc::BandwidthUpgradeRetryFrame::WIFI_LAN);
        if (hotspot_connector) reply->add_supported_medium(nc::BandwidthUpgradeRetryFrame::WIFI_HOTSPOT);
        if (bootstrap_probe && (direct_client || direct_go) && !upgrade_completed)
          reply->add_supported_medium(nc::BandwidthUpgradeRetryFrame::WIFI_DIRECT);
        if (!Write(response)) return -1;
      }
      if (upgrade_retry_count == 1)
        LOG(WARNING) << "crQuickShare: Bandwidth upgrade retry handled; request="
                     << retry.is_request() << "; retaining=" << channel->name();
      return 0;
    }
    if (v.type() == nc::V1Frame::BANDWIDTH_UPGRADE_NEGOTIATION) {
      if (!v.has_bandwidth_upgrade_negotiation()) { Fail("Missing upgrade fields"); return -1; }
      if (bootstrap_probe) {
        if (++upgrade_events > 32) { Fail("Too many bootstrap upgrade events"); return -1; }
        const auto& upgrade = v.bandwidth_upgrade_negotiation();
        const auto& path = upgrade.upgrade_path_info();
        LOG(INFO) << "crQuickShare: Upgrade negotiation received; session=" << diagnostic_session
                  << "; has_event=" << upgrade.has_event_type() << "; event=" << static_cast<int>(upgrade.event_type())
                  << "; has_path=" << upgrade.has_upgrade_path_info() << "; has_medium=" << path.has_medium()
                  << "; medium=" << static_cast<int>(path.medium()) << "; requested_media=" << UpgradeMedia(requested_upgrade_media)
                  << "; selected_medium=" << static_cast<int>(requested_upgrade_medium)
                  << "; hotspot_credentials=" << path.has_wifi_hotspot_credentials()
                  << "; direct_credentials=" << path.has_wifi_direct_credentials() << "; lan_socket=" << path.has_wifi_lan_socket()
                  << "; has_request=" << path.has_upgrade_path_request();
        if (upgrade.event_type() == nc::BandwidthUpgradeNegotiationFrame::UPGRADE_PATH_REQUEST) {
          const auto& path_request = path.upgrade_path_request();
          if (!path.has_upgrade_path_request() || path_request.mediums_size() == 0 ||
              path_request.mediums_size() > 32) {
            Fail("Invalid upgrade path request"); return -1;
          }
          if (upgrade_local_last || upgrade_peer_last || upgrade_completed) {
            LOG(INFO) << "crQuickShare: Peer upgrade request; session=" << diagnostic_session
                      << "; action=ignored; reason=channel_migration_committed";
            return 0;
          }
          using Path = nc::BandwidthUpgradeNegotiationFrame::UpgradePathInfo;
          const bool requests_direct = std::find(path_request.mediums().begin(),
              path_request.mediums().end(), Path::WIFI_DIRECT) != path_request.mediums().end();
          const bool can_offer = direct_go && requests_direct &&
              path_request.medium_meta_data().medium_role().support_wifi_direct_group_client() &&
              (outgoing || !accepted) && !local_go_attempted &&
              !upgrade_in_progress && !upgrade_completed && !upgrade_path &&
              !upgrade_local_last && !upgrade_peer_last;
          LOG(INFO) << "crQuickShare: Peer upgrade request; session=" << diagnostic_session
                    << "; media=" << UpgradeMedia(path_request.mediums())
                    << "; action=" << (can_offer ? "schedule_direct_go" : "reject")
                    << "; reason=" << (can_offer ? "compatible_roles" : "no_available_host_role");
          LogUpgradeRoles(diagnostic_session, "peer_upgrade_request", path_request.has_medium_meta_data(), path_request.medium_meta_data());
          if (can_offer) {
            peer_requested_direct_go = true;
          } else {
            std::set<int> rejected_request_media;
            for (auto medium : path_request.mediums()) {
              if (!rejected_request_media.insert(medium).second) continue;
              auto response = Frame(nc::V1Frame::BANDWIDTH_UPGRADE_NEGOTIATION);
              auto* body = response.mutable_v1()->mutable_bandwidth_upgrade_negotiation();
              body->set_event_type(nc::BandwidthUpgradeNegotiationFrame::UPGRADE_FAILURE);
              body->mutable_upgrade_path_info()->set_medium(static_cast<Path::Medium>(medium));
              if (!Write(response)) return -1;
            }
          }
          return 0;
        }
        if (upgrade.event_type() == nc::BandwidthUpgradeNegotiationFrame::UPGRADE_PATH_AVAILABLE ||
            upgrade.event_type() == nc::BandwidthUpgradeNegotiationFrame::UPGRADE_FAILURE) {
          if (!upgrade.has_upgrade_path_info() || !path.has_medium()) {
            Fail("Missing bootstrap upgrade path medium"); return -1;
          }
          upgrade_path_received |= upgrade.event_type() == nc::BandwidthUpgradeNegotiationFrame::UPGRADE_PATH_AVAILABLE;
          // A failure names one medium, not the whole advertised candidate list.
          // During a join only the selected path can fail that join.
          if (upgrade.event_type() == nc::BandwidthUpgradeNegotiationFrame::UPGRADE_FAILURE) {
            const bool requested = std::ranges::find(requested_upgrade_media, path.medium()) != requested_upgrade_media.end();
            if (upgrade_in_progress) {
              if (path.medium() == requested_upgrade_medium) upgrade_path_rejected = true;
            } else if (requested) {
              rejected_upgrade_media.insert(path.medium());
              upgrade_path_rejected = std::ranges::all_of(requested_upgrade_media,
                  [&](auto medium) { return rejected_upgrade_media.contains(medium); });
            }
            LOG(INFO) << "crQuickShare: Upgrade medium rejected; session=" << diagnostic_session
                      << "; medium=" << static_cast<int>(path.medium()) << "; requested=" << requested
                      << "; request_exhausted=" << upgrade_path_rejected;
            // Do not join a proposal withdrawn before the coordinator starts it.
            if (!upgrade_in_progress && upgrade_path && upgrade_path->medium() == path.medium())
              upgrade_path.reset();
          }
        }
        if (upgrade.event_type() == nc::BandwidthUpgradeNegotiationFrame::LAST_WRITE_TO_PRIOR_CHANNEL) {
          if (!upgrade_in_progress) { Fail("Unexpected upgrade last-write"); return -1; }
          LOG(INFO) << "crQuickShare: Wi-Fi upgrade barrier; peer_last_write=1";
          upgrade_peer_last = true; return 0;
        }
        if (upgrade.event_type() == nc::BandwidthUpgradeNegotiationFrame::SAFE_TO_CLOSE_PRIOR_CHANNEL) {
          if (!upgrade_in_progress || !upgrade_local_last || !upgrade_peer_last) {
            Fail("Unexpected upgrade safe-to-close"); return -1;
          }
          LOG(INFO) << "crQuickShare: Wi-Fi upgrade barrier; peer_safe_to_close=1";
          upgrade_peer_safe = true; return 0;
        }
        LOG(INFO) << "crQuickShare: Bootstrap upgrade event; event=" << static_cast<int>(upgrade.event_type())
                  << "; has_path=" << upgrade.has_upgrade_path_info()
                  << "; medium=" << static_cast<int>(path.medium())
                  << "; hotspot_credentials=" << path.has_wifi_hotspot_credentials()
                  << "; direct_credentials=" << path.has_wifi_direct_credentials()
                  << "; lan_socket=" << path.has_wifi_lan_socket()
                  << "; has_disabling_encryption=" << path.has_supports_disabling_encryption()
                  << "; disabling_encryption=" << path.supports_disabling_encryption()
                  << "; has_introduction_ack=" << path.has_supports_client_introduction_ack()
                  << "; introduction_ack=" << path.supports_client_introduction_ack()
                  << "; network_switch=0";
      }
      if (v.bandwidth_upgrade_negotiation().event_type() == nc::BandwidthUpgradeNegotiationFrame::UPGRADE_PATH_AVAILABLE) {
        const auto& path = v.bandwidth_upgrade_negotiation().upgrade_path_info();
        if (upgrade_local_last || upgrade_peer_last || upgrade_completed) {
          LOG(INFO) << "crQuickShare: Upgrade path decision; session=" << diagnostic_session
                    << "; action=ignored; reason=channel_migration_committed";
          return 0;
        }
        // Retransmitting the same proposal while NM joins is not a new upgrade.
        if ((joining_peer_path && upgrade_in_progress &&
             joining_peer_path->SerializeAsString() == path.SerializeAsString()) ||
            (upgrade_path && upgrade_path->SerializeAsString() == path.SerializeAsString())) {
          LOG(INFO) << "crQuickShare: Upgrade path decision; session=" << diagnostic_session
                    << "; medium=" << static_cast<int>(path.medium()) << "; action=ignored; reason=duplicate_path";
          return 0;
        }
        if (bootstrap_probe && (outgoing || !accepted) &&
            !upgrade_in_progress && !upgrade_completed && !upgrade_path &&
            ((hotspot_connector && path.medium() == nc::BandwidthUpgradeNegotiationFrame::UpgradePathInfo::WIFI_HOTSPOT &&
              path.has_wifi_hotspot_credentials() && !path.has_wifi_lan_socket() && !path.has_wifi_direct_credentials()) ||
             (lan_upgrade_enabled && !lan_upgrade_attempted &&
              path.medium() == nc::BandwidthUpgradeNegotiationFrame::UpgradePathInfo::WIFI_LAN &&
              path.has_wifi_lan_socket() && !path.has_wifi_hotspot_credentials() && !path.has_wifi_direct_credentials()) ||
             (direct_client && path.medium() == nc::BandwidthUpgradeNegotiationFrame::UpgradePathInfo::WIFI_DIRECT &&
              path.has_wifi_direct_credentials() && !path.has_wifi_lan_socket() && !path.has_wifi_hotspot_credentials())) &&
            !path.has_bluetooth_credentials() && !path.has_wifi_aware_credentials() && !path.has_web_rtc_credentials() &&
            !path.has_awdl_credentials() && !path.has_upgrade_path_request()) {
          LOG(INFO) << "crQuickShare: Upgrade path decision; session=" << diagnostic_session
                    << "; medium=" << static_cast<int>(path.medium()) << "; action=accepted";
          upgrade_path = path;
          return 0;
        }
        using Path = nc::BandwidthUpgradeNegotiationFrame::UpgradePathInfo;
        const char* reason = !bootstrap_probe ? "not_bootstrap" :
            !(outgoing || !accepted) ? "incoming_already_accepted" :
            upgrade_in_progress ? "upgrade_in_progress" : upgrade_completed ? "upgrade_completed" :
            upgrade_path ? "path_already_pending" :
            path.medium() == Path::WIFI_HOTSPOT && !hotspot_connector ? "hotspot_client_disabled" :
            path.medium() == Path::WIFI_DIRECT && !direct_client ? "direct_client_disabled" :
            path.medium() == Path::WIFI_LAN && (!lan_upgrade_enabled || lan_upgrade_attempted) ? "lan_disabled_or_attempted" :
            path.medium() != Path::WIFI_HOTSPOT && path.medium() != Path::WIFI_DIRECT && path.medium() != Path::WIFI_LAN ? "unsupported_medium" :
            "missing_or_conflicting_path_fields";
        LOG(INFO) << "crQuickShare: Upgrade path decision; session=" << diagnostic_session
                  << "; medium=" << static_cast<int>(path.medium()) << "; action=rejected; reason=" << reason;
        auto response = Frame(nc::V1Frame::BANDWIDTH_UPGRADE_NEGOTIATION);
        if (bluetooth_direct)
          LOG(INFO) << "crQuickShare: Bluetooth direct transfer retaining channel; session="
                    << diagnostic_session << "; reason=peer_upgrade_path";
        auto* b = response.mutable_v1()->mutable_bandwidth_upgrade_negotiation();
        b->set_event_type(nc::BandwidthUpgradeNegotiationFrame::UPGRADE_FAILURE);
        *b->mutable_upgrade_path_info() = v.bandwidth_upgrade_negotiation().upgrade_path_info();
        if (!Write(response)) return -1;
      }
      return 0;
    }
    return 1;
  }
  int Read(nc::OfflineFrame& frame, int wait_ms = 100) {
    if (!pending_files.empty()) {
      if (!TickConnection()) return -1;
      pending_file_bytes -= pending_files.front().ByteSizeLong();
      frame = std::move(pending_files.front()); pending_files.pop_front(); return 1;
    }
    return ReadWire(frame, wait_ms);
  }
  bool Flush() {
    while (IoActive()) {
      if (!TickConnection()) return false;
      if (writes.empty()) return true;
      nc::OfflineFrame f; int result = ReadWire(f);
      if (result < 0) return false;
      if (result > 0) {
        // The peer may send the next file while our acceptance/ACK drains.
        // Preserve order without processing file bytes before local consent.
        if (!outgoing && accepted && f.v1().type() == nc::V1Frame::PAYLOAD_TRANSFER &&
            f.v1().payload_transfer().packet_type() == nc::PayloadTransferFrame::DATA &&
            f.v1().payload_transfer().payload_header().type() == Header::FILE) {
          if (pending_files.size() >= 32 || pending_file_bytes + f.ByteSizeLong() > 2 * kMaxRecord)
            return Fail("Incoming file queue exhausted");
          pending_file_bytes += f.ByteSizeLong(); pending_files.push_back(std::move(f));
        } else if (!Control(f)) return false;
      }
    }
    return false;
  }
  bool Handshake(bool is_outgoing) {
    outgoing = is_outgoing;
    if (outgoing) {
      const auto setup = channel->PrepareConnection(
          [this] { return Active(); }, [this](const char* stage) { Stage(stage); });
      if (!setup.connected) {
        if (owner.stop || owner.cancel) return false;
        timed_out = setup.timed_out;
        return Fail(setup.error, Failure::kConnection);
      }
    }
    handshake_deadline = base::TimeTicks::Now() + base::Seconds(15);
    Stage("connection_request");
    if (outgoing) {
      auto f = Frame(nc::V1Frame::CONNECTION_REQUEST);
      auto* r = f.mutable_v1()->mutable_connection_request();
      r->set_endpoint_id(local_endpoint);
      r->set_endpoint_name(owner.endpoint_info);
      r->set_endpoint_info(owner.endpoint_info); r->set_nonce(base::RandUint64() & INT32_MAX);
      r->add_mediums(std::string_view(channel->name()) == "BLUETOOTH" ?
          nc::ConnectionRequestFrame::BLUETOOTH : nc::ConnectionRequestFrame::WIFI_LAN);
      if (lan_upgrade_enabled) r->add_mediums(nc::ConnectionRequestFrame::WIFI_LAN);
      if (hotspot_connector) r->add_mediums(nc::ConnectionRequestFrame::WIFI_HOTSPOT);
      if (bootstrap_probe && (direct_client || direct_go)) r->add_mediums(nc::ConnectionRequestFrame::WIFI_DIRECT);
      if (hotspot_connector || direct_go || direct_client) {
        auto* roles = r->mutable_medium_metadata()->mutable_medium_role();
        roles->set_support_wifi_hotspot_client(bool(hotspot_connector));
        roles->set_support_wifi_hotspot_host(false);
        roles->set_support_wifi_direct_group_client(bool(direct_client));
        roles->set_support_wifi_direct_group_owner(bootstrap_probe && bool(direct_go));
      }
      LOG(INFO) << "crQuickShare: Connection request capabilities; session=" << diagnostic_session
                << "; media=" << UpgradeMedia(r->mediums());
      LogUpgradeRoles(diagnostic_session, "connection_request", r->has_medium_metadata(), r->medium_metadata());
      r->set_keep_alive_interval_millis(5000); r->set_keep_alive_timeout_millis(30000);
      if (!Write(f)) return false;
    } else {
      std::string raw; nc::OfflineFrame f;
      if (!AwaitRaw(raw)) {
        if (remote_closed && buffer.empty() && !failed && !timed_out) {
          empty_incoming_connection = true;
          LOG(INFO) << "crQuickShare: Incoming connection closed without a request; session=" << diagnostic_session;
          return false;
        }
        return Fail("Connection closed before connection request completed", Failure::kConnection);
      }
      if (!f.ParseFromString(raw) || f.version() != nc::OfflineFrame::V1 || f.v1().type() != nc::V1Frame::CONNECTION_REQUEST || !f.v1().has_connection_request()) return Fail("Invalid connection request");
      const auto& r = f.v1().connection_request();
      peer_direct_client_supported = r.medium_metadata().medium_role().support_wifi_direct_group_client();
      ConfigureKeepalive(r.has_keep_alive_interval_millis() ? r.keep_alive_interval_millis() : 5000,
                         r.has_keep_alive_timeout_millis() ? r.keep_alive_timeout_millis() : 30000);
      auto peer_name = EndpointName(r.has_endpoint_info() ? r.endpoint_info() : r.endpoint_name());
      if (peer_name.empty() || r.endpoint_id().size() != 4) return Fail("Invalid endpoint information");
      remote_endpoint = r.endpoint_id();
      const auto& peer_info = r.has_endpoint_info() ? r.endpoint_info() : r.endpoint_name();
      owner.Update([&](Snapshot& s) { s.peer_name = peer_name; s.peer_device_type = EndpointDeviceType(peer_info); });
    }
    Stage("ukey2");
    using Handshake = securegcm::UKey2Handshake;
    auto handshake = outgoing ? Handshake::ForInitiator(Handshake::HandshakeCipher::P256_SHA512) : Handshake::ForResponder(Handshake::HandshakeCipher::P256_SHA512);
    if (!handshake) return Fail("Unable to initialize UKEY2");
    auto send_next = [&] { auto bytes = handshake->GetNextHandshakeMessage(); return bytes && WriteRaw(*bytes); };
    auto receive_next = [&] { std::string bytes; return AwaitRaw(bytes) && handshake->ParseHandshakeMessage(bytes).success; };
    if (outgoing ? !(send_next() && receive_next() && send_next()) : !(receive_next() && send_next() && receive_next())) {
      const auto setup_error = channel->connection_error();
      if (!setup_error.empty()) {
        Stage("bluetooth_connect");
        return Fail(setup_error, Failure::kConnection);
      }
      return Fail("UKEY2 handshake failed");
    }
    auto token = handshake->GetVerificationString(32);
    if (!token) return Fail("Missing verification token");
    owner.Update([&](Snapshot& s) { s.code = Code(*token); });
    Stage("connection_response");
    auto accept = Frame(nc::V1Frame::CONNECTION_RESPONSE);
    accept.mutable_v1()->mutable_connection_response()->set_response(nc::ConnectionResponseFrame::ACCEPT);
    accept.mutable_v1()->mutable_connection_response()->set_status(0);
    accept.mutable_v1()->mutable_connection_response()->set_safe_to_disconnect_version(6);
    accept.mutable_v1()->mutable_connection_response()->set_keep_alive_timeout_millis(keepalive_timeout.InMilliseconds());
    if (!Write(accept)) return false;
    std::string raw; nc::OfflineFrame response;
    if (!AwaitRaw(raw) || !response.ParseFromString(raw) || response.version() != nc::OfflineFrame::V1 || response.v1().type() != nc::V1Frame::CONNECTION_RESPONSE || !response.v1().has_connection_response()) return Fail("Missing connection response");
    const auto& r = response.v1().connection_response();
    if (r.has_response() ? r.response() != nc::ConnectionResponseFrame::ACCEPT : (!r.has_status() || r.status() != 0)) return Fail("Connection rejected");
    peer_disconnect_version = r.has_safe_to_disconnect_version() ? std::max(0, r.safe_to_disconnect_version()) : 0;
    if (r.has_keep_alive_timeout_millis()) ConfigureKeepalive(keepalive_interval.InMilliseconds(), r.keep_alive_timeout_millis());
    // Nearby enables encryption before the sharing-layer visual confirmation.
    // This authorizes metadata exchange only; file IO remains confirmation-gated.
    if (!handshake->VerifyHandshake()) return Fail("Unable to establish secure channel");
    crypto = handshake->ToConnectionContext();
    last_received = last_keepalive = base::TimeTicks::Now();
    if (!crypto) return Fail("Unable to create encrypted channel");
    handshake_deadline = {};
    Stage("encrypted_channel_ready");
    LOG(INFO) << "crQuickShare: Connection negotiated; session=" << diagnostic_session
              << "; disconnect_version=" << peer_disconnect_version
              << "; heartbeat_interval_ms=" << keepalive_interval.InMilliseconds()
              << "; heartbeat_timeout_ms=" << keepalive_timeout.InMilliseconds();
    return true;
  }
  bool Chunk(int64_t id, Header::PayloadType type, int64_t total, int64_t offset, const std::string& body, bool last, int32_t byte_chunk_index = 0) {
    auto f = Frame(nc::V1Frame::PAYLOAD_TRANSFER);
    auto* p = f.mutable_v1()->mutable_payload_transfer(); p->set_packet_type(nc::PayloadTransferFrame::DATA);
    auto* h = p->mutable_payload_header(); h->set_id(id); h->set_type(type); h->set_total_size(total);
    auto* c = p->mutable_payload_chunk(); c->set_offset(offset); c->set_flags(last ? 1 : 0); c->set_body(body);
    if (type == Header::FILE) {
      auto& payload = sent_payloads.at(id);
      h->set_file_name(payload.file_name);
      h->set_parent_folder(payload.parent_folder);
      c->set_index(payload.next_chunk_index);
    } else {
      c->set_index(byte_chunk_index);
    }
    if (!Write(f)) return false;
    if (type == Header::FILE) {
      ++sent_payloads.at(id).next_chunk_index;
    }
    writes.back().file_id = id; writes.back().control_payload = type == Header::BYTES;
    writes.back().file_offset = offset + body.size(); writes.back().last = last;
    return true;
  }
  bool SendSharing(const qs::Frame& frame) {
    if (sent_controls.size() >= 64) return Fail("Too many outgoing control payloads");
    auto bytes = frame.SerializeAsString(); auto id = NewId();
    SentPayload payload; payload.size = bytes.size();
    sent_controls.emplace(id, std::move(payload));
    return Chunk(id, Header::BYTES, bytes.size(), 0, bytes, false, 0) && Chunk(id, Header::BYTES, bytes.size(), bytes.size(), {}, true, 1);
  }
  void RetireSentControls() {
    for (auto& [id, payload] : sent_controls) payload.retired = true;
  }
  // Match Nearby PayloadManager: received ACKs apply to FILE, not BYTES.
  bool AckFile(int64_t id) {
    if (!SupportsAck()) return true;
    auto f = Frame(nc::V1Frame::PAYLOAD_TRANSFER);
    auto* p = f.mutable_v1()->mutable_payload_transfer(); p->set_packet_type(nc::PayloadTransferFrame::PAYLOAD_ACK);
    p->mutable_payload_header()->set_id(id); p->mutable_payload_header()->set_total_size(-1);
    return Write(f);
  }
  // All wire payloads pass here before routing or queueing. This does not
  // advance chunk offsets/indices: queued DATA is consumed separately.
  bool ValidatePayload(const nc::OfflineFrame& frame) {
    if (!frame.v1().has_payload_transfer()) return Fail("Missing payload transfer body");
    const auto& p = frame.v1().payload_transfer();
    const auto& h = p.payload_header();
    if (!p.has_packet_type() || !p.has_payload_header() || !h.has_id())
      return Fail("Missing payload envelope fields");
    switch (p.packet_type()) {
      case nc::PayloadTransferFrame::DATA:
        if (!p.has_payload_chunk() || p.has_control_message())
          return Fail("Invalid DATA message structure");
        if (!h.has_type() || (h.type() != Header::FILE && h.type() != Header::BYTES) ||
            !h.has_total_size() || h.total_size() < 0)
          return Fail("Invalid DATA payload header");
        if (auto text = text_payloads.find(h.id()); text != text_payloads.end()) {
          if (!accepted || h.type() != Header::BYTES || h.total_size() != text->second.size)
            return Fail("Unexpected or changed text payload");
        }
        if (h.type() == Header::FILE) {
          auto known = received_file_headers.find(h.id());
          if (!accepted || known == received_file_headers.end()) return Fail("Unexpected file payload");
          auto& saved = known->second;
          if (h.total_size() != saved.total_size()) return Fail("Changed file payload size");
          // Missing optional fields do not erase earlier values. Preserve the
          // original offered name, not a local name changed to avoid collisions.
          if ((h.has_file_name() && saved.has_file_name() && h.file_name() != saved.file_name()) ||
              (h.has_parent_folder() && saved.has_parent_folder() && h.parent_folder() != saved.parent_folder()) ||
              (h.has_is_sensitive() && saved.has_is_sensitive() && h.is_sensitive() != saved.is_sensitive()) ||
              (h.has_last_modified_timestamp_millis() && saved.has_last_modified_timestamp_millis() &&
               h.last_modified_timestamp_millis() != saved.last_modified_timestamp_millis()))
            return Fail("Conflicting file payload metadata");
          if (h.has_file_name()) saved.set_file_name(h.file_name());
          if (h.has_parent_folder()) saved.set_parent_folder(h.parent_folder());
          if (h.has_is_sensitive()) saved.set_is_sensitive(h.is_sensitive());
          if (h.has_last_modified_timestamp_millis()) saved.set_last_modified_timestamp_millis(h.last_modified_timestamp_millis());
        }
        return true;
      case nc::PayloadTransferFrame::CONTROL:
        if (!p.has_control_message() || p.has_payload_chunk())
          return Fail("Invalid CONTROL message structure");
        return true;
      case nc::PayloadTransferFrame::PAYLOAD_ACK:
        if (p.has_payload_chunk() || p.has_control_message())
          return Fail("Invalid ACK message structure");
        // ACK headers can omit type and carry total_size=-1.
        return true;
      default:
        return Fail("Unsupported payload packet type");
    }
  }
  bool ValidateDataChunk(const nc::PayloadTransferFrame& payload, int64_t total,
                         int64_t offset, bool complete) {
    const auto& h = payload.payload_header();
    const auto& chunk = payload.payload_chunk();
    if (complete) return RepeatedFinal(h.id(), h.total_size(), total, chunk);
    if (h.total_size() != total || !chunk.has_offset() || chunk.offset() != offset ||
        offset < 0 || offset > total || chunk.body().size() > static_cast<uint64_t>(total - offset) ||
        (chunk.flags() & ~1))
      return Fail("Invalid payload chunk bounds or flags");
    if ((chunk.flags() & 1) && chunk.body().size() != static_cast<uint64_t>(total - offset))
      return Fail("Premature final payload chunk");
    return ValidateChunkIndex(h.id(), chunk);
  }
  bool Control(const nc::OfflineFrame& f) {
    if (f.v1().type() != nc::V1Frame::PAYLOAD_TRANSFER || !f.v1().has_payload_transfer())
      return Fail(base::StringPrintf("Unexpected connection frame: type=%d, payload=%d, expected_sharing=%d, accepted=%d",
          static_cast<int>(f.v1().type()), static_cast<int>(f.v1().has_payload_transfer()),
          static_cast<int>(expected_sharing), static_cast<int>(accepted)));
    const auto& p = f.v1().payload_transfer(); const auto& h = p.payload_header();
    if (!p.has_payload_header() || !h.has_id()) return Fail("Missing payload header");
    if (p.packet_type() == nc::PayloadTransferFrame::PAYLOAD_ACK) {
      auto it = sent_payloads.find(h.id());
      // Like upstream ProcessPayloadAckPacket, ignore IDs without an outgoing
      // file. This includes incoming payloads and BYTES control messages.
      if (it == sent_payloads.end()) {
        VLOG(1) << "crQuickShare: Ignoring ACK without an outgoing file; session=" << diagnostic_session;
        return true;
      }
      // Repeated ACKs are idempotent, not protocol-message duplicates.
      if (it->second.acknowledged) return true;
      if (!it->second.last_sent || it->second.offset != it->second.size)
        return Fail("Acknowledgement does not match a completed outgoing payload");
      it->second.acknowledged = true; return true;
    }
    if (p.packet_type() == nc::PayloadTransferFrame::CONTROL) {
      if (!p.has_control_message()) return Fail("Missing payload control fields");
      const auto& c = p.control_message();
      if (!c.has_offset() || c.offset() < 0 ||
          (c.event() != nc::PayloadTransferFrame::ControlMessage::PAYLOAD_CANCELED &&
           c.event() != nc::PayloadTransferFrame::ControlMessage::PAYLOAD_ERROR))
        return Fail("Invalid payload control event or offset");
      auto sent = sent_payloads.find(h.id()); auto received = received_payloads.find(h.id());
      auto control = sent_controls.find(h.id()); auto incoming_control = byte_sizes.find(h.id());
      int64_t limit = -1;
      bool completed_control = false;
      if (sent != sent_payloads.end()) limit = sent->second.offset;
      else if (received != received_payloads.end()) limit = received->second.first;
      else if (auto text = text_payloads.find(h.id()); text != text_payloads.end()) limit = text->second.size;
      else if (control != sent_controls.end()) {
        limit = control->second.offset; completed_control = control->second.last_sent && control->second.retired;
      } else if (incoming_control != byte_sizes.end()) {
        limit = incoming_control->second; completed_control = completed_bytes.contains(h.id());
      }
      if (limit < 0) return Fail("Unknown payload control id");
      if (c.offset() > limit) return Fail("Invalid payload control offset");
      // Completed metadata is already consumed or queued by Sharing. A late
      // transport cleanup for it must not cancel unrelated file payloads.
      if (completed_control) {
        if (++late_controls > 16) return Fail("Too many late control notifications");
        LOG(INFO) << "crQuickShare: Ignoring late metadata control; session=" << diagnostic_session
                  << "; event=" << static_cast<int>(c.event());
        return true;
      }
      if (c.event() == nc::PayloadTransferFrame::ControlMessage::PAYLOAD_CANCELED) {
        peer_cancelled = true; terminal_received = true; error = "Peer cancelled the transfer"; return false;
      }
      terminal_received = true; return Fail("Peer reported a payload error");
    }
    if (auto text = text_payloads.find(h.id()); text != text_payloads.end()) {
      if (!accepted || p.packet_type() != nc::PayloadTransferFrame::DATA || h.type() != Header::BYTES)
        return Fail("Unexpected text payload before approval");
      auto& item = text->second;
      if (!ValidateDataChunk(p, item.size, item.body.size(), item.complete)) return false;
      if (item.complete) return true;
      RetireSentControls();
      const auto& chunk = p.payload_chunk();
      item.body += chunk.body();
      if (chunk.flags() & 1) {
        if (!base::IsStringUTF8(item.body) || item.body.find('\0') != std::string::npos)
          return Fail("Invalid UTF-8 text attachment");
        item.complete = true;
      }
      owner.Update([&](Snapshot& s) {
        s.transferred += chunk.body().size();
        if (item.complete) ++s.completed_files;
      });
      return true;
    }
    if (finishing && batch_committed && p.packet_type() == nc::PayloadTransferFrame::DATA && h.type() == Header::FILE) {
      auto received = received_payloads.find(h.id());
      if (received == received_payloads.end() || !h.has_total_size() || !p.has_payload_chunk())
        return Fail("Unexpected file payload after commit");
      return ValidateDataChunk(p, received->second.first, received->second.second, true);
    }
    if (p.packet_type() != nc::PayloadTransferFrame::DATA || h.type() != Header::BYTES || !p.has_payload_chunk() || !h.has_total_size() || h.total_size() < 0 || h.total_size() > int64_t(kMaxRecord)) return Fail("Unexpected payload before approval");
    const auto& c = p.payload_chunk();
    if (received_payloads.contains(h.id()) || sent_payloads.contains(h.id()) || sent_controls.contains(h.id()))
      return Fail("Reused control payload id");
    if (completed_bytes.contains(h.id())) return ValidateDataChunk(p, byte_sizes.at(h.id()), byte_sizes.at(h.id()), true);
    if (!byte_payloads.contains(h.id()) && byte_payloads.size() >= 4) return Fail("Too many control payloads");
    auto& bytes = byte_payloads[h.id()];
    if (!byte_sizes.contains(h.id())) byte_sizes[h.id()] = h.total_size();
    if (!ValidateDataChunk(p, byte_sizes.at(h.id()), bytes.size(), false)) return false;
    bytes += c.body();
    if (!(c.flags() & 1)) return true;
    qs::Frame frame;
    if (bytes.size() != size_t(h.total_size()) || !frame.ParseFromString(bytes) || frame.version() != qs::Frame::V1 || !frame.has_v1()) return Fail("Invalid sharing message");
    byte_payloads.erase(h.id());
    if (completed_bytes.size() >= 64) return Fail("Too many control payloads");
    completed_bytes.insert(h.id());
    return DispatchSharing(std::move(frame));
  }
  bool ValidateChunkIndex(int64_t id, const nc::PayloadTransferFrame::PayloadChunk& chunk) {
    auto& sequence = received_sequences[id];
    if (!sequence.indexed) sequence.indexed = chunk.has_index();
    if (*sequence.indexed != chunk.has_index() ||
        (chunk.has_index() && (chunk.index() < 0 || chunk.index() != sequence.next)))
      return Fail("Invalid payload chunk index");
    ++sequence.next;
    if (chunk.flags() & 1) {
      sequence.empty_final = chunk.body().empty();
      if (chunk.has_index()) sequence.final_index = chunk.index();
    }
    return true;
  }
  bool RepeatedFinal(int64_t id, int64_t total, int64_t expected_total,
                     const nc::PayloadTransferFrame::PayloadChunk& chunk) {
    const auto it = received_sequences.find(id);
    if (it == received_sequences.end() || !it->second.empty_final || total != expected_total ||
        !chunk.has_offset() || chunk.offset() != expected_total || !chunk.body().empty() || chunk.flags() != 1 ||
        it->second.final_index.has_value() != chunk.has_index() ||
        (chunk.has_index() && chunk.index() != *it->second.final_index))
      return Fail("Conflicting payload after completion");
    if (++duplicate_messages > 16) return Fail("Too many duplicate messages");
    return true;
  }
  bool DispatchSharing(qs::Frame frame) {
    const auto& v = frame.v1();
    const auto type = v.type();
    if (type == qs::V1Frame::CANCEL) { peer_cancelled = true; terminal_received = true; error = "Peer cancelled the transfer"; return false; }
    if (type == qs::V1Frame::CERTIFICATE_INFO || type == qs::V1Frame::PROGRESS_UPDATE || type == qs::V1Frame::UNKNOWN_FRAME_TYPE) {
      if (++ignored_extensions > 16) return Fail("Too many extension messages");
      if (type == qs::V1Frame::CERTIFICATE_INFO) DiagnoseCertificates(v);
      return true;
    }
    const bool body_valid =
        (type == qs::V1Frame::PAIRED_KEY_ENCRYPTION && v.has_paired_key_encryption()) ||
        (type == qs::V1Frame::PAIRED_KEY_RESULT && v.has_paired_key_result() && v.paired_key_result().has_status()) ||
        (type == qs::V1Frame::INTRODUCTION && v.has_introduction()) ||
        (type == qs::V1Frame::RESPONSE && v.has_connection_response() && v.connection_response().has_status());
    if (!body_valid) return Fail("Missing or unsupported sharing message body");
    // An incoming sender cannot grant local consent. This frame is never
    // queued as an acceptance and does not extend any waiting deadline.
    if (!outgoing && type == qs::V1Frame::RESPONSE) {
      if (++ignored_extensions > 16) return Fail("Too many unsolicited sharing responses");
      LOG(INFO) << "crQuickShare: Ignoring incoming sender response; session=" << diagnostic_session
                << "; status=" << static_cast<int>(v.connection_response().status());
      return true;
    }
    if (outgoing && type == qs::V1Frame::INTRODUCTION) return Fail("Unexpected incoming introduction on sending session");
    if (outgoing && type == qs::V1Frame::RESPONSE) {
      if (!offer_sent) return Fail("Sharing response arrived before an offer was sent");
      const auto status = v.connection_response().status();
      if (status != qs::ConnectionResponseFrame::ACCEPT) {
        terminal_received = true;
        if (status == qs::ConnectionResponseFrame::REJECT) { rejected = true; error = "Peer rejected the transfer"; }
        else if (status == qs::ConnectionResponseFrame::TIMED_OUT) { timed_out = true; error = "Peer confirmation timed out"; }
        else if (status == qs::ConnectionResponseFrame::NOT_ENOUGH_SPACE) return Fail("Peer has insufficient storage", Failure::kStorage);
        else if (status == qs::ConnectionResponseFrame::UNSUPPORTED_ATTACHMENT_TYPE) return Fail("Peer does not support these attachments", Failure::kUnsupported);
        else return Fail("Unknown sharing response status");
        return false;
      }
      if (v.connection_response().attachment_details_size() || v.connection_response().stream_metadata_size())
        return Fail("Resumed or streaming responses are not supported", Failure::kUnsupported);
    }
    // Cache at most one immutable message of each supported type. Identical
    // retransmissions are harmless; a changed manifest/result is not.
    const auto serialized = frame.SerializeAsString();
    auto previous = sharing_messages.find(static_cast<int>(type));
    if (previous != sharing_messages.end()) {
      if (previous->second != serialized) return Fail("Conflicting repeated sharing message");
      if (++duplicate_messages > 16) return Fail("Too many duplicate messages");
      return true;
    }
    if (sharing_queue.size() >= 4 || sharing_messages.size() >= 4) return Fail("Too many sharing messages");
    sharing_messages.emplace(static_cast<int>(type), serialized);
    if (type != expected_sharing)
      LOG(INFO) << "crQuickShare: Queuing early sharing message; session=" << diagnostic_session
                << "; type=" << static_cast<int>(type) << "; expected=" << static_cast<int>(expected_sharing);
    sharing_queue.push_back(std::move(frame)); return true;
  }
  bool AwaitSharing(qs::Frame& out, qs::V1Frame::FrameType type) {
    expected_sharing = type;
    if (type == qs::V1Frame::INTRODUCTION) Stage("waiting_introduction");
    if (type == qs::V1Frame::RESPONSE) Stage("peer_confirmation");
    auto deadline = type == qs::V1Frame::RESPONSE ? response_deadline : base::TimeTicks::Now() + base::Seconds(15);
    while (Active() && base::TimeTicks::Now() < deadline) {
      const auto queued = std::find_if(sharing_queue.begin(), sharing_queue.end(),
          [type](const qs::Frame& frame) { return frame.v1().type() == type; });
      if (queued != sharing_queue.end()) {
        out = std::move(*queued); sharing_queue.erase(queued); return true;
      }
      nc::OfflineFrame f; int r = Read(f); if (r < 0 || (r > 0 && !Control(f))) return false;
    }
    if (!Active()) return false;
    timed_out = true; return Fail("Sharing response timed out");
  }
  bool RequestWifiUpgrade(const std::function<uint64_t()>& socket_sent_bytes) {
    using Path = nc::BandwidthUpgradeNegotiationFrame::UpgradePathInfo;
    // Keep negotiation bounded independently of per-medium association timeouts.
    // The acceptor normally offers a path. PATH_REQUEST is a compatibility
    // fallback for peers which do not initiate an upgrade themselves.
    upgrade_negotiation_deadline = base::TimeTicks::Now() + base::Seconds(120);
    auto wait_for_path = [&](base::TimeDelta delay) {
      const auto until = std::min(upgrade_negotiation_deadline, base::TimeTicks::Now() + delay);
      while (Active() && !upgrade_path && !peer_requested_direct_go &&
             base::TimeTicks::Now() < until) {
        nc::OfflineFrame frame;
        const int status = Read(frame);
        if (status < 0 || (status > 0 && !Control(frame))) return false;
      }
      return Active();
    };
    auto flush_bootstrap = [&]() {
      if (!Flush()) return false;
      const uint64_t end = transport_written_bytes;
      const auto until = std::min(upgrade_negotiation_deadline, base::TimeTicks::Now() + base::Seconds(5));
      while (Active() && socket_sent_bytes() < end && base::TimeTicks::Now() < until) {
        nc::OfflineFrame frame;
        const int status = Read(frame);
        if (status < 0 || (status > 0 && !Control(frame))) return false;
      }
      if (!Active()) return false;
      if (socket_sent_bytes() < end) return Fail("Bluetooth upgrade control write timed out", Failure::kConnection);
      return true;
    };
    auto recover_network = [&]() {
      // Complete the local failure write before restoring networking. Continue
      // servicing the old encrypted channel during restoration and backoff.
      if (!flush_bootstrap()) return false;
      if (upgrade_network) {
        Stage("upgrade_network_recovery");
        // Restoration has a separate budget even when negotiation has expired.
        const bool cleaned = upgrade_network->Cleanup(
            base::TimeTicks::Now() + base::Seconds(12), [&] {
              if (!Active()) return false;
              nc::OfflineFrame frame;
              const int status = Read(frame, 0);
              return status >= 0 && (!status || Control(frame)) && Active();
            });
        owner.Update([&](Snapshot& snapshot) { snapshot.network_cleanup_failed = !cleaned; });
        if (!cleaned) return Fail("Unable to restore network after Wi-Fi upgrade failure", Failure::kConnection);
        upgrade_network.reset();
      }
      upgrade_path_rejected = false;
      return Active();
    };
    if (!upgrade_path && !peer_requested_direct_go &&
        !(direct_go && !outgoing && (peer_direct_client_supported || direct_go_preferred)) &&
        !hotspot_preferred && !direct_go_preferred) {
      Stage("upgrade_wait_peer_path");
      if (!wait_for_path(base::Seconds(5))) return false;
    }
    bool list_request_sent = false;
    std::set<Path::Medium> single_requests_sent;
    // Three attempts, including unsolicited offers. Retry only while the old
    // channel remains authoritative; never resume it after either drain marker.
    for (int attempt = 0; attempt < 3 && Active() &&
         base::TimeTicks::Now() < upgrade_negotiation_deadline; ++attempt) {
      if (!upgrade_path && direct_go && !local_go_attempted &&
          (peer_requested_direct_go || (!outgoing && (peer_direct_client_supported || direct_go_preferred)))) {
        peer_requested_direct_go = false;
        local_go_attempted = true;
        upgrade_path.emplace();
        upgrade_path->set_medium(Path::WIFI_DIRECT);
        LOG(INFO) << "crQuickShare: Upgrade role selected; session=" << diagnostic_session
                  << "; role=direct_group_owner; attempt=" << attempt + 1;
      }
      Stage("upgrade_path_probe");
      bool sent_request = false;
      if (!upgrade_path) {
        upgrade_path_received = upgrade_path_rejected = false;
        auto upgrade_request = Frame(nc::V1Frame::BANDWIDTH_UPGRADE_NEGOTIATION);
        auto* negotiation = upgrade_request.mutable_v1()->mutable_bandwidth_upgrade_negotiation();
        negotiation->set_event_type(nc::BandwidthUpgradeNegotiationFrame::UPGRADE_PATH_REQUEST);
        auto* path_request = negotiation->mutable_upgrade_path_info()->mutable_upgrade_path_request();
        std::vector<Path::Medium> candidates;
        auto add_candidate = [&](Path::Medium medium, bool enabled) {
          if (enabled && !rejected_upgrade_media.contains(medium) &&
              std::ranges::find(candidates, medium) == candidates.end()) candidates.push_back(medium);
        };
        // This is a supported-medium list. Its order is a local preference,
        // not a requirement that the peer try the mediums in that order.
        if (direct_go_preferred) add_candidate(Path::WIFI_DIRECT, bool(direct_client));
        if (hotspot_preferred) add_candidate(Path::WIFI_HOTSPOT, bool(hotspot_connector));
        add_candidate(Path::WIFI_LAN, lan_upgrade_enabled && !lan_upgrade_attempted);
        add_candidate(Path::WIFI_DIRECT, bool(direct_client));
        add_candidate(Path::WIFI_HOTSPOT, bool(hotspot_connector));
        requested_upgrade_media.clear();
        requested_upgrade_medium = Path::UNKNOWN_MEDIUM;
        if (!list_request_sent) {
          requested_upgrade_media = candidates;
          list_request_sent = true;
        } else {
          // Older peers may ignore a list request. Retry individual candidates
          // without creating concurrent upgrade transactions.
          for (auto medium : candidates) {
            if (single_requests_sent.contains(medium)) continue;
            requested_upgrade_media.push_back(medium);
            break;
          }
        }
        if (requested_upgrade_media.empty()) break;
        for (auto medium : requested_upgrade_media) path_request->add_mediums(medium);
        sent_request = true;
        auto* roles = path_request->mutable_medium_meta_data()->mutable_medium_role();
        roles->set_support_wifi_direct_group_owner(bool(direct_go) && !local_go_attempted);
        roles->set_support_wifi_direct_group_client(bool(direct_client));
        roles->set_support_wifi_hotspot_host(false);
        roles->set_support_wifi_hotspot_client(bool(hotspot_connector));
        LOG(INFO) << "crQuickShare: Upgrade request diagnostics; session=" << diagnostic_session
                  << "; media=" << UpgradeMedia(path_request->mediums())
                  << "; event=" << static_cast<int>(negotiation->event_type())
                  << "; reason=compatibility_fallback; attempt=" << attempt + 1;
        LogUpgradeRoles(diagnostic_session, "upgrade_request", path_request->has_medium_meta_data(), path_request->medium_meta_data());
        if (!Write(upgrade_request) || !flush_bootstrap()) return false;
        LOG(INFO) << "crQuickShare: Upgrade request socket completed; session=" << diagnostic_session
                  << "; media=" << UpgradeMedia(requested_upgrade_media)
                  << "; peer_receipt_confirmed=0";
        const auto until = std::min(upgrade_negotiation_deadline,
            base::TimeTicks::Now() + base::Seconds(
                requested_upgrade_media.size() == 1 && requested_upgrade_media.front() == Path::WIFI_LAN ? 5 : 15));
        while (Active() && !upgrade_path && !peer_requested_direct_go && !upgrade_path_rejected &&
               base::TimeTicks::Now() < until) {
          nc::OfflineFrame frame;
          const int status = Read(frame);
          if (status < 0 || (status > 0 && !Control(frame))) return false;
        }
        LOG(INFO) << "crQuickShare: Bootstrap path wait; session=" << diagnostic_session
                  << "; path_received=" << bool(upgrade_path) << "; path_rejected=" << upgrade_path_rejected
                  << "; wait_expired=" << (base::TimeTicks::Now() >= until);
        if (!upgrade_path && !peer_requested_direct_go && requested_upgrade_media.size() == 1)
          single_requests_sent.insert(requested_upgrade_media.front());
        if (!upgrade_path && peer_requested_direct_go) {
          peer_requested_direct_go = false;
          local_go_attempted = true;
          upgrade_path.emplace(); upgrade_path->set_medium(Path::WIFI_DIRECT);
        }
      }
      if (!Active()) return false;
      const bool had_path = upgrade_path.has_value();
      const bool lan_path = upgrade_path && upgrade_path->medium() == Path::WIFI_LAN;
      if (upgrade_path) {
        // A real peer offer wins over a queued role-switch request.
        peer_requested_direct_go = false;
        if (UpgradeWifi(socket_sent_bytes)) return true;
      }
      if (!Active() || upgrade_completed || upgrade_local_last || upgrade_peer_last) return false;
      // A request timeout does not prove that a LAN connection was attempted.
      // Continue accepting a delayed valid offer until migration is committed.
      if (lan_path) lan_upgrade_attempted = true;
      if (!recover_network()) return false;
      if (attempt == 2) break;
      if (sent_request && !had_path) continue;
      Stage("upgrade_retry_wait");
      LOG(INFO) << "crQuickShare: Upgrade retry scheduled; session=" << diagnostic_session
                << "; completed_attempts=" << attempt + 1 << "; delay_ms=" << (attempt + 1) * 5000
                << "; retaining=BLUETOOTH";
      if (!wait_for_path(base::Seconds((attempt + 1) * 5))) return false;
    }
    if (!hotspot_connector && !lan_upgrade_enabled && !direct_go && !direct_client) return false;
    if (!Active()) return false;
    timed_out = base::TimeTicks::Now() >= upgrade_negotiation_deadline;
    LOG(WARNING) << "crQuickShare: Upgrade negotiation exhausted; session=" << diagnostic_session
                 << "; reason=" << (timed_out ? "deadline" : "attempt_limit_or_no_medium");
    return Fail("No usable Wi-Fi upgrade path", Failure::kConnection);
  }
  bool UpgradeWifi(const std::function<uint64_t()>& socket_sent_bytes) {
    if (!upgrade_path) return false;
    auto path = std::move(*upgrade_path); upgrade_path.reset();
    requested_upgrade_medium = path.medium();
    requested_upgrade_media = {path.medium()};
    upgrade_path_rejected = false;
    upgrade_in_progress = true;
    const bool lan = path.medium() == nc::BandwidthUpgradeNegotiationFrame::UpgradePathInfo::WIFI_LAN;
    const bool direct = path.medium() == nc::BandwidthUpgradeNegotiationFrame::UpgradePathInfo::WIFI_DIRECT;
    const bool go = direct && !path.has_wifi_direct_credentials();
    if (!go) joining_peer_path = path;
    Stage(go ? "direct_go_create" : direct ? "direct_client_join" : lan ? "lan_upgrade_connect" : "hotspot_join");
    const auto deadline = std::min(upgrade_negotiation_deadline,
        base::TimeTicks::Now() + base::Seconds(lan ? 10 : 40));
    auto service_old = [&]() {
      if (!Active() || upgrade_path_rejected || base::TimeTicks::Now() >= deadline) return false;
      nc::OfflineFrame frame;
      const int result = Read(frame, 0);
      return result >= 0 && (!result || Control(frame)) && !upgrade_path_rejected && Active();
    };
    const auto& c = path.wifi_hotspot_credentials();
    HotspotCredentials credentials{c.ssid(), c.password(), c.gateway(), c.port(), c.frequency()};
    HotspotUpgradeResult result;
    if (lan) {
      const auto& socket = path.wifi_lan_socket();
      auto connected = ConnectLanUpgradeChannel(socket.ip_address(), socket.wifi_port(),
          std::min(deadline, base::TimeTicks::Now() + base::Seconds(3)), service_old);
      result.channel = std::move(connected.channel);
      result.error = connected.outcome;
      LOG(INFO) << "crQuickShare: LAN upgrade connect; session=" << diagnostic_session
                << "; outcome=" << connected.outcome << "; errno=" << connected.system_error;
    } else if (direct && !go && direct_client) {
      const auto& direct_credentials = path.wifi_direct_credentials();
      HotspotCredentials peer{direct_credentials.ssid(), direct_credentials.password(),
                             direct_credentials.gateway(), direct_credentials.port(), direct_credentials.frequency()};
      result = direct_client(peer, deadline, service_old);
    } else if (go && direct_go) {
      auto group = direct_go(deadline, service_old);
      if (group) {
        const auto& credentials_go = group->credentials;
        auto* go_credentials = path.mutable_wifi_direct_credentials();
        go_credentials->set_ssid(credentials_go.ssid); go_credentials->set_password(credentials_go.password);
        go_credentials->set_gateway(credentials_go.gateway); go_credentials->set_port(credentials_go.port);
        go_credentials->set_frequency(credentials_go.frequency);
        if (!group->ipv6_address.empty()) go_credentials->set_ip_v6_address(group->ipv6_address);
        path.set_supports_disabling_encryption(false);
        path.set_supports_client_introduction_ack(true);
        auto available = Frame(nc::V1Frame::BANDWIDTH_UPGRADE_NEGOTIATION);
        auto* proposal = available.mutable_v1()->mutable_bandwidth_upgrade_negotiation();
        proposal->set_event_type(nc::BandwidthUpgradeNegotiationFrame::UPGRADE_PATH_AVAILABLE);
        *proposal->mutable_upgrade_path_info() = path;
        Stage("direct_go_accept");
        if (Write(available) && Flush()) result.channel = group->Accept(deadline, service_old);
        result.network = std::move(group);
      }
      result.error = "direct_go_setup_or_accept_failed";
    } else if (!direct && hotspot_connector) {
      result = hotspot_connector(credentials, deadline, service_old);
    }
    auto reject = [&](const char* reason) {
      if (result.channel) result.channel->Close();
      LOG(WARNING) << "crQuickShare: Wi-Fi upgrade failed; session=" << diagnostic_session
                   << "; reason=" << reason
                   << "; local_last=" << upgrade_local_last << "; peer_last=" << upgrade_peer_last;
      // Once either end has committed to draining the old channel, resuming
      // application writes on Bluetooth would violate the shared cipher order.
      if (upgrade_local_last || upgrade_peer_last) {
        WireFail("Wi-Fi upgrade failed after channel drain began");
        channel->Close();
      } else if (Active()) {
        auto failure_frame = Frame(nc::V1Frame::BANDWIDTH_UPGRADE_NEGOTIATION);
        auto* body = failure_frame.mutable_v1()->mutable_bandwidth_upgrade_negotiation();
        body->set_event_type(nc::BandwidthUpgradeNegotiationFrame::UPGRADE_FAILURE);
        *body->mutable_upgrade_path_info() = path;
        Write(failure_frame);
      }
      upgrade_in_progress = false;
      joining_peer_path.reset();
      // The coordinator restores this network before a pre-drain retry.
      // Terminal/post-drain failures are restored during session cleanup.
      upgrade_network = std::move(result.network);
      return false;
    };
    if (!result.channel) return reject(result.error ? result.error : "wifi_connect_failed");
    upgrade_tcp_ready = base::TimeTicks::Now();
    upgrade_tcp_received = false;
    LOG(INFO) << "crQuickShare: Wi-Fi upgrade TCP ready; session=" << diagnostic_session
              << "; outgoing=" << outgoing << "; accepted=" << accepted
              << "; medium=" << result.channel->name()
              << "; old_buffered_bytes=" << buffer.size() << "; old_pending_bytes=" << queued_bytes;
    Stage("upgrade_client_introduction");
    // The first records on the replacement socket are plaintext Nearby frames;
    // the existing UKEY2 context is retained and used only after both barriers.
    auto log_upgrade_io = [&](const char* operation, int status, int system_error, size_t bytes) {
      LOG(INFO) << "crQuickShare: Wi-Fi upgrade introduction IO; session=" << diagnostic_session
                << "; operation=" << operation << "; status=" << status << "; errno=" << system_error
                << "; bytes=" << bytes
                << "; tcp_age_ms=" << (base::TimeTicks::Now() - upgrade_tcp_ready).InMilliseconds();
    };
    auto write_plain = [&](const nc::OfflineFrame& frame) {
      const auto body = frame.SerializeAsString();
      std::string bytes(4, '\0');
      for (int i = 0; i < 4; ++i) bytes[i] = char(body.size() >> (24 - 8 * i));
      bytes += body;
      size_t offset = 0;
      while (offset < bytes.size() && service_old()) {
        const auto io = result.channel->Write(base::span<const char>(std::string_view(bytes).substr(offset)));
        if (io.status == TransportChannel::Status::kOk && io.bytes) offset += io.bytes;
        else if (io.status != TransportChannel::Status::kWouldBlock) {
          log_upgrade_io("write_failed", static_cast<int>(io.status), io.system_error, offset); return false;
        } else if (const auto ready = result.channel->Wait(false, true, 50); ready.system_error) {
          log_upgrade_io("write_wait_failed", -1, ready.system_error, offset); return false;
        }
      }
      log_upgrade_io("write_finished", offset == bytes.size() ? 0 : -1, 0, offset);
      return offset == bytes.size();
    };
    auto read_exact = [&](std::string& bytes, size_t size) {
      bytes.clear();
      while (bytes.size() < size && service_old()) {
        std::array<char, 4096> data;
        const auto io = result.channel->Read(base::span(data).first(std::min(data.size(), size - bytes.size())));
        if (io.status == TransportChannel::Status::kOk && io.bytes) {
          if (!upgrade_tcp_received) {
            upgrade_tcp_received = true;
            log_upgrade_io("first_receive", static_cast<int>(io.status), 0, io.bytes);
          }
          bytes.append(data.data(), io.bytes);
        } else if (io.status != TransportChannel::Status::kWouldBlock) {
          log_upgrade_io("read_failed", static_cast<int>(io.status), io.system_error, bytes.size()); return false;
        } else if (const auto ready = result.channel->Wait(true, false, 50); ready.system_error) {
          log_upgrade_io("read_wait_failed", -1, ready.system_error, bytes.size()); return false;
        }
      }
      log_upgrade_io("read_finished", bytes.size() == size ? 0 : -1, 0, bytes.size());
      return bytes.size() == size;
    };
    auto intro = Frame(nc::V1Frame::BANDWIDTH_UPGRADE_NEGOTIATION);
    auto* introduction = intro.mutable_v1()->mutable_bandwidth_upgrade_negotiation();
    introduction->set_event_type(nc::BandwidthUpgradeNegotiationFrame::CLIENT_INTRODUCTION);
    introduction->mutable_client_introduction()->set_endpoint_id(local_endpoint);
    // Match Nearby's client protocol: echo the authenticated path's mode in
    // CLIENT_INTRODUCTION and apply it only after the old-channel barriers.
    // Our own GO advertises false; LAN continues to retain UKEY2 encryption.
    const bool negotiated_plaintext = !go && !lan && path.supports_disabling_encryption();
    introduction->mutable_client_introduction()->set_supports_disabling_encryption(negotiated_plaintext);
    LOG(INFO) << "crQuickShare: Wi-Fi upgrade encryption selection; session=" << diagnostic_session
              << "; peer_has_disabling_encryption=" << path.has_supports_disabling_encryption()
              << "; peer_disabling_encryption=" << path.supports_disabling_encryption()
              << "; local_disabling_encryption=" << negotiated_plaintext
              << "; encryption_retained=" << !negotiated_plaintext
              << "; introduction_ack_expected=" << path.supports_client_introduction_ack();
    if (go) {
      std::string bytes;
      if (!read_exact(bytes, 4)) return reject("go_introduction_header_failed");
      uint32_t size = 0;
      for (unsigned char byte : bytes) size = (size << 8) | byte;
      if (!size || size > 4096 || !read_exact(bytes, size)) return reject("go_introduction_length");
      nc::OfflineFrame client;
      if (!client.ParseFromString(bytes) || client.version() != nc::OfflineFrame::V1 || !client.has_v1() ||
          client.v1().type() != nc::V1Frame::BANDWIDTH_UPGRADE_NEGOTIATION ||
          !client.v1().has_bandwidth_upgrade_negotiation()) return reject("go_introduction_invalid");
      const auto& body = client.v1().bandwidth_upgrade_negotiation();
      const auto& peer_intro = body.client_introduction();
      const auto& received_endpoint = peer_intro.endpoint_id();
      // Bound malformed identity output; never log credentials or cipher keys.
      LOG(INFO) << "crQuickShare: GO introduction diagnostics; session=" << diagnostic_session
                << "; outgoing=" << outgoing
                << "; has_event=" << body.has_event_type()
                << "; event=" << static_cast<int>(body.event_type())
                << "; has_introduction=" << body.has_client_introduction()
                << "; has_path=" << body.has_upgrade_path_info()
                << "; has_endpoint=" << peer_intro.has_endpoint_id()
                << "; expected_endpoint_bytes=" << remote_endpoint.size()
                << "; expected_endpoint_hex=" << base::HexEncode(std::string_view(remote_endpoint).substr(0, 16))
                << "; received_endpoint_bytes=" << received_endpoint.size()
                << "; received_endpoint_hex=" << base::HexEncode(std::string_view(received_endpoint).substr(0, 16))
                << "; endpoint_match=" << (received_endpoint == remote_endpoint)
                << "; has_disabling_encryption=" << peer_intro.has_supports_disabling_encryption()
                << "; disabling_encryption=" << peer_intro.supports_disabling_encryption()
                << "; local_disabling_encryption=" << negotiated_plaintext;
      if (body.event_type() != nc::BandwidthUpgradeNegotiationFrame::CLIENT_INTRODUCTION)
        return reject("go_introduction_wrong_event");
      if (!body.has_client_introduction()) return reject("go_introduction_missing_body");
      if (body.has_upgrade_path_info()) return reject("go_introduction_unexpected_path");
      if (remote_endpoint.size() != 4) return reject("go_introduction_expected_endpoint_length");
      if (received_endpoint != remote_endpoint) return reject("go_introduction_endpoint_mismatch");
      if (peer_intro.supports_disabling_encryption()) return reject("go_introduction_disabling_encryption");
      auto ack = Frame(nc::V1Frame::BANDWIDTH_UPGRADE_NEGOTIATION);
      ack.mutable_v1()->mutable_bandwidth_upgrade_negotiation()->set_event_type(
          nc::BandwidthUpgradeNegotiationFrame::CLIENT_INTRODUCTION_ACK);
      if (!write_plain(ack)) return reject("go_introduction_ack_failed");
    } else if (!write_plain(intro)) return reject("client_introduction_write_failed");
    if (!go && path.supports_client_introduction_ack()) {
      std::string bytes;
      if (!read_exact(bytes, 4)) return reject("client_introduction_ack_header_failed");
      uint32_t size = 0;
      for (unsigned char byte : bytes) size = (size << 8) | byte;
      if (!size || size > 4096 || !read_exact(bytes, size)) return reject("client_introduction_ack_length");
      nc::OfflineFrame ack;
      if (!ack.ParseFromString(bytes) || ack.version() != nc::OfflineFrame::V1 || !ack.has_v1() ||
          ack.v1().type() != nc::V1Frame::BANDWIDTH_UPGRADE_NEGOTIATION ||
          !ack.v1().has_bandwidth_upgrade_negotiation() ||
          ack.v1().bandwidth_upgrade_negotiation().event_type() != nc::BandwidthUpgradeNegotiationFrame::CLIENT_INTRODUCTION_ACK ||
          ack.v1().bandwidth_upgrade_negotiation().has_upgrade_path_info() ||
          ack.v1().bandwidth_upgrade_negotiation().has_client_introduction())
        return reject("client_introduction_ack_invalid");
    }
    Stage("upgrade_drain_bluetooth");
    auto event = [&](nc::BandwidthUpgradeNegotiationFrame::EventType type) {
      auto frame = Frame(nc::V1Frame::BANDWIDTH_UPGRADE_NEGOTIATION);
      frame.mutable_v1()->mutable_bandwidth_upgrade_negotiation()->set_event_type(type);
      return Write(frame);
    };
    upgrade_local_last = true;  // Suppress heartbeat writes after this barrier.
    if (!event(nc::BandwidthUpgradeNegotiationFrame::LAST_WRITE_TO_PRIOR_CHANNEL)) return reject("last_write_failed");
    while (!upgrade_peer_last && service_old()) channel->Wait(true, !writes.empty(), 50);
    if (!upgrade_peer_last || !Active()) return reject("peer_last_write_missing");
    if (!event(nc::BandwidthUpgradeNegotiationFrame::SAFE_TO_CLOSE_PRIOR_CHANNEL)) return reject("safe_to_close_failed");
    uint64_t barrier_end = transport_written_bytes;
    for (const auto& pending : writes) barrier_end += pending.bytes.size() - pending.offset;
    while (Active() && base::TimeTicks::Now() < deadline) {
      // After SAFE_TO_CLOSE the peer may send the optional plaintext disconnect.
      // Do not feed it to the shared cipher's encrypted-record reader.
      if (upgrade_peer_safe) { if (!PumpOutput()) break; }
      else if (!service_old()) break;
      if (upgrade_peer_safe && writes.empty() && socket_sent_bytes() >= barrier_end) break;
      channel->Wait(!upgrade_peer_safe, !writes.empty(), 50);
    }
    if (!Active() || !upgrade_peer_safe || !writes.empty() || socket_sent_bytes() < barrier_end)
      return reject("old_channel_drain_incomplete");
    Stage("upgrade_close_bluetooth");
    // Match BwuManager::ProcessSafeToClosePriorChannelEvent: the optional
    // disconnect is plaintext, so it cannot advance the shared UKEY2 sequence.
    // The peer may wait for this record or EOF before resuming its new channel.
    auto disconnect_frame = Frame(nc::V1Frame::DISCONNECTION);
    auto* disconnect_body = disconnect_frame.mutable_v1()->mutable_disconnection();
    disconnect_body->set_request_safe_to_disconnect(false);
    disconnect_body->set_ack_safe_to_disconnect(false);
    const auto disconnect_body_bytes = disconnect_frame.SerializeAsString();
    std::string disconnect_record(4, '\0');
    for (int i = 0; i < 4; ++i) disconnect_record[i] = char(disconnect_body_bytes.size() >> (24 - 8 * i));
    disconnect_record += disconnect_body_bytes;
    const auto close_deadline = base::TimeTicks::Now() + base::Seconds(2);
    size_t disconnect_offset = 0;
    bool old_closed = false, close_io_error = false, disconnect_received = false;
    while (Active() && base::TimeTicks::Now() < close_deadline) {
      if (disconnect_offset < disconnect_record.size()) {
        const auto io = channel->Write(base::span<const char>(std::string_view(disconnect_record).substr(disconnect_offset)));
        if (io.status == TransportChannel::Status::kOk && io.bytes) {
          disconnect_offset += io.bytes; transport_written_bytes += io.bytes;
        } else if (io.status == TransportChannel::Status::kClosed) { old_closed = true; break; }
        else if (io.status != TransportChannel::Status::kWouldBlock) { close_io_error = true; break; }
      }
      if (buffer.size() >= 4) {
        uint32_t length = 0;
        for (int i = 0; i < 4; ++i) length = (length << 8) | uint8_t(buffer[i]);
        if (!length || length > 4096) { close_io_error = true; break; }
        if (buffer.size() >= length + 4) {
          nc::OfflineFrame goodbye;
          disconnect_received = goodbye.ParseFromString(buffer.substr(4, length)) &&
              goodbye.version() == nc::OfflineFrame::V1 && goodbye.has_v1() &&
              goodbye.v1().type() == nc::V1Frame::DISCONNECTION && goodbye.v1().has_disconnection();
          if (!disconnect_received) { close_io_error = true; break; }
        }
      }
      if (disconnect_received && disconnect_offset == disconnect_record.size() &&
          socket_sent_bytes() >= transport_written_bytes) break;
      const auto ready = channel->Wait(!disconnect_received, disconnect_offset < disconnect_record.size(), 50);
      if (ready.system_error) { close_io_error = true; break; }
      if (ready.readable) {
        std::array<char, 4096> tail;
        const auto io = channel->Read(base::span(tail));
        if (io.status == TransportChannel::Status::kClosed) { old_closed = true; break; }
        if (io.status == TransportChannel::Status::kOk && io.bytes) buffer.append(tail.data(), io.bytes);
        else if (io.status != TransportChannel::Status::kWouldBlock) { close_io_error = true; break; }
      }
    }
    LOG(INFO) << "crQuickShare: Wi-Fi upgrade prior channel shutdown; session=" << diagnostic_session
              << "; plaintext_disconnect_written=" << (disconnect_offset == disconnect_record.size() && socket_sent_bytes() >= transport_written_bytes)
              << "; plaintext_disconnect_received=" << disconnect_received << "; peer_closed=" << old_closed
              << "; io_error=" << close_io_error << "; wait_expired=" << (base::TimeTicks::Now() >= close_deadline);
    buffer.clear();
    channel->Close();
    if (!Active()) return reject("cancelled_during_old_channel_close");
    channel = std::move(result.channel);
    bluetooth_direct = false;
    channel_plaintext = negotiated_plaintext;
    upgrade_network = std::move(result.network);
    upgrade_in_progress = false; upgrade_completed = true; upgrade_local_last = false;
    joining_peer_path.reset();
    upgrade_traffic = {};
    upgrade_traffic.started = base::TimeTicks::Now();
    last_received = last_keepalive = base::TimeTicks::Now();
    Stage(direct ? "direct_channel_ready" : lan ? "lan_channel_ready" : "hotspot_channel_ready");
    LOG(INFO) << "crQuickShare: Wi-Fi upgrade channel upgraded; encryption_retained=" << !channel_plaintext
              << "; negotiated_plaintext=" << channel_plaintext << "; medium=" << channel->name();
    // Match Nearby's BWU completion: resume normal traffic after draining the
    // old channel. Regular keepalives monitor the new channel without gating
    // file transfer on a separate verification round trip.
    return Active();
  }
  bool Pair() {
    Stage("anonymous_pairing");
    auto f = Sharing(qs::V1Frame::PAIRED_KEY_ENCRYPTION);
    f.mutable_v1()->mutable_paired_key_encryption()->set_signed_data(base::RandBytesAsString(72));
    f.mutable_v1()->mutable_paired_key_encryption()->set_secret_id_hash(base::RandBytesAsString(6));
    qs::Frame remote;
    if (!SendSharing(f) || !AwaitSharing(remote, qs::V1Frame::PAIRED_KEY_ENCRYPTION) || !remote.v1().has_paired_key_encryption()) return false;
    auto result = Sharing(qs::V1Frame::PAIRED_KEY_RESULT);
    result.mutable_v1()->mutable_paired_key_result()->set_status(qs::PairedKeyResultFrame::UNABLE);
    expected_sharing = qs::V1Frame::PAIRED_KEY_RESULT;
    if (!SendSharing(result) || !AwaitSharing(remote, qs::V1Frame::PAIRED_KEY_RESULT) || !remote.v1().has_paired_key_result()) return false;
    auto status = remote.v1().paired_key_result().status();
    pairing_result_received = true;
    LOG(INFO) << "crQuickShare: Anonymous pairing diagnostics; outgoing=" << outgoing
              << "; session=" << diagnostic_session << "; local_result=UNABLE; peer_result=" << static_cast<int>(status);
    if (status != qs::PairedKeyResultFrame::UNABLE && status != qs::PairedKeyResultFrame::SUCCESS)
      return Fail("Peer requires account verification", Failure::kUnsupported);
    RetireSentControls();
    return true;
  }
  bool Confirm() {
    Stage("local_confirmation");
    owner.PhaseChanged(Phase::kConfirm);
    auto deadline = base::TimeTicks::Now() + base::Seconds(55);
    if (outgoing) deadline = std::min(deadline, response_deadline);
    while (Active() && !owner.decision && base::TimeTicks::Now() < deadline) {
      nc::OfflineFrame f; int r = Read(f);
      if (r < 0 || (r > 0 && !Control(f))) return false;
    }
    if (!Active()) return false;
    if (owner.decision == 1) return true;
    // Queue the terminal response while the normal IO state is still active.
    if (owner.decision == -1) {
      if (!outgoing) Response(qs::ConnectionResponseFrame::REJECT);
      rejected = true; error = "Transfer rejected locally";
    } else {
      if (!outgoing) Response(qs::ConnectionResponseFrame::TIMED_OUT);
      timed_out = true; error = "Confirmation timed out";
    }
    return false;
  }
  bool Response(qs::ConnectionResponseFrame::Status status) {
    auto f = Sharing(qs::V1Frame::RESPONSE);
    f.mutable_v1()->mutable_connection_response()->set_status(status);
    const size_t queued_before = queued_bytes;
    if (!SendSharing(f)) return false;
    // SendSharing queues its complete payload synchronously. Completion of the
    // last record proves local socket writes, not receipt or decoding by peer.
    if (upgrade_completed && !writes.empty()) {
      writes.back().sharing_response = static_cast<int>(status);
      writes.back().sharing_response_bytes = queued_bytes - queued_before;
    }
    LOG(INFO) << "crQuickShare: Sharing response queued; session=" << diagnostic_session
              << "; status=" << static_cast<int>(status) << "; medium=" << channel->name()
              << "; upgrade_completed=" << upgrade_completed << "; pending_bytes=" << queued_bytes;
    if (status != qs::ConnectionResponseFrame::ACCEPT) terminal_sent = true;
    return true;
  }
  struct File { int64_t id = 0, size = 0, offset = 0; uint64_t index = 0; std::string name, parent; base::FilePath source_path; base::File handle; bool complete = false; base::Time last_modified; };
  bool PrepareFiles(const Request& send_request, std::vector<File>& files, qs::Frame& intro, int64_t& total) {
    Stage("source_preflight");
    if (send_request.paths.empty() || send_request.paths.size() > kMaxFiles) return Fail("Select between 1 and 1024 files or directories", Failure::kUnsupported);
    intro = Sharing(qs::V1Frame::INTRODUCTION);
    intro.mutable_v1()->mutable_introduction()->set_start_transfer(true);
    SourceSelection selection; StorageError storage_error;
    if (!storage->Collect(send_request.paths, [this] { return Active(); }, selection, storage_error))
      return storage_error.message.empty() ? false : Fail(storage_error.message, storage_error.failure);
    total = selection.total;
    for (auto& source : selection.files) {
      if (!Active()) return false;
      base::File::Info info;
      if (!source.handle.GetInfo(&info) || info.is_directory || info.size != source.size)
        return Fail("Source file changed during directory inspection", Failure::kSourceFile);
      File f; f.id = NewId(); f.index = files.size() + 1; f.size = source.size;
      f.name = std::move(source.name); f.parent = std::move(source.parent); f.handle = std::move(source.handle);
      f.last_modified = info.last_modified;
      f.source_path = std::move(source.path);
      files.push_back(std::move(f));
    }
    for (const auto& f : files) {
      auto* metadata = intro.mutable_v1()->mutable_introduction()->add_file_metadata();
      metadata->set_name(f.name); metadata->set_size(f.size); metadata->set_payload_id(f.id);
      metadata->set_parent_folder(f.parent);
      metadata->set_id(f.id); metadata->set_type(qs::FileMetadata::UNKNOWN); metadata->set_mime_type("application/octet-stream");
      SentPayload payload; payload.size = f.size; payload.file_name = f.name;
      payload.parent_folder = f.parent; payload.file_index = f.index;
      sent_payloads.emplace(f.id, std::move(payload));
    }
    // Sharing messages are currently sent as a single encrypted BYTES record.
    // Reserve space for the payload envelope, signature and encryption padding.
    if (intro.ByteSizeLong() > kMaxRecord - 4096)
      return Fail("Source metadata exceeds the transfer record limit", Failure::kUnsupported);
    LOG(INFO) << "crQuickShare: Source selection collected; files=" << files.size()
              << "; entries=" << selection.entries << "; files_with_parent="
              << std::count_if(files.begin(), files.end(), [](const File& f) { return !f.parent.empty(); });
    owner.Update([&](Snapshot& s) { s.total = total; for (auto& f : files) s.files.push_back(RelativeName(f.parent, f.name)); });
    LOG(INFO) << "crQuickShare: Source preflight complete; session=" << diagnostic_session
              << "; files=" << files.size() << "; total_bytes=" << total
              << "; metadata_bytes=" << intro.ByteSizeLong();
    for (const auto& f : files)
      if (!BeginJournal(f.source_path, RelativeName(f.parent, f.name))) return false;
    return Active();
  }
  bool SourceUnchanged(File& file) {
    base::File::Info info;
    return (file.handle.GetInfo(&info) && !info.is_directory && info.size == file.size &&
            info.last_modified == file.last_modified) || Fail("Source file changed after inspection", Failure::kSourceFile);
  }
  bool SendFiles(std::vector<File>& files, const qs::Frame& intro, int64_t total) {
    // Select before the handshake so advertised media and upgrade replies
    // agree with the transfer policy. This limit applies to the whole batch.
    constexpr int64_t kBluetoothDirectLimit = 72 * 1024;
    bluetooth_direct = bootstrap_sent_bytes && total <= kBluetoothDirectLimit;
    if (bluetooth_direct) { hotspot_connector = {}; lan_upgrade_enabled = false; direct_go = {}; direct_client = {}; }
    else if (bootstrap_sent_bytes && !hotspot_connector && !lan_upgrade_enabled && !direct_client)
      return Fail("This batch requires an available Wi-Fi upgrade path", Failure::kHotspotUnavailable);
    LOG(INFO) << "crQuickShare: Transfer medium selected; session=" << diagnostic_session
              << "; medium=" << (bluetooth_direct ? "BLUETOOTH" :
                  bootstrap_sent_bytes ? (direct_client && direct_go_preferred ? "WIFI_DIRECT_CLIENT_UPGRADE" :
                      hotspot_preferred && hotspot_connector ? "WIFI_HOTSPOT" :
                      lan_upgrade_enabled ? "WIFI_LAN_UPGRADE" : direct_client ? "WIFI_DIRECT_CLIENT_UPGRADE" : "WIFI_HOTSPOT") : "WIFI_LAN")
              << "; total_bytes=" << total << "; bluetooth_limit_bytes=" << kBluetoothDirectLimit
              << "; lan_client=" << lan_upgrade_enabled << "; hotspot_client=" << bool(hotspot_connector)
              << "; direct_client=" << bool(direct_client) << "; direct_go=" << bool(direct_go) << "; prefer_direct_go=" << direct_go_preferred
              << "; prefer_hotspot=" << hotspot_preferred;
    if (!Handshake(true) || !Pair()) return false;
    // Offer on the existing authenticated channel so the receiver can show
    // its confirmation before the hotspot association and DHCP delay.
    expected_sharing = qs::V1Frame::RESPONSE; offer_sent = true;
    response_deadline = base::TimeTicks::Now() + base::Seconds(60);
    Stage("file_introduction");
    if (!SendSharing(intro)) return false;
    LOG(INFO) << "crQuickShare: File introduction queued; session=" << diagnostic_session
              << "; upgrade_pending=" << (bootstrap_sent_bytes && !bluetooth_direct);
    if (!Confirm()) return false;
    owner.PhaseChanged(Phase::kWaiting);
    qs::Frame response;
    if (!AwaitSharing(response, qs::V1Frame::RESPONSE)) return false;
    if (!response.v1().has_connection_response() || response.v1().connection_response().status() != qs::ConnectionResponseFrame::ACCEPT) { rejected = true; return false; }
    RetireSentControls();
    accepted = true; expected_sharing = qs::V1Frame::UNKNOWN_FRAME_TYPE;
    // Do not change networks for a rejected offer. No file bytes are sent
    // until confirmation and, when selected, replacement-channel verification.
    if (bootstrap_sent_bytes && !bluetooth_direct) {
      owner.PhaseChanged(Phase::kConnecting);
      if (!RequestWifiUpgrade(bootstrap_sent_bytes)) {
        if (Active()) Fail("Wi-Fi channel verification failed", Failure::kConnection);
        return false;
      }
    }
    LOG(WARNING) << "crQuickShare: Sending files; count=" << files.size()
                 << "; medium=" << channel->name()
                 << "; total_bytes=" << total << "; peer_disconnect_version=" << peer_disconnect_version
                 << "; start_transfer=1; indexed_chunks=1";
    Stage("file_transfer");
    owner.PhaseChanged(Phase::kTransferring);
    for (auto& f : files) {
      if (!Active() || !SourceUnchanged(f)) return false;
      owner.Update([&](Snapshot& s) {
        s.current_file = RelativeName(f.parent, f.name); s.current_file_index = f.index;
        s.current_file_transferred = 0; s.current_file_total = f.size;
      });
      while (Active() && f.offset < f.size) {
        std::string bytes(std::min<int64_t>(kChunk, f.size - f.offset), '\0');
        if (!f.handle.ReadAtCurrentPosAndCheck(base::as_writable_byte_span(bytes))) return Fail("Source file changed or read failed", Failure::kSourceFile);
        if (!Chunk(f.id, Header::FILE, f.size, f.offset, bytes, false) || !Flush()) return false;
        f.offset += bytes.size();
        // Drain cancellation / acknowledgements between writes.
        for (int i = 0; i < 8; ++i) { nc::OfflineFrame frame; int r = Read(frame, 0); if (r < 0 || (r > 0 && !Control(frame))) return false; if (!r) break; }
      }
      if (!Active() || !SourceUnchanged(f)) return false;
      f.handle.Close();
      if (!Chunk(f.id, Header::FILE, f.size, f.size, {}, true) || !Flush()) return SenderFinishedAfterClose();
    }
    owner.PhaseChanged(Phase::kWaiting);
    Stage("delivery_confirmation");
    LOG(WARNING) << "crQuickShare: All file records written to socket; waiting for delivery confirmation";
    auto deadline = base::TimeTicks::Now() + base::Seconds(60);
    while (Active() && base::TimeTicks::Now() < deadline) {
      bool all = true; for (const auto& [id, payload] : sent_payloads) all &= payload.acknowledged;
      if (all) return true;
      nc::OfflineFrame f; int r = Read(f);
      // Legacy peers close after receiving all payloads instead of sending ACK.
      if (r < 0) return SenderFinishedAfterClose();
      if (r > 0 && !Control(f)) return false;
    }
    if (!Active()) return false;
    int64_t written_bytes = 0;
    size_t acknowledged_files = 0, completed_files = 0;
    for (const auto& [id, payload] : sent_payloads) {
      written_bytes += payload.offset;
      acknowledged_files += payload.acknowledged;
      completed_files += payload.last_sent;
    }
    LOG(WARNING) << "crQuickShare: Delivery confirmation timed out; socket_bytes=" << written_bytes
                 << "; total_bytes=" << total << "; last_chunks=" << completed_files
                 << "; acknowledged_files=" << acknowledged_files << "; files=" << files.size()
                 << "; peer_disconnect_version=" << peer_disconnect_version
                 << "; receive_idle_ms=" << (base::TimeTicks::Now() - last_received).InMilliseconds();
    timed_out = true; return Fail("Peer did not finish the transfer");
  }
  void CleanupNetwork() {
    if (!upgrade_network) return;
    Stage("network_cleanup");
    owner.PhaseChanged(Phase::kRestoringNetwork);
    const auto deadline = base::TimeTicks::Now() + base::Seconds(owner.stop ? 2 : 12);
    const bool cleaned = upgrade_network->Cleanup(deadline, [&] { return !owner.stop; });
    owner.Update([&](Snapshot& s) { s.network_cleanup_failed = !cleaned; });
    upgrade_network.reset();
  }
  bool SenderFinishedAfterClose() {
    if (!remote_closed || failed || owner.cancel || owner.stop || peer_cancelled || rejected || timed_out) return false;
    bool all_acked = true;
    for (const auto& [id, p] : sent_payloads) {
      if (!p.last_sent || p.offset != p.size) return false;
      all_acked &= p.acknowledged;
    }
    if (SupportsAck() && !all_acked) return Fail("Peer disconnected before acknowledging files");
    confirmed_delivery = all_acked; return true;
  }
  bool ReceiveTexts(const qs::IntroductionFrame& offer) {
    if (!owner.receive_text || offer.text_metadata_size() > 16 || offer.file_metadata_size() ||
        offer.wifi_credentials_metadata_size() || offer.app_metadata_size() || offer.stream_metadata_size()) {
      Response(qs::ConnectionResponseFrame::UNSUPPORTED_ATTACHMENT_TYPE);
      return Fail("Unsupported mixed or text attachment batch", Failure::kUnsupported);
    }
    int64_t total = 0;
    std::vector<int64_t> order;
    bool urls_only = true;
    for (const auto& metadata : offer.text_metadata()) {
      // Text subtypes describe how a receiver could open the same BYTES body.
      // Clipboard reception treats all known subtypes as plain text, including
      // ADDRESS and PHONE_NUMBER, and never launches their associated handlers.
      // An omitted type defaults to UNKNOWN and also uses UTF-8 validation.
      const char* metadata_error = nullptr;
      if (!metadata.has_payload_id()) metadata_error = "missing_payload_id";
      else if (!metadata.has_size()) metadata_error = "missing_size";
      else if (metadata.size() <= 0) metadata_error = "nonpositive_size";
      else if (metadata.size() > int64_t(kMaxRecord) - total) metadata_error = "batch_size_limit";
      else if (metadata.type() != qs::TextMetadata::UNKNOWN &&
               metadata.type() != qs::TextMetadata::TEXT && metadata.type() != qs::TextMetadata::URL &&
               metadata.type() != qs::TextMetadata::ADDRESS && metadata.type() != qs::TextMetadata::PHONE_NUMBER)
        metadata_error = "unsupported_text_type";
      else if (text_payloads.contains(metadata.payload_id()) || byte_sizes.contains(metadata.payload_id()) ||
               sent_controls.contains(metadata.payload_id()) || local_ids.contains(metadata.payload_id()))
        metadata_error = "payload_id_collision";
      LOG(INFO) << "crQuickShare: Text attachment metadata; session=" << diagnostic_session
                << "; index=" << order.size() << "; has_type=" << metadata.has_type()
                << "; type=" << static_cast<int>(metadata.type())
                << "; has_payload_id=" << metadata.has_payload_id()
                << "; has_size=" << metadata.has_size() << "; size=" << metadata.size()
                << "; result=" << (metadata_error ? metadata_error : "accepted");
      if (metadata_error) {
        Response(qs::ConnectionResponseFrame::UNSUPPORTED_ATTACHMENT_TYPE);
        return Fail(std::string("Invalid text attachment metadata: ") + metadata_error, Failure::kUnsupported);
      }
      text_payloads.emplace(metadata.payload_id(), TextPayload{metadata.size(), {}, false});
      order.push_back(metadata.payload_id()); total += metadata.size();
      urls_only &= metadata.type() == qs::TextMetadata::URL;
    }
    offer_received = true; expected_sharing = qs::V1Frame::UNKNOWN_FRAME_TYPE;
    owner.Update([&](Snapshot& s) {
      s.total = total; s.text_count = order.size(); s.text_is_url = urls_only;
    });
    if (!Confirm()) return false;
    if (!PrepareIncomingMedium(total)) return false;
    accepted = true;
    if (!Response(qs::ConnectionResponseFrame::ACCEPT) || !Flush()) return false;
    Stage("text_transfer"); owner.PhaseChanged(Phase::kTransferring);
    auto deadline = base::TimeTicks::Now() + base::Seconds(60);
    auto complete = [&] { return std::ranges::all_of(text_payloads, [](const auto& item) { return item.second.complete; }); };
    while (Active() && !complete() && base::TimeTicks::Now() < deadline) {
      size_t before = 0;
      for (const auto& [id, text] : text_payloads) before += text.body.size();
      nc::OfflineFrame frame; int result = Read(frame);
      if (result < 0 || (result > 0 && !Control(frame))) return false;
      size_t after = 0;
      for (const auto& [id, text] : text_payloads) after += text.body.size();
      if (after > before) deadline = base::TimeTicks::Now() + base::Seconds(60);
    }
    if (!Active()) return false;
    if (!complete()) { timed_out = true; return Fail("Text transfer stalled"); }
    std::string content;
    for (auto id : order) {
      if (!content.empty()) content += '\n';
      content += text_payloads.at(id).body;
    }
    {
      std::lock_guard guard(owner.mutex);
      if (owner.received_texts.size() >= 8) return Fail("Text delivery queue full", Failure::kStorage);
      owner.received_texts.push_back({owner.state.session, std::move(content)});
    }
    batch_committed = true;
    LOG(INFO) << "crQuickShare: Text batch received; session=" << diagnostic_session << "; attachments=" << order.size()
              << "; total_bytes=" << total;
    return true;
  }
  bool PrepareIncomingMedium(int64_t total) {
    if (!bluetooth_direct || upgrade_completed) return true;
    const bool required = total > 72 * 1024;
    LOG(INFO) << "crQuickShare: Incoming medium selection; session=" << diagnostic_session
              << "; total_bytes=" << total << "; wifi_upgrade_required=" << required
              << "; hotspot_client=" << bool(hotspot_connector)
              << "; direct_client=" << bool(direct_client) << "; direct_go=" << bool(direct_go)
              << "; lan_client=" << lan_upgrade_enabled
              << "; path_available=" << bool(upgrade_path);
    // Keep small transfers on Bluetooth unless the sender already offered a
    // supported path. Never release ACCEPT until the replacement is ready:
    // the sender can start streaming immediately after receiving it.
    if (!required && !upgrade_path && !peer_requested_direct_go) return true;
    if (!hotspot_connector && !lan_upgrade_enabled && !direct_client && !direct_go)
      return Fail("Large Bluetooth reception requires a Wi-Fi upgrade", Failure::kConnection);
    if (!RequestWifiUpgrade(bootstrap_sent_bytes)) {
      if (!failed && Active())
        Fail("Incoming Wi-Fi upgrade failed", Failure::kConnection);
      return false;
    }
    return true;
  }
  bool ReceiveFiles() {
    if (!Handshake(false) || !Pair()) return false;
    qs::Frame intro;
    if (!AwaitSharing(intro, qs::V1Frame::INTRODUCTION) || !intro.v1().has_introduction()) return false;
    const auto& offer = intro.v1().introduction();
    if (bluetooth_direct && !hotspot_connector && !lan_upgrade_enabled && !direct_client && !direct_go) {
      int64_t bluetooth_total = 0;
      const auto add_size = [&](int64_t size) {
        if (size < 0 || size > 72 * 1024 - bluetooth_total) return false;
        bluetooth_total += size;
        return true;
      };
      bool fits = true;
      for (const auto& metadata : offer.file_metadata()) fits = add_size(metadata.size()) && fits;
      for (const auto& metadata : offer.text_metadata()) fits = add_size(metadata.size()) && fits;
      if (!fits) {
        Response(qs::ConnectionResponseFrame::UNSUPPORTED_ATTACHMENT_TYPE);
        return Fail("Bluetooth reception currently supports batches up to 72 KiB", Failure::kUnsupported);
      }
    }
    if (offer.text_metadata_size()) return ReceiveTexts(offer);
    if (offer.file_metadata_size() < 1 || offer.file_metadata_size() > int(kMaxFiles) || offer.text_metadata_size() || offer.wifi_credentials_metadata_size() || offer.app_metadata_size() || offer.stream_metadata_size()) { Response(qs::ConnectionResponseFrame::UNSUPPORTED_ATTACHMENT_TYPE); return Fail("Only file attachments are supported", Failure::kUnsupported); }
    std::map<int64_t, File> files; std::set<std::string> names, folders; int64_t total = 0;
    // Reserve every parent first, making file/directory collision handling
    // independent of the order in which the introduction lists its files.
    for (const auto& m : offer.file_metadata()) {
      const auto parts = FolderParts(m.parent_folder());
      if (!parts) { Response(qs::ConnectionResponseFrame::UNSUPPORTED_ATTACHMENT_TYPE); return Fail("Unsafe parent folder"); }
      std::string folder;
      for (const auto& part : *parts) { folder = RelativeName(folder, part); folders.insert(folder); }
    }
    for (const auto& m : offer.file_metadata()) {
      if (!m.has_payload_id() || !m.has_size() || m.size() < 0 || m.size() > kMaxTotal - total || (!SafeTransferName(m.name()) || m.name().find(':') != std::string::npos) || files.contains(m.payload_id()) || completed_bytes.contains(m.payload_id()) || byte_payloads.contains(m.payload_id()) || sent_controls.contains(m.payload_id())) { Response(qs::ConnectionResponseFrame::UNSUPPORTED_ATTACHMENT_TYPE); return Fail("Invalid file metadata"); }
      File f; f.id = m.payload_id(); f.size = m.size(); f.name = m.name(); f.parent = m.parent_folder(); total += f.size;
      if (folders.contains(RelativeName(f.parent, f.name))) return Fail("File conflicts with a directory");
      for (unsigned suffix = 2; folders.contains(RelativeName(f.parent, f.name)) ||
           !names.insert(RelativeName(f.parent, f.name)).second; ++suffix) {
        std::string tail = " (" + base::NumberToString(suffix) + ")";
        base::TruncateUTF8ToByteSize(m.name(), 240 - tail.size(), &f.name); f.name += tail;
      }
      received_payloads.emplace(f.id, std::make_pair(f.size, 0));
      auto& header = received_file_headers[f.id];
      header.set_total_size(f.size); header.set_file_name(m.name());
      header.set_parent_folder(m.parent_folder());
      f.index = files.size() + 1;
      files.emplace(f.id, std::move(f));
    }
    LOG(INFO) << "crQuickShare: Receive paths validated; files=" << files.size() << "; directories=" << folders.size();
    offer_received = true; expected_sharing = qs::V1Frame::UNKNOWN_FRAME_TYPE;
    owner.Update([&](Snapshot& s) {
      s.total = total;
      for (const auto& m : offer.file_metadata()) {
        const auto& f = files.at(m.payload_id());
        const auto local = RelativeName(f.parent, f.name);
        const auto original = RelativeName(m.parent_folder(), m.name());
        s.files.push_back(local == original ? local : original + " → " + local);
      }
    });
    if (!Confirm()) return false;
    StorageError storage_error;
    std::vector<ReceivePath> paths;
    for (const auto& [id, f] : files) paths.push_back({f.name, f.parent});
    for (const auto& [id, f] : files) {
      const auto relative = RelativeName(f.parent, f.name);
      const auto& original = received_file_headers.at(id);
      if (!BeginJournal(owner.destination.Append(base::FilePath::FromUTF8Unsafe(relative)),
                        RelativeName(original.parent_folder(), original.file_name()), relative)) return false;
    }
    auto batch = storage->CreateBatch(owner.destination, total, paths, storage_error);
    if (!batch) {
      if (storage_error.no_space) Response(qs::ConnectionResponseFrame::NOT_ENOUGH_SPACE);
      return Fail(storage_error.message, storage_error.failure);
    }
    if (!PrepareIncomingMedium(total)) return false;
    accepted = true;
    if (!Response(qs::ConnectionResponseFrame::ACCEPT) || !Flush()) return false;
    Stage("file_transfer");
    owner.PhaseChanged(Phase::kTransferring);
    int64_t last_completed_id = 0;
    size_t completed = 0; auto deadline = base::TimeTicks::Now() + base::Seconds(60);
    while (Active() && completed < files.size() && base::TimeTicks::Now() < deadline) {
      nc::OfflineFrame frame; int r = Read(frame); if (r < 0) return false; if (!r) continue;
      const auto& p = frame.v1().payload_transfer(); const auto& h = p.payload_header();
      if (frame.v1().type() != nc::V1Frame::PAYLOAD_TRANSFER || h.type() != Header::FILE || p.packet_type() != nc::PayloadTransferFrame::DATA) { if (!Control(frame)) return false; continue; }
      auto entry = files.find(h.id());
      if (!h.has_id() || entry == files.end() || p.packet_type() != nc::PayloadTransferFrame::DATA || !p.has_payload_chunk()) return Fail("Unexpected file payload");
      auto& f = entry->second; const auto& c = p.payload_chunk();
      if (!ValidateDataChunk(p, f.size, f.offset, f.complete)) return false;
      if (f.complete) continue;
      // A valid file chunk proves the sender processed our ACCEPT metadata.
      RetireSentControls();
      if (!batch->Write(RelativeName(f.parent, f.name), base::as_byte_span(c.body()), storage_error)) return Fail(storage_error.message, storage_error.failure);
      f.offset += c.body().size(); received_payloads.at(f.id).second = f.offset;
      owner.Update([&](Snapshot& s) {
        s.transferred += c.body().size(); s.current_file = RelativeName(f.parent, f.name);
        s.current_file_index = f.index; s.current_file_total = f.size;
        s.current_file_transferred = f.offset;
      });
      if (!c.body().empty() || (c.flags() & 1)) deadline = base::TimeTicks::Now() + base::Seconds(60);
      if (c.flags() & 1) {
        if (f.offset != f.size) return Fail("Incomplete file");
        if (!batch->FinishFile(RelativeName(f.parent, f.name), storage_error)) return Fail(storage_error.message, storage_error.failure);
        f.complete = true; ++completed; last_completed_id = f.id;
        owner.Update([&](Snapshot& s) { s.completed_files = completed; });
        // Defer the final ACK until the complete batch has been committed.
        if (completed != files.size() && (!AckFile(f.id) || !Flush())) return false;
      }
    }
    if (!Active()) return false;
    if (completed != files.size()) { timed_out = true; return Fail("File transfer stalled"); }
    if (!batch->Commit(storage_error)) return Fail(storage_error.message, storage_error.failure);
    batch_committed = true;
    journal_saved_directory = batch->saved_directory();
    if (!batch->directory_durable())
      LOG(WARNING) << "crQuickShare: Receive batch saved but directory durability is unconfirmed; session=" << diagnostic_session;
    LOG(INFO) << "crQuickShare: Receive batch committed; session=" << diagnostic_session << "; files=" << completed;
    owner.Update([&](Snapshot& s) { s.received_directory = batch->saved_directory(); });
    if (!AckFile(last_completed_id))
      LOG(WARNING) << "crQuickShare: Final payload ACK could not be queued after commit; session=" << diagnostic_session;
    // Files are received even if the peer disconnects during the final ACK.
    return true;
  }
  void Finish(bool success) {
    if (!crypto || wire_broken || owner.stop) return;
    // A failed migration cannot send CANCEL/disconnect after promising that
    // the prior channel is drained. The caller closes both channels instead.
    if (!upgrade_completed && (upgrade_local_last || upgrade_peer_last)) return;
    finishing = true;
    // Cleanup has its own bounded IO budget; do not clear the user's cancel
    // flag, and never append ciphertext after a truncated/invalid record.
    finish_deadline = base::TimeTicks::Now() + base::Seconds(2);
    if (!success && !terminal_sent && !terminal_received && !remote_closed) {
      if (accepted) {
        auto notify = [&](int64_t id, int64_t size, int64_t offset, Header::PayloadType type = Header::FILE) {
          auto f = Frame(nc::V1Frame::PAYLOAD_TRANSFER);
          auto* p = f.mutable_v1()->mutable_payload_transfer();
          p->set_packet_type(nc::PayloadTransferFrame::CONTROL);
          auto* h = p->mutable_payload_header(); h->set_id(id); h->set_type(type); h->set_total_size(size);
          auto* c = p->mutable_control_message(); c->set_offset(offset);
          c->set_event(owner.cancel || rejected ? nc::PayloadTransferFrame::ControlMessage::PAYLOAD_CANCELED : nc::PayloadTransferFrame::ControlMessage::PAYLOAD_ERROR);
          return Write(f);
        };
        for (const auto& [id, p] : sent_payloads) if (!p.acknowledged && !notify(id, p.size, p.offset)) break;
        for (const auto& [id, p] : received_payloads) if (!batch_committed && !notify(id, p.first, p.second)) break;
        for (const auto& [id, text] : text_payloads) if (!batch_committed && !notify(id, text.size, text.body.size(), Header::BYTES)) break;
      }
      if (offer_received && !accepted && timed_out) Response(qs::ConnectionResponseFrame::TIMED_OUT);
      else SendSharing(Sharing(qs::V1Frame::CANCEL));
      terminal_sent = true;
    }
    // Drain pending encrypted records in sequence, including any disconnect ACK.
    while (IoActive() && !writes.empty()) {
      if (!PumpOutput()) return;
      if (!writes.empty() && !WaitWritable(50)) return;
    }
    if (!IoActive() || remote_closed || safe_request_received) return;
    if (!SupportsSafeDisconnect()) return;
    auto f = Frame(nc::V1Frame::DISCONNECTION);
    f.mutable_v1()->mutable_disconnection()->set_request_safe_to_disconnect(true);
    f.mutable_v1()->mutable_disconnection()->set_ack_safe_to_disconnect(false);
    safe_request_sent = true;
    if (!Write(f)) return;
    while (IoActive() && !safe_ack_received && !remote_closed) {
      nc::OfflineFrame frame; int r = ReadWire(frame);
      if (r < 0) break;
      if (r > 0) Control(frame);
    }
    // Simultaneous close may have queued an ACK for the peer's request.
    while (IoActive() && !writes.empty()) {
      if (!PumpOutput()) break;
      if (!writes.empty() && !WaitWritable(50)) break;
    }
  }

};
bool QuickShareService::ProbeBootstrap(std::unique_ptr<TransportChannel> channel,
                                     std::function<uint64_t()> socket_sent_bytes,
                                     HotspotConnector hotspot, std::function<bool()> cancelled) {
  if (!channel || !socket_sent_bytes) return false;
  Impl owner("crFiles connection probe", base::FilePath(), false, false);
  Impl::Session session(owner, std::move(channel));
  session.outgoing = true;
  session.bootstrap_probe = true;
  session.hotspot_connector = std::move(hotspot);
  session.externally_cancelled = std::move(cancelled);
  const bool handshake = session.Handshake(true) && session.Pair();
  bool upgraded = false;
  LOG(INFO) << "crQuickShare: Bootstrap handshake result; success=" << handshake
            << "; stage=" << session.diagnostic_stage;
  if (handshake) {
    upgraded = session.RequestWifiUpgrade(socket_sent_bytes);
  }
  LOG(INFO) << "crQuickShare: Bootstrap upgrade result; handshake_success=" << handshake
            << "; path_received=" << session.upgrade_path_received << "; path_rejected=" << session.upgrade_path_rejected
            << "; upgrade_completed=" << session.upgrade_completed << "; upgrade_success=" << upgraded
            << "; encryption_retained=" << (upgraded && !session.channel_plaintext)
            << "; negotiated_plaintext=" << session.channel_plaintext
            << "; protocol_failed=" << session.failed << "; medium=" << session.channel->name()
            << "; attachments_sent=0";
  session.Finish(handshake && !session.failed);
  session.channel->Close();
  session.CleanupNetwork();
  return session.hotspot_connector ? upgraded : handshake;
}
void QuickShareService::Impl::Start(std::unique_ptr<TransportChannel> transport, std::optional<Request> outgoing,
                                  std::string initial_record) {
  std::optional<Request> bluetooth_incoming;
  if (outgoing && outgoing->incoming) {
    bluetooth_incoming = std::move(outgoing);
    outgoing.reset();
    transport = std::move(bluetooth_incoming->bluetooth_channel);
  }
  if (session_thread.joinable()) session_thread.join();
  Update([&](Snapshot& s) {
    if (!outgoing && !bluetooth_incoming) ++s.session;
    s.busy = true; s.incoming = !outgoing; s.phase = Phase::kConnecting;
    s.peer_name = outgoing ? outgoing->peer.name : std::string(); s.code.clear(); s.files.clear();
    s.text_count = 0; s.text_is_url = false;
    s.peer_device_type = outgoing ? EndpointDeviceType(outgoing->peer.endpoint_info) : 0;
    s.transferred = s.total = 0; s.diagnostic.clear(); s.received_directory.clear();
    s.failure = Failure::kNone;
    ResetProgress();
  });
  session_thread = std::thread([this, transport = std::move(transport), outgoing = std::move(outgoing),
                                bluetooth_incoming = std::move(bluetooth_incoming),
                                initial_record = std::move(initial_record)]() mutable {
    if (outgoing && outgoing->bluetooth_channel) transport = std::move(outgoing->bluetooth_channel);
    Session session(*this, std::move(transport));
    if (outgoing && outgoing->socket_sent_bytes) {
      session.bootstrap_probe = true;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN)
      session.lan_upgrade_enabled = true;
#endif
      session.bootstrap_sent_bytes = std::move(outgoing->socket_sent_bytes);
      session.hotspot_connector = std::move(outgoing->hotspot);
      session.externally_cancelled = std::move(outgoing->cancelled);
    }
    if (bluetooth_incoming) {
      session.bluetooth_direct = true;
      session.hotspot_connector = std::move(bluetooth_incoming->hotspot);
      session.bootstrap_probe = true;
      session.lan_upgrade_enabled = true;
      session.bootstrap_sent_bytes = std::move(bluetooth_incoming->socket_sent_bytes);
      session.externally_cancelled = std::move(bluetooth_incoming->cancelled);
    }
    session.buffer = std::move(initial_record);
    session.outgoing = outgoing.has_value();
    // Outgoing sessions already know the peer's endpoint from discovery.
    // Preserve it for identity validation when accepting an upgrade as GO.
    if (outgoing) session.remote_endpoint = outgoing->peer.id;
    std::vector<Session::File> prepared_files;
    qs::Frame prepared_intro;
    int64_t prepared_total = 0;
    const bool prepared = !outgoing || session.PrepareFiles(*outgoing, prepared_files, prepared_intro, prepared_total);
    if (!outgoing) session.Stage(bluetooth_incoming ? "bluetooth_accepted" : "tcp_accepted");
    bool connected = session.channel != nullptr;
    if (outgoing && !session.bootstrap_sent_bytes && prepared && session.Active()) {
      session.Stage("tcp_connect");
      LOG(INFO) << "crQuickShare: Selected discovery record; session=" << session.diagnostic_session
                << "; cache_id=" << outgoing->peer.discovery_cache_id << "; interface=" << outgoing->peer.discovery_interface;
      connected = false;
      auto routes = outgoing->peer.routes;
      // Refresh paths after potentially slow source preflight. Retain the
      // selected identity; never substitute another device with the same name.
      {
        std::lock_guard guard(mutex);
        const auto peer = std::ranges::find_if(state.peers, [&](const auto& candidate) {
          return candidate.id == outgoing->peer.id && candidate.endpoint_info == outgoing->peer.endpoint_info &&
                 candidate.port == outgoing->peer.port;
        });
        if (peer != state.peers.end() && !peer->routes.empty()) routes = peer->routes;
      }
      // Compatibility for callers without discovery route information only.
      if (routes.empty()) {
        routes.push_back({outgoing->peer.address, {}, 0});
        for (const auto& ip : outgoing->peer.alternate_addresses)
          if (ip != outgoing->peer.address && routes.size() < 32) routes.push_back({ip, {}, 0});
      }
      auto until = base::TimeTicks::Now() + base::Seconds(10);
      size_t attempt = 0;
      for (const auto& route : routes) {
        if (!session.Active() || base::TimeTicks::Now() >= until) break;
        ++attempt;
        const auto attempt_started = base::TimeTicks::Now();
        auto report_attempt = [&](const char* result_name, int system_error) {
          LOG(INFO) << "crQuickShare: TCP attempt; session=" << session.diagnostic_session
                    << "; address_index=" << attempt << "; address_count=" << routes.size()
                    << "; target_ipv4=" << route.address << "; local_ipv4=" << route.local_address
                    << "; interface=" << route.interface_index
                    << "; routing=" << (route.interface_index ? "discovery_bound" : "system")
                    << "; port=" << outgoing->peer.port << "; result=" << result_name
                    << "; errno=" << system_error
                    << "; elapsed_ms=" << (base::TimeTicks::Now() - attempt_started).InMilliseconds();
        };
        report_attempt("started", 0);
        const auto remaining = until - base::TimeTicks::Now();
        const auto budget = base::Milliseconds(std::clamp<int64_t>(
            remaining.InMilliseconds() / static_cast<int64_t>(routes.size() - attempt + 1), 100, 3000));
        const auto attempt_until = std::min(until, base::TimeTicks::Now() + budget);
        auto result = ConnectTcpChannel(route.address, outgoing->peer.port, attempt_until,
            [&session] { return session.Active(); }, route.local_address, route.interface_index);
        report_attempt(result.outcome, result.system_error);
        session.channel = std::move(result.channel);
        connected = session.channel != nullptr;
        if (connected) break;
      }
      if (!connected && session.Active()) session.Fail("Unable to connect to any advertised address", Failure::kConnection);
    }
    bool success = prepared && connected && session.Active() && (outgoing ? session.SendFiles(prepared_files, prepared_intro, prepared_total) : session.ReceiveFiles());
    session.LogUpgradeTraffic(success);
    session.FinishJournal(success);
    // Closing the connection may encounter further frames or socket errors.
    // Preserve the transfer outcome so cleanup cannot hide the original cause.
    const std::string transfer_error = session.error;
    const Failure transfer_failure = session.failure;
    const std::string transfer_diagnostic = base::StringPrintf(
        "Quick Share session=%llu; outgoing=%d; stage=%s; reason=%s",
        static_cast<unsigned long long>(session.diagnostic_session), session.outgoing,
        session.diagnostic_stage, transfer_error.c_str());
    const Phase result_phase = success ? (session.confirmed_delivery ? Phase::kComplete : Phase::kSent) : (cancel || session.peer_cancelled) ? Phase::kCancelled : session.timed_out ? Phase::kTimedOut : session.rejected ? Phase::kRejected : Phase::kError;
    if (!success && !cancel && !session.rejected && !session.empty_incoming_connection)
      LOG(WARNING) << "crQuickShare: " << transfer_error
                   << "; incoming=" << !session.outgoing
                   << "; offer_received=" << session.offer_received
                   << "; accepted=" << session.accepted
                   << "; expected_sharing=" << static_cast<int>(session.expected_sharing);
    LOG(INFO) << "crQuickShare: Connection outcome; session=" << session.diagnostic_session
              << "; outgoing=" << session.outgoing << "; stage=" << session.diagnostic_stage
              << "; success=" << success << "; delivery_confirmed=" << (success && session.confirmed_delivery)
              << "; cancelled=" << (cancel || session.peer_cancelled || stop)
              << "; rejected=" << session.rejected << "; timed_out=" << session.timed_out
              << "; remote_closed=" << session.remote_closed
              << "; empty_incoming_connection=" << session.empty_incoming_connection
              << "; elapsed_ms=" << (base::TimeTicks::Now() - session.diagnostic_started).InMilliseconds();
    session.Stage("disconnect_cleanup");
    session.handshake_deadline = {};
    session.Finish(success);
    if (session.channel) session.channel->Close();
    session.CleanupNetwork();  // Keep admission blocked until explicit cleanup finishes.
    Update([&](Snapshot& s) {
      s.busy = false;
      s.phase = session.empty_incoming_connection ? Phase::kIdle : result_phase;
      if (!success && !session.empty_incoming_connection) { s.diagnostic = transfer_diagnostic; s.failure = transfer_failure; }
    });
    busy = false;
    refresh_advertisement = true;
    LOG(INFO) << "crQuickShare: Session released; receiving=" << receiving.load()
              << "; advertisement_refresh_requested=1";
    if (outgoing && outgoing->finished) outgoing->finished();
    if (bluetooth_incoming && bluetooth_incoming->finished) bluetooth_incoming->finished();
  });
}
void QuickShareService::Impl::Discover() {
  auto result = CreateConnectionListener();
  if (!result.listener) {
    LOG(WARNING) << "crQuickShare: TCP listener setup failed; reason=" << result.error << "; system_error=" << result.system_error;
    Update([](Snapshot& s) { s.phase = Phase::kError; s.diagnostic = "Unable to listen on LAN"; }); return;
  }
  auto listener = std::move(result.listener);
  LOG(INFO) << "crQuickShare: TCP listener ready; port=" << listener->port();
  auto discovery = CreateLanDiscovery(endpoint, endpoint_info, listener->port());
  Update([&](Snapshot& s) { s.available = discovery->ready(); if (!s.available) { s.phase = Phase::kError; s.diagnostic = "mDNS unavailable; check the network and UDP port 5353"; } });
  base::TimeTicks next_listener_diagnostic;
  uint64_t listener_events = 0;
  auto listener_diagnostic = [&](const char* reason, int system_error) {
    ++listener_events;
    const auto now = base::TimeTicks::Now();
    if (now < next_listener_diagnostic) return;
    next_listener_diagnostic = now + base::Seconds(30);
    LOG(INFO) << "crQuickShare: Incoming connection not started; reason=" << reason
              << "; errno=" << system_error << "; cumulative_events=" << listener_events;
  };
  struct PendingConnection {
    std::unique_ptr<TransportChannel> channel;
    std::string address, record;
    base::TimeTicks deadline;
    size_t expected_bytes = 4;
  };
  // Only the first framed connection request is read here. The session worker
  // receives it unchanged and performs the normal handshake validation again.
  std::vector<PendingConnection> pending;
  constexpr size_t kMaxPendingConnections = 8;
  std::array<char, kChunk> pending_bytes;
  while (!stop) {
    std::optional<Request> outgoing;
    {
      std::lock_guard guard(mutex);
      if (receiving && base::TimeTicks::Now() >= visible_until) { receiving = false; state.receiving = false; ++state.revision; }
      if (request) { outgoing = std::move(request); request.reset(); }
    }
    if (outgoing) Start(nullptr, std::move(outgoing));
    if (refresh_discovery.exchange(false)) discovery->RestartQueries();
    if (refresh_advertisement.exchange(false)) discovery->RefreshAdvertisement();
    discovery->Tick(receiving && !busy);
    {
      std::lock_guard guard(mutex);
      endpoint = discovery->local_endpoint_id();
      if (state.available != discovery->ready()) {
        state.available = discovery->ready(); ++state.revision;
        if (state.available && !state.session && state.phase == Phase::kError) {
          state.phase = Phase::kIdle; state.diagnostic.clear(); state.failure = Failure::kNone;
        }
      }
    }
    auto peers = discovery->Peers();
    { std::lock_guard guard(mutex); if (state.peers != peers) { LOG(INFO) << "crQuickShare: Discovery list updated; previous_count=" << state.peers.size() << "; current_count=" << peers.size(); state.peers = std::move(peers); ++state.revision; } }
    if (busy || !receiving || stop) pending.clear();
    auto incoming = listener->Accept(100);
    if (!incoming.channel) {
      if (incoming.error) listener_diagnostic(incoming.error, incoming.system_error);
    } else if (!discovery->IsLocalAddress(incoming.address)) {
      listener_diagnostic("nonlocal_source", 0);
    } else if (!receiving || busy || stop) {
      listener_diagnostic("receiving_disabled_or_busy", 0);
    } else if (pending.size() >= kMaxPendingConnections) {
      listener_diagnostic("prehandshake_capacity", 0);
    } else {
      pending.push_back({std::move(incoming.channel), std::move(incoming.address), {},
                         base::TimeTicks::Now() + base::Seconds(15), 4});
    }
    for (auto it = pending.begin(); it != pending.end();) {
      const char* discard = nullptr;
      int system_error = 0;
      bool complete = false;
      if (stop || busy || !receiving) { pending.clear(); break; }
      if (base::TimeTicks::Now() >= it->deadline) discard = "prehandshake_timeout";
      else if (!discovery->IsLocalAddress(it->address)) discard = "prehandshake_network_changed";
      else {
        // At most one nonblocking read per candidate per discovery iteration.
        // Never consume UKEY2 bytes that follow the initial frame.
        const size_t remaining = std::min(pending_bytes.size(), it->expected_bytes - it->record.size());
        const auto read = it->channel->Read(base::span(pending_bytes).first(remaining));
        system_error = read.system_error;
        if (read.status == TransportChannel::Status::kClosed)
          discard = it->record.empty() ? "prehandshake_empty_close" : "prehandshake_truncated_request";
        else if (read.status == TransportChannel::Status::kError) discard = "prehandshake_read_error";
        else if (read.status == TransportChannel::Status::kOk) {
          if (!read.bytes || read.bytes > remaining) discard = "prehandshake_invalid_read";
          else {
            it->record.append(pending_bytes.data(), read.bytes);
            if (it->expected_bytes == 4 && it->record.size() == 4) {
              uint32_t length = 0;
              for (unsigned char byte : it->record) length = (length << 8) | byte;
              if (!length || length > kMaxRecord) discard = "prehandshake_invalid_length";
              else it->expected_bytes = size_t{4} + length;
            }
            complete = !discard && it->record.size() == it->expected_bytes;
          }
        }
      }
      if (complete) {
        nc::OfflineFrame frame;
        if (!frame.ParseFromString(it->record.substr(4)) || frame.version() != nc::OfflineFrame::V1 ||
            !frame.has_v1() || frame.v1().type() != nc::V1Frame::CONNECTION_REQUEST ||
            !frame.v1().has_connection_request()) discard = "prehandshake_invalid_request";
        else {
          const auto& connection = frame.v1().connection_request();
          if (connection.endpoint_id().size() != 4 ||
              EndpointName(connection.has_endpoint_info() ? connection.endpoint_info() : connection.endpoint_name()).empty())
            discard = "prehandshake_invalid_endpoint";
        }
      }
      if (discard) {
        listener_diagnostic(discard, system_error);
        it = pending.erase(it);
        continue;
      }
      if (!complete) { ++it; continue; }
      bool admitted = false;
      {
        std::lock_guard guard(mutex);
        bool expected = false;
        if (!stop && receiving && busy.compare_exchange_strong(expected, true)) {
          cancel = false; decision = 0; admitted = true;
        }
      }
      if (!admitted) { pending.clear(); break; }
      auto channel = std::move(it->channel);
      auto initial_record = std::move(it->record);
      pending.clear();
      LOG(INFO) << "crQuickShare: Incoming prehandshake accepted; request_bytes=" << initial_record.size();
      Start(std::move(channel), std::nullopt, std::move(initial_record));
      break;
    }
  }
}
QuickShareService::QuickShareService(std::string name, base::FilePath destination, bool receive_text,
                                   TransferJournalFactory journal_factory)
    : impl_(std::make_unique<Impl>(std::move(name), std::move(destination), receive_text, true, std::move(journal_factory))) {}
std::vector<QuickShareService::ReceivedText> QuickShareService::TakeReceivedTexts() {
  std::lock_guard guard(impl_->mutex);
  std::vector<ReceivedText> result;
  result.swap(impl_->received_texts);
  return result;
}
QuickShareService::~QuickShareService() = default;
Snapshot QuickShareService::GetSnapshot() const {
  std::lock_guard guard(impl_->mutex);
  Snapshot snapshot = impl_->state;
  auto& samples = impl_->progress_samples;
  if (snapshot.busy && snapshot.phase == Phase::kTransferring && !samples.empty()) {
    const auto now = base::TimeTicks::Now();
    // Sample cumulative payload bytes, excluding pairing and confirmation time.
    // Keep one baseline before the window, with a bounded sampling frequency.
    while (samples.size() > 1 && now - samples[1].first >= base::Seconds(3)) samples.pop_front();
    const auto elapsed = now - samples.front().first;
    if (elapsed >= base::Milliseconds(500)) {
      snapshot.bytes_per_second = static_cast<uint64_t>(
          (snapshot.transferred - samples.front().second) / elapsed.InSecondsF());
      if (snapshot.bytes_per_second && snapshot.transferred < snapshot.total) {
        const auto remaining = snapshot.total - snapshot.transferred;
        snapshot.remaining_seconds = (remaining + snapshot.bytes_per_second - 1) / snapshot.bytes_per_second;
      }
    }
    if (now - samples.back().first >= base::Milliseconds(200))
      samples.emplace_back(now, snapshot.transferred);
  }
  return snapshot;
}
void QuickShareService::RefreshDiscovery() {
  LOG(INFO) << "crQuickShare: LAN discovery refresh requested";
  impl_->refresh_discovery = true;
}
void QuickShareService::SetReceiving(bool enabled, bool continuous) {
  std::lock_guard guard(impl_->mutex);
  impl_->receiving = enabled && impl_->state.available;
  impl_->visible_until = continuous ? base::TimeTicks::Max() : base::TimeTicks::Now() + base::Minutes(10);
  impl_->state.receiving = impl_->receiving; ++impl_->state.revision;
  impl_->refresh_advertisement = true;
  LOG(INFO) << "crQuickShare: Reception changed; enabled=" << impl_->receiving.load()
            << "; continuous=" << continuous << "; busy=" << impl_->busy.load();
}
bool QuickShareService::StopReceivingIfIdle() {
  std::lock_guard guard(impl_->mutex);
  if (impl_->busy) return false;
  impl_->receiving = false; impl_->state.receiving = false; ++impl_->state.revision;
  return true;
}
bool QuickShareService::Send(const Peer& peer, std::vector<base::FilePath> paths) {
  std::lock_guard guard(impl_->mutex);
  if (peer.ble_only || peer.address.empty() || !peer.port || impl_->busy || paths.empty() || paths.size() > kMaxFiles || !impl_->state.available) return false;
  auto it = std::ranges::find_if(impl_->state.peers, [&](const Peer& candidate) {
    return candidate.id == peer.id && candidate.endpoint_info == peer.endpoint_info &&
           candidate.address == peer.address && candidate.port == peer.port;
  });
  if (it == impl_->state.peers.end()) return false;
  Peer selected = *it;
  if (selected.name.empty()) {
    const auto& info = selected.endpoint_info;
    const auto& public_info = peer.public_name_info;
    const auto public_name = EndpointName(public_info);
    if (info.size() < 17 || public_info.size() < 17 || public_name.empty() ||
        info.compare(1, 16, public_info, 1, 16) != 0 ||
        ((uint8_t(info[0]) ^ uint8_t(public_info[0])) & 0xef) != 0) return false;
    selected.name = public_name;
  }
  bool expected = false; if (!impl_->busy.compare_exchange_strong(expected, true)) return false;
  impl_->cancel = false; impl_->decision = 0;
  ++impl_->state.session;
  impl_->state.incoming = false; impl_->state.peer_name = selected.name;
  impl_->state.peer_device_type = EndpointDeviceType(peer.endpoint_info);
  impl_->state.code.clear(); impl_->state.files.clear();
  impl_->state.transferred = impl_->state.total = 0; impl_->state.diagnostic.clear();
  impl_->state.failure = Failure::kNone;
  impl_->ResetProgress();
  impl_->state.busy = true; impl_->state.phase = Phase::kConnecting; ++impl_->state.revision;
  impl_->request = Impl::Request{std::move(selected), std::move(paths)}; return true;
}
bool QuickShareService::SendBluetooth(const Peer& peer, std::vector<base::FilePath> paths,
                                      std::unique_ptr<TransportChannel> channel,
                                      std::function<uint64_t()> socket_sent_bytes,
                                      HotspotConnector hotspot, std::function<bool()> cancelled,
                                      std::function<void()> finished) {
  std::lock_guard guard(impl_->mutex);
  const auto reject = [](const char* reason) {
    LOG(WARNING) << "crQuickShare: Send rejected before session; component=bluetooth_service; reason=" << reason;
    return false;
  };
  if (!peer.ble_only || peer.id.size() != 4 || EndpointName(peer.endpoint_info).empty() || peer.classic_bluetooth_address.empty())
    return reject("invalid_bluetooth_peer");
  if (!channel || !socket_sent_bytes) return reject("transport_missing");
  if (paths.empty()) return reject("empty_selection");
  if (paths.size() > kMaxFiles) return reject("too_many_files");
  if (!impl_->state.available) return reject("service_unavailable");
  if (impl_->stop) return reject("service_stopping");
  bool expected = false;
  if (!impl_->busy.compare_exchange_strong(expected, true)) return reject("service_busy");
  impl_->cancel = false; impl_->decision = 0;
  ++impl_->state.session;
  impl_->state.incoming = false; impl_->state.peer_name = peer.name;
  impl_->state.peer_device_type = EndpointDeviceType(peer.endpoint_info);
  impl_->state.code.clear(); impl_->state.files.clear();
  impl_->state.transferred = impl_->state.total = 0; impl_->state.diagnostic.clear();
  impl_->state.failure = Failure::kNone; impl_->ResetProgress();
  impl_->state.busy = true; impl_->state.phase = Phase::kConnecting; ++impl_->state.revision;
  impl_->request = Impl::Request{peer, std::move(paths), std::move(channel), std::move(socket_sent_bytes),
                                std::move(hotspot), std::move(cancelled), std::move(finished)};
  return true;
}
Peer QuickShareService::LocalPeer() const {
  std::lock_guard guard(impl_->mutex);
  Peer peer;
  peer.id = impl_->endpoint; peer.name = impl_->name; peer.endpoint_info = impl_->endpoint_info;
  return peer;
}
void QuickShareService::SetWifiDirectClientConnector(WifiDirectClientConnector connector) {
  std::lock_guard guard(impl_->mutex);
  if (!impl_->busy) impl_->direct_client_connector = std::move(connector);
}
void QuickShareService::SetPreferHotspotUpgrade(bool prefer_hotspot) {
  std::lock_guard guard(impl_->mutex);
  if (!impl_->busy) impl_->prefer_hotspot_upgrade = prefer_hotspot;
}
void QuickShareService::SetWifiDirectGoFactory(WifiDirectGoFactory factory, bool prefer_direct) {
  std::lock_guard guard(impl_->mutex);
  if (!impl_->busy) {
    impl_->direct_go_factory = std::move(factory);
    impl_->prefer_direct_go = prefer_direct;
  }
}
bool QuickShareService::ReceiveBluetooth(std::unique_ptr<TransportChannel> channel,
                                        std::function<uint64_t()> socket_sent_bytes,
                                        std::function<bool()> cancelled,
                                        std::function<void()> finished, HotspotConnector hotspot) {
  std::lock_guard guard(impl_->mutex);
  if (!channel || !socket_sent_bytes || impl_->stop || !impl_->receiving ||
      base::TimeTicks::Now() >= impl_->visible_until) return false;
  bool expected = false;
  if (!impl_->busy.compare_exchange_strong(expected, true)) return false;
  impl_->cancel = false; impl_->decision = 0;
  ++impl_->state.session;
  impl_->state.incoming = true; impl_->state.busy = true;
  impl_->state.phase = Phase::kConnecting;
  impl_->state.peer_name.clear(); impl_->state.code.clear(); impl_->state.files.clear();
  impl_->state.peer_device_type = 0; impl_->state.text_count = 0; impl_->state.text_is_url = false;
  impl_->state.transferred = impl_->state.total = 0; impl_->state.diagnostic.clear();
  impl_->state.received_directory.clear(); impl_->state.failure = Failure::kNone;
  impl_->ResetProgress(); ++impl_->state.revision;
  impl_->request = Impl::Request{{}, {}, std::move(channel), std::move(socket_sent_bytes),
                                std::move(hotspot), std::move(cancelled), std::move(finished), true};
  LOG(INFO) << "crQuickShare: Bluetooth incoming session admitted; session=" << impl_->state.session;
  return true;
}
void QuickShareService::Confirm(bool accept) {
  std::lock_guard guard(impl_->mutex);
  if (impl_->state.phase == Phase::kConfirm && impl_->decision == 0) impl_->decision = accept ? 1 : -1;
}
void QuickShareService::Confirm(uint64_t session, bool accept) {
  std::lock_guard guard(impl_->mutex);
  if (impl_->state.session == session && impl_->state.busy &&
      impl_->state.phase == Phase::kConfirm && impl_->decision == 0)
    impl_->decision = accept ? 1 : -1;
}
void QuickShareService::Cancel(uint64_t session) {
  std::lock_guard guard(impl_->mutex);
  if (impl_->state.session == session && impl_->state.busy) impl_->cancel = true;
}
void QuickShareService::Cancel() { impl_->cancel = true; }
}
