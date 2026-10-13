// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_QUICK_SHARE_SERVICE_H_
#define CRQUICKSHARE_QUICK_SHARE_SERVICE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "base/files/file_path.h"

namespace crquickshare {
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
  std::string public_name_info;  // Matching public BLE endpoint info; display hint only.
  std::string bluetooth_address;  // Display only; never a transport address or identity.
  bool ble_only = false;  // No LAN address; must never be passed to Send().
  // Local diagnostics only, never serialized or used for authentication.
  uint64_t discovery_cache_id = 0;
  unsigned discovery_interface = 0;
  bool operator==(const Peer&) const = default;
};
enum class Phase { kIdle, kConnecting, kConfirm, kWaiting, kTransferring,
                   kComplete, kSent, kRejected, kCancelled, kTimedOut, kError };
enum class Failure { kNone, kConnection, kNetwork, kStorage, kSourceFile,
                     kUnsupported, kProtocol };
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
  base::FilePath received_directory;
};
class QuickShareService {
 public:
  struct ReceivedText { uint64_t session; std::string text; };
  QuickShareService(std::string name, base::FilePath destination, bool receive_text = false);
  ~QuickShareService();
  Snapshot GetSnapshot() const;
  // Consumes completed, approved UTF-8 text batches once. Clipboard access
  // belongs to the embedding application's UI thread, not the session worker.
  std::vector<ReceivedText> TakeReceivedTexts();
  // Default visibility lasts ten minutes; an embedding app can opt into
  // continuous reception for the lifetime of its enabled service.
  void SetReceiving(bool enabled, bool continuous = false);
  // Atomically prevents new incoming sessions when idle, for reconfiguration.
  bool StopReceivingIfIdle();
  void RefreshDiscovery();
  bool Send(const Peer& peer, std::vector<base::FilePath> paths);
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
