// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_QUICK_SHARE_SERVICE_H_
#define CRQUICKSHARE_QUICK_SHARE_SERVICE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "base/files/file_path.h"
#include "crQuickShare/hotspot_upgrade.h"
#include "crQuickShare/wifi_direct_go.h"

namespace crquickshare {
// Optional host journal for file transfers. All methods and destruction run on
// the session worker. An installed factory must return a valid record before IO;
// returning null aborts the transfer. Hosts without a journal need no factory.
class TransferJournal {
 public:
  virtual ~TransferJournal() = default;
  virtual void Finish(bool success, bool cancelled,
                      const base::FilePath& saved_path) = 0;
};
using TransferJournalFactory = std::function<std::unique_ptr<TransferJournal>(
    bool incoming, const std::string& peer, const base::FilePath& local_path,
    const std::string& remote_path)>;
class TransportChannel;
struct LanRoute {
  std::string address, local_address;
  unsigned interface_index = 0;
  bool operator==(const LanRoute&) const = default;
};
struct Peer {
  std::string id;
  std::string name;
  std::string address;
  uint16_t port = 0;
  std::vector<std::string> alternate_addresses;
  std::vector<LanRoute> routes;  // Each address retains its discovery interface and source IPv4.
  std::string endpoint_info;  // Untrusted discovery identity, used only to deduplicate.
  std::string classic_bluetooth_address;  // Only the explicit connection-advertisement MAC.
  std::string public_name_info;  // Matching public BLE endpoint info; display hint only.
  std::string bluetooth_address;  // Display only; never a transport address or identity.
  bool ble_only = false;  // No LAN address; use SendBluetooth(), not Send().
  // Local diagnostics only, never serialized or used for authentication.
  uint64_t discovery_cache_id = 0;
  unsigned discovery_interface = 0;
  bool operator==(const Peer&) const = default;
};
enum class Phase { kIdle, kConnecting, kConfirm, kWaiting, kTransferring,
                   kRestoringNetwork, kComplete, kSent, kRejected, kCancelled, kTimedOut, kError };
enum class Failure { kNone, kConnection, kNetwork, kStorage, kSourceFile,
                     kUnsupported, kProtocol, kHotspotUnavailable };
struct Snapshot {
  std::vector<Peer> peers;
  Phase phase = Phase::kIdle;
  bool receiving = false;
  bool available = false;
  bool busy = false;
  bool incoming = false;
  uint64_t revision = 0;
  uint64_t session = 0;
  uint64_t transferred = 0;
  uint64_t total = 0;
  std::string current_file;
  uint64_t current_file_index = 0;  // One-based introduction order; zero before transfer.
  uint64_t current_file_transferred = 0;
  uint64_t current_file_total = 0;
  uint64_t completed_files = 0;  // Payloads finished, not a remote delivery receipt.
  uint64_t bytes_per_second = 0;
  int64_t remaining_seconds = -1;  // Unknown until a speed estimate is available.
  std::string peer_name;
  uint8_t peer_device_type = 0;
  std::string code;
  std::vector<std::string> files;
  uint32_t text_count = 0;
  bool text_is_url = false;
  // Diagnostic only; UI presents a localized error, details go to the log.
  std::string diagnostic;
  Failure failure = Failure::kNone;
  bool network_cleanup_failed = false;
  base::FilePath received_directory;
};
class QuickShareService {
 public:
  struct ReceivedText { uint64_t session; std::string text; };
  QuickShareService(std::string name, base::FilePath destination, bool receive_text = false,
                    TransferJournalFactory journal_factory = {});
  ~QuickShareService();
  Snapshot GetSnapshot() const;
  // Blocking worker-only experiment: handshake, anonymous pairing, optional
  // hotspot channel migration, then close.
  // Does not start discovery or introduce attachments. An explicitly supplied
  // hotspot connector enables real network/channel migration and rollback.
  // socket_sent_bytes returns a thread-safe cumulative count of completed
  // socket writes on this channel, starting at zero (not queued bytes).
  static bool ProbeBootstrap(std::unique_ptr<TransportChannel> channel,
                             std::function<uint64_t()> socket_sent_bytes,
                             HotspotConnector hotspot = {},
                             std::function<bool()> cancelled = {});
  // Consumes completed, approved UTF-8 text batches once. Clipboard access
  // belongs to the embedding application's UI thread, not the session worker.
  std::vector<ReceivedText> TakeReceivedTexts();
  // Default visibility lasts ten minutes; an embedding app can opt into
  // continuous reception for the lifetime of its enabled service.
  void SetReceiving(bool enabled, bool continuous = false);
  // Atomically prevents new incoming sessions when idle, for reconfiguration.
  bool StopReceivingIfIdle();
  void RefreshDiscovery();
  // Configure only while idle. GO offers and peer-provided client paths are separate roles.
  // A client preference requests Direct alone once before the usual candidate
  // list. A valid peer offer can still choose another available medium.
  void SetWifiDirectClientConnector(WifiDirectClientConnector connector,
                                    bool prefer_direct = false);
  void SetWifiDirectGoFactory(WifiDirectGoFactory factory, bool prefer_direct = false);
  // Idle-only diagnostic preference for outgoing Bluetooth upgrade requests.
  // Requires a hotspot connector; does not override direct LAN connections.
  void SetPreferHotspotUpgrade(bool prefer_hotspot);
  bool Send(const Peer& peer, std::vector<base::FilePath> paths);
  // UI-selected BLE peer. The UI bridge owns the Bluetooth socket; the worker
  // owns its channel and runs preflight, handshake, upgrade and normal Send.
  // finished runs on the session worker after network cleanup and busy release;
  // it must not access UI objects directly. Only called for an accepted request.
  bool SendBluetooth(const Peer& peer, std::vector<base::FilePath> paths,
                     std::unique_ptr<TransportChannel> channel,
                     std::function<uint64_t()> socket_sent_bytes,
                     HotspotConnector hotspot, std::function<bool()> cancelled,
                     std::function<void()> finished = {});
  // UI-owned Bluetooth listener supplies an already accepted socket bridge.
  // Admission is atomic with LAN/outgoing sessions; confirmation is still
  // required before file creation. Larger batches require a hotspot connector.
  bool ReceiveBluetooth(std::unique_ptr<TransportChannel> channel,
                        std::function<uint64_t()> socket_sent_bytes,
                        std::function<bool()> cancelled,
                        std::function<void()> finished, HotspotConnector hotspot = {});
  // Share the LAN identity with the Bluetooth receiver advertisement.
  Peer LocalPeer() const;
  void Confirm(bool accept);
  // Ignore stale UI decisions from a different transfer.
  void Confirm(uint64_t session, bool accept);
  void Cancel(uint64_t session);
  void Cancel();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace crquickshare
#endif
