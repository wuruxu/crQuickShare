// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/quick_share_service.h"
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
    progress_samples.clear();
  }
  std::atomic<bool> stop{false}, cancel{false}, busy{false}, receiving{false};
  std::atomic<int> decision{0};
  std::atomic<bool> refresh_discovery{false}, refresh_advertisement{false};
  std::thread discovery_thread, session_thread;
  std::string name, endpoint, endpoint_info;
  base::FilePath destination;
  struct Request { Peer peer; std::vector<base::FilePath> paths; };
  std::optional<Request> request;
  base::TimeTicks visible_until;
  struct Session;
  Impl(std::string n, base::FilePath d) : name(std::move(n)), destination(std::move(d)) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int i = 0; i < 4; ++i) endpoint += alphabet[base::RandGenerator(36)];
    endpoint_info = MakeEndpointInfo(name);
    discovery_thread = std::thread([this] { Discover(); });
  }
  ~Impl() {
    stop = true; cancel = true;
    if (discovery_thread.joinable()) discovery_thread.join();
    if (session_thread.joinable()) session_thread.join();
  }
  template<class F> void Update(F f) { std::lock_guard guard(mutex); f(state); ++state.revision; }
  void PhaseChanged(Phase phase) {
    Update([&](Snapshot& s) {
      s.phase = phase;
      if (phase == Phase::kTransferring) {
        progress_samples.clear();
        progress_samples.emplace_back(base::TimeTicks::Now(), s.transferred);
      }
    });
  }
  void Discover();
  void Start(std::unique_ptr<TransportChannel> transport, std::optional<Request> outgoing);
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
  base::TimeTicks upgrade_retry_window;
  qs::V1Frame::FrameType expected_sharing = qs::V1Frame::PAIRED_KEY_ENCRYPTION;
  base::TimeTicks finish_deadline, response_deadline;
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
  bool Active() const { return !failed && !owner.stop && !owner.cancel && !peer_cancelled && !remote_closed && !rejected && !timed_out; }
  bool IoActive() const {
    return !wire_broken && !owner.stop && (finishing ? base::TimeTicks::Now() < finish_deadline : Active());
  }
  bool WireFail(const char* message, int system_error = 0, Failure category = Failure::kNetwork) {
    if (system_error) LOG(WARNING) << "crQuickShare: Transport error; session=" << diagnostic_session
        << "; medium=" << (channel ? channel->name() : "unconnected")
        << "; stage=" << diagnostic_stage << "; errno=" << system_error;
    wire_broken = true; return Fail(message, category);
  }
  bool SupportsAck() const { return peer_disconnect_version >= 6; }
  bool SupportsSafeDisconnect() const { return peer_disconnect_version >= 1; }
  int64_t NewId() {
    int64_t id;
    do { id = PayloadId(); } while (received_payloads.contains(id) || byte_sizes.contains(id) || !local_ids.insert(id).second);
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
    while (Active() && sent < bytes.size() && base::TimeTicks::Now() < deadline) {
      const auto result = channel->Write(base::span<const char>(std::string_view(bytes).substr(sent)));
      if (result.status == TransportChannel::Status::kOk && result.bytes) { sent += result.bytes; continue; }
      if (result.status == TransportChannel::Status::kWouldBlock) {
        if (!WaitWritable(100)) return false;
        continue;
      }
      return WireFail("Transport write failed", result.system_error);
    }
    if (sent != bytes.size()) return WireFail("Transport write cancelled or timed out");
    return true;
  }
  bool WaitWritable(int wait_ms) {
    const auto ready = channel->Wait(false, true, wait_ms);
    if (ready.system_error) return WireFail("Transport wait failed", ready.system_error);
    return true;
  }
  bool PumpOutput() {
    size_t budget = 256 * 1024;
    while (IoActive() && !writes.empty() && budget) {
      auto& w = writes.front();
      auto remaining = std::string_view(w.bytes).substr(w.offset);
      const auto result = channel->Write(base::span<const char>(remaining.substr(0, budget)));
      if (result.status == TransportChannel::Status::kWouldBlock) break;
      if (result.status != TransportChannel::Status::kOk || !result.bytes)
        return WireFail("Transport write failed", result.system_error);
      w.offset += result.bytes; budget -= result.bytes; write_progress = base::TimeTicks::Now();
      if (w.offset == w.bytes.size()) {
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
      out = buffer.substr(4, n); buffer.erase(0, n + 4); return 1;
    };
    if (!IoActive()) return -1;
    int result = extract(); if (result) return result;
    const auto ready = channel->Wait(true, !writes.empty(), wait_ms);
    if (ready.system_error) { WireFail("Transport wait failed", ready.system_error); return -1; }
    if (ready.writable && !PumpOutput()) return -1;
    if (!ready.readable) return 0;
    std::array<char, kChunk> bytes;
    const auto read = channel->Read(base::span(bytes));
    if (read.status == TransportChannel::Status::kClosed) {
      LOG(INFO) << "crQuickShare: Peer transport closed; medium=" << channel->name()
                << "; session=" << diagnostic_session << "; stage=" << diagnostic_stage;
      remote_closed = true; wire_broken = true; return -1;
    }
    if (read.status == TransportChannel::Status::kWouldBlock) return 0;
    if (read.status != TransportChannel::Status::kOk || !read.bytes) {
      WireFail("Transport read failed", read.system_error); return -1;
    }
    buffer.append(bytes.data(), read.bytes); return extract();
  }
  bool AwaitRaw(std::string& out) {
    auto deadline = base::TimeTicks::Now() + base::Seconds(15);
    while (Active() && base::TimeTicks::Now() < deadline) { int r = ReadRaw(out, 100); if (r) return r > 0; }
    if (!Active()) return false;
    timed_out = true; return Fail("Handshake timed out");
  }
  bool Write(const nc::OfflineFrame& frame) {
    if (!crypto) return WriteRaw(frame.SerializeAsString());
    if (!IoActive()) return false;
    auto encoded = crypto->EncodeMessageToPeer(frame.SerializeAsString());
    if (!encoded || encoded->empty() || encoded->size() > kMaxRecord || queued_bytes + encoded->size() + 4 > 2 * kMaxRecord)
      return WireFail("Encryption failed or output queue exhausted", 0, Failure::kProtocol);
    std::string record(4, '\0');
    for (int i = 0; i < 4; ++i) record[i] = char(encoded->size() >> (24 - 8 * i));
    record += *encoded;
    if (writes.empty()) write_progress = base::TimeTicks::Now();
    queued_bytes += record.size();
    PendingWrite pending; pending.bytes = std::move(record); writes.push_back(std::move(pending));
    return true;
  }
  bool TickConnection() {
    if (!IoActive()) return false;
    auto now = base::TimeTicks::Now();
    if (crypto && !finishing) {
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
    if (crypto) {
      auto decoded = crypto->DecodeMessageFromPeer(data);
      if (!decoded) {
        nc::OfflineFrame early;
        if (allow_initial_plain_keepalive && early.ParseFromString(data) &&
            early.version() == nc::OfflineFrame::V1 && early.has_v1() &&
            early.v1().type() == nc::V1Frame::KEEP_ALIVE && early.v1().has_keep_alive()) {
          allow_initial_plain_keepalive = false; return 0;
        }
        WireFail("Invalid encrypted frame or sequence", 0, Failure::kProtocol); return -1;
      }
      allow_initial_plain_keepalive = false; data = *decoded;
    }
    if (!frame.ParseFromString(data) || frame.version() != nc::OfflineFrame::V1 || !frame.has_v1()) { WireFail("Invalid Nearby frame", 0, Failure::kProtocol); return -1; }
    if (frame.v1().type() == nc::V1Frame::PAYLOAD_TRANSFER && !ValidatePayload(frame)) return -1;
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
      if (!v.keep_alive().ack()) {
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
      if (retry.is_request()) {
        // This frame exchanges available media; it does not approve a file
        // transfer or require replacing the current encrypted LAN channel.
        auto response = Frame(nc::V1Frame::BANDWIDTH_UPGRADE_RETRY);
        auto* reply = response.mutable_v1()->mutable_bandwidth_upgrade_retry();
        reply->set_is_request(false);
        reply->add_supported_medium(nc::BandwidthUpgradeRetryFrame::WIFI_LAN);
        if (!Write(response)) return -1;
      }
      if (upgrade_retry_count == 1)
        LOG(WARNING) << "crQuickShare: Bandwidth upgrade retry handled; request="
                     << retry.is_request() << "; retaining WIFI_LAN";
      return 0;
    }
    if (v.type() == nc::V1Frame::BANDWIDTH_UPGRADE_NEGOTIATION) {
      if (!v.has_bandwidth_upgrade_negotiation()) { Fail("Missing upgrade fields"); return -1; }
      if (v.bandwidth_upgrade_negotiation().event_type() == nc::BandwidthUpgradeNegotiationFrame::UPGRADE_PATH_AVAILABLE) {
        auto response = Frame(nc::V1Frame::BANDWIDTH_UPGRADE_NEGOTIATION);
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
    Stage("connection_request");
    if (outgoing) {
      auto f = Frame(nc::V1Frame::CONNECTION_REQUEST);
      auto* r = f.mutable_v1()->mutable_connection_request();
      { std::lock_guard guard(owner.mutex); r->set_endpoint_id(owner.endpoint); }
      r->set_endpoint_name(owner.endpoint_info);
      r->set_endpoint_info(owner.endpoint_info); r->set_nonce(base::RandUint64() & INT32_MAX);
      r->add_mediums(nc::ConnectionRequestFrame::WIFI_LAN);
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
      ConfigureKeepalive(r.has_keep_alive_interval_millis() ? r.keep_alive_interval_millis() : 5000,
                         r.has_keep_alive_timeout_millis() ? r.keep_alive_timeout_millis() : 30000);
      auto peer_name = EndpointName(r.has_endpoint_info() ? r.endpoint_info() : r.endpoint_name());
      if (peer_name.empty() || r.endpoint_id().size() != 4) return Fail("Invalid endpoint information");
      const auto& peer_info = r.has_endpoint_info() ? r.endpoint_info() : r.endpoint_name();
      owner.Update([&](Snapshot& s) { s.peer_name = peer_name; s.peer_device_type = (static_cast<uint8_t>(peer_info[0]) >> 1) & 7; });
    }
    Stage("ukey2");
    using Handshake = securegcm::UKey2Handshake;
    auto handshake = outgoing ? Handshake::ForInitiator(Handshake::HandshakeCipher::P256_SHA512) : Handshake::ForResponder(Handshake::HandshakeCipher::P256_SHA512);
    if (!handshake) return Fail("Unable to initialize UKEY2");
    auto send_next = [&] { auto bytes = handshake->GetNextHandshakeMessage(); return bytes && WriteRaw(*bytes); };
    auto receive_next = [&] { std::string bytes; return AwaitRaw(bytes) && handshake->ParseHandshakeMessage(bytes).success; };
    if (outgoing ? !(send_next() && receive_next() && send_next()) : !(receive_next() && send_next() && receive_next())) return Fail("UKEY2 handshake failed");
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
  bool Ack(int64_t id) {
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
      if (it == sent_payloads.end() || !it->second.last_sent || it->second.offset != it->second.size)
        return Fail("Acknowledgement does not match a completed outgoing payload");
      if (it->second.acknowledged && ++duplicate_messages > 16)
        return Fail("Too many duplicate messages");
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
    if (!SendSharing(f)) return false;
    if (status != qs::ConnectionResponseFrame::ACCEPT) terminal_sent = true;
    return true;
  }
  struct File { int64_t id = 0, size = 0, offset = 0; uint64_t index = 0; std::string name, parent; base::File handle; bool complete = false; };
  bool SendFiles(const Request& send_request) {
    std::vector<File> files; int64_t total = 0;
    if (send_request.paths.empty() || send_request.paths.size() > kMaxFiles) return Fail("Select between one and one hundred files or directories", Failure::kUnsupported);
    auto intro = Sharing(qs::V1Frame::INTRODUCTION);
    intro.mutable_v1()->mutable_introduction()->set_start_transfer(true);
    SourceSelection selection; StorageError storage_error;
    if (!storage->Collect(send_request.paths, [this] { return Active(); }, selection, storage_error))
      return storage_error.message.empty() ? false : Fail(storage_error.message, storage_error.failure);
    total = selection.total;
    for (auto& source : selection.files) {
      File f; f.id = NewId(); f.index = files.size() + 1; f.size = source.size;
      f.name = std::move(source.name); f.parent = std::move(source.parent); f.handle = std::move(source.handle);
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
    LOG(INFO) << "crQuickShare: Source selection collected; files=" << files.size()
              << "; entries=" << selection.entries << "; files_with_parent="
              << std::count_if(files.begin(), files.end(), [](const File& f) { return !f.parent.empty(); });
    owner.Update([&](Snapshot& s) { s.total = total; for (auto& f : files) s.files.push_back(RelativeName(f.parent, f.name)); });
    if (!Handshake(true) || !Pair()) return false;
    expected_sharing = qs::V1Frame::RESPONSE; offer_sent = true;
    response_deadline = base::TimeTicks::Now() + base::Seconds(60);
    if (!SendSharing(intro) || !Confirm()) return false;
    owner.PhaseChanged(Phase::kWaiting);
    qs::Frame response;
    if (!AwaitSharing(response, qs::V1Frame::RESPONSE)) return false;
    if (!response.v1().has_connection_response() || response.v1().connection_response().status() != qs::ConnectionResponseFrame::ACCEPT) { rejected = true; return false; }
    RetireSentControls();
    accepted = true; expected_sharing = qs::V1Frame::UNKNOWN_FRAME_TYPE;
    LOG(WARNING) << "crQuickShare: Sending files; count=" << files.size()
                 << "; total_bytes=" << total << "; peer_disconnect_version=" << peer_disconnect_version
                 << "; start_transfer=1; indexed_chunks=1";
    Stage("file_transfer");
    owner.PhaseChanged(Phase::kTransferring);
    for (auto& f : files) {
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
      if (!Active() || !Chunk(f.id, Header::FILE, f.size, f.size, {}, true) || !Flush()) return SenderFinishedAfterClose();
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
  bool ReceiveFiles() {
    if (!Handshake(false) || !Pair()) return false;
    qs::Frame intro;
    if (!AwaitSharing(intro, qs::V1Frame::INTRODUCTION) || !intro.v1().has_introduction()) return false;
    const auto& offer = intro.v1().introduction();
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
    auto batch = storage->CreateBatch(owner.destination, total, paths, storage_error);
    if (!batch) {
      if (storage_error.no_space) Response(qs::ConnectionResponseFrame::NOT_ENOUGH_SPACE);
      return Fail(storage_error.message, storage_error.failure);
    }
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
        if (completed != files.size() && (!Ack(f.id) || !Flush())) return false;
      }
    }
    if (!Active()) return false;
    if (completed != files.size()) { timed_out = true; return Fail("File transfer stalled"); }
    if (!batch->Commit(storage_error)) return Fail(storage_error.message, storage_error.failure);
    batch_committed = true;
    if (!batch->directory_durable())
      LOG(WARNING) << "crQuickShare: Receive batch saved but directory durability is unconfirmed; session=" << diagnostic_session;
    LOG(INFO) << "crQuickShare: Receive batch committed; session=" << diagnostic_session << "; files=" << completed;
    owner.Update([&](Snapshot& s) { s.received_directory = batch->saved_directory(); });
    if (!Ack(last_completed_id))
      LOG(WARNING) << "crQuickShare: Final payload ACK could not be queued after commit; session=" << diagnostic_session;
    // Files are received even if the peer disconnects during the final ACK.
    return true;
  }
  void Finish(bool success) {
    if (!crypto || wire_broken || owner.stop) return;
    finishing = true;
    // Cleanup has its own bounded IO budget; do not clear the user's cancel
    // flag, and never append ciphertext after a truncated/invalid record.
    finish_deadline = base::TimeTicks::Now() + base::Seconds(2);
    if (!success && !terminal_sent && !terminal_received && !remote_closed) {
      if (accepted) {
        auto notify = [&](int64_t id, int64_t size, int64_t offset) {
          auto f = Frame(nc::V1Frame::PAYLOAD_TRANSFER);
          auto* p = f.mutable_v1()->mutable_payload_transfer();
          p->set_packet_type(nc::PayloadTransferFrame::CONTROL);
          auto* h = p->mutable_payload_header(); h->set_id(id); h->set_type(Header::FILE); h->set_total_size(size);
          auto* c = p->mutable_control_message(); c->set_offset(offset);
          c->set_event(owner.cancel || rejected ? nc::PayloadTransferFrame::ControlMessage::PAYLOAD_CANCELED : nc::PayloadTransferFrame::ControlMessage::PAYLOAD_ERROR);
          return Write(f);
        };
        for (const auto& [id, p] : sent_payloads) if (!p.acknowledged && !notify(id, p.size, p.offset)) break;
        for (const auto& [id, p] : received_payloads) if (!batch_committed && !notify(id, p.first, p.second)) break;
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
void QuickShareService::Impl::Start(std::unique_ptr<TransportChannel> transport, std::optional<Request> outgoing) {
  if (session_thread.joinable()) session_thread.join();
  Update([&](Snapshot& s) {
    if (!outgoing) ++s.session;
    s.busy = true; s.incoming = !outgoing; s.phase = Phase::kConnecting;
    s.peer_name = outgoing ? outgoing->peer.name : std::string(); s.code.clear(); s.files.clear();
    s.peer_device_type = outgoing && !outgoing->peer.endpoint_info.empty() ?
        (static_cast<uint8_t>(outgoing->peer.endpoint_info[0]) >> 1) & 7 : 0;
    s.transferred = s.total = 0; s.diagnostic.clear(); s.received_directory.clear();
    s.failure = Failure::kNone;
    ResetProgress();
  });
  session_thread = std::thread([this, transport = std::move(transport), outgoing = std::move(outgoing)]() mutable {
    Session session(*this, std::move(transport));
    session.outgoing = outgoing.has_value();
    session.Stage(outgoing ? "tcp_connect" : "tcp_accepted");
    bool connected = session.channel != nullptr;
    if (outgoing) {
      LOG(INFO) << "crQuickShare: Selected discovery record; session=" << session.diagnostic_session
                << "; cache_id=" << outgoing->peer.discovery_cache_id << "; interface=" << outgoing->peer.discovery_interface;
      connected = false;
      auto addresses = outgoing->peer.alternate_addresses;
      addresses.insert(addresses.begin(), outgoing->peer.address);
      auto until = base::TimeTicks::Now() + base::Seconds(10);
      unsigned attempt = 0;
      for (const auto& ip : addresses) {
        if (!session.Active() || base::TimeTicks::Now() >= until) break;
        ++attempt;
        const auto attempt_started = base::TimeTicks::Now();
        auto report_attempt = [&](const char* result_name, int system_error) {
          LOG(INFO) << "crQuickShare: TCP attempt; session=" << session.diagnostic_session
                    << "; address_index=" << attempt << "; address_count=" << addresses.size()
                    << "; port=" << outgoing->peer.port << "; result=" << result_name
                    << "; errno=" << system_error
                    << "; elapsed_ms=" << (base::TimeTicks::Now() - attempt_started).InMilliseconds();
        };
        report_attempt("started", 0);
        const auto attempt_until = std::min(until, base::TimeTicks::Now() + base::Seconds(3));
        auto result = ConnectTcpChannel(ip, outgoing->peer.port, attempt_until,
            [&session] { return session.Active(); });
        report_attempt(result.outcome, result.system_error);
        session.channel = std::move(result.channel);
        connected = session.channel != nullptr;
        if (connected) break;
      }
      if (!connected && session.Active()) session.Fail("Unable to connect to any advertised address", Failure::kConnection);
    }
    bool success = connected && session.Active() && (outgoing ? session.SendFiles(*outgoing) : session.ReceiveFiles());
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
    session.Finish(success);
    if (session.channel) session.channel->Close();
    Update([&](Snapshot& s) {
      s.busy = false;
      s.phase = session.empty_incoming_connection ? Phase::kIdle : result_phase;
      if (!success && !session.empty_incoming_connection) { s.diagnostic = transfer_diagnostic; s.failure = transfer_failure; }
    });
    busy = false;
    refresh_advertisement = true;
    LOG(INFO) << "crQuickShare: Session released; receiving=" << receiving.load()
              << "; advertisement_refresh_requested=1";
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
    auto incoming = listener->Accept(100);
    if (!incoming.channel) {
      if (incoming.error) listener_diagnostic(incoming.error, incoming.system_error);
      continue;
    }
    if (!discovery->IsLocalAddress(incoming.address)) { listener_diagnostic("nonlocal_source", 0); continue; }
    {
      std::lock_guard guard(mutex);
      if (!receiving) { listener_diagnostic("receiving_disabled", 0); continue; }
      bool expected = false;
      if (!busy.compare_exchange_strong(expected, true)) { listener_diagnostic("busy", 0); continue; }
      cancel = false; decision = 0;
    }
    Start(std::move(incoming.channel), std::nullopt);
  }
}
QuickShareService::QuickShareService(std::string name, base::FilePath destination)
    : impl_(std::make_unique<Impl>(std::move(name), std::move(destination))) {}
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
  auto it = std::find(impl_->state.peers.begin(), impl_->state.peers.end(), peer);
  if (it == impl_->state.peers.end()) return false;
  bool expected = false; if (!impl_->busy.compare_exchange_strong(expected, true)) return false;
  impl_->cancel = false; impl_->decision = 0;
  ++impl_->state.session;
  impl_->state.incoming = false; impl_->state.peer_name = peer.name;
  impl_->state.peer_device_type = peer.endpoint_info.empty() ? 0 : (static_cast<uint8_t>(peer.endpoint_info[0]) >> 1) & 7;
  impl_->state.code.clear(); impl_->state.files.clear();
  impl_->state.transferred = impl_->state.total = 0; impl_->state.diagnostic.clear();
  impl_->state.failure = Failure::kNone;
  impl_->ResetProgress();
  impl_->state.busy = true; impl_->state.phase = Phase::kConnecting; ++impl_->state.revision;
  impl_->request = Impl::Request{peer, std::move(paths)}; return true;
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
