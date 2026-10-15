// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_TRANSPORT_CHANNEL_H_
#define CRQUICKSHARE_TRANSPORT_CHANNEL_H_
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "base/containers/span.h"
#include "base/time/time.h"

namespace crquickshare {
// Ordered, reliable byte stream owned by one session worker. Implementations
// handle OS errors; framing, encryption, deadlines and cancellation belong to
// the session. Read/Write never block and may complete only part of a buffer.
class TransportChannel {
 public:
  enum class Status { kOk, kWouldBlock, kClosed, kError };
  struct Result {
    Status status = Status::kWouldBlock;
    size_t bytes = 0;
    int system_error = 0;
  };
  struct Ready {
    bool readable = false, writable = false;
    int system_error = 0;
  };
  virtual ~TransportChannel();
  struct SetupResult {
    bool connected = true;
    bool timed_out = false;
    std::string error;
  };
  // Called after source preflight, before starting protocol handshake timers.
  // Already connected channels need no setup. Bridges poll cancellation while
  // reporting their setup stages on the session worker.
  virtual SetupResult PrepareConnection(const std::function<bool()>& active,
                                       const std::function<void(const char*)>& stage);
  virtual Result Read(base::span<char> bytes) = 0;
  virtual Result Write(base::span<const char> bytes) = 0;
  // A hangup makes Read ready: buffered data must be consumed before EOF.
  // Interrupted waits return no readiness. Callers recheck cancellation.
  virtual Ready Wait(bool read, bool write, int timeout_ms) = 0;
  virtual void Close() = 0;
  virtual const char* name() const = 0;
  // Asynchronous connection bridges retain their setup failure separately
  // from an established peer closing the stream. Safe to call on the worker.
  virtual std::string connection_error() const;
};

struct ChannelConnectResult {
  std::unique_ptr<TransportChannel> channel;
  const char* outcome = "connect_failed";
  int system_error = 0;
};
// One IPv4 attempt. The caller owns address fallback and overall time budget.
// An explicit route requires both source IPv4 and interface index. Failure to
// bind it is reported, never silently retried with system routing. Empty route
// preserves system routing for callers without discovery information.
// active is called synchronously on the worker, at most 100 ms between polls.
ChannelConnectResult ConnectTcpChannel(const std::string& address, uint16_t port,
                                      base::TimeTicks deadline,
                                      const std::function<bool()>& active,
                                      const std::string& local_address = {},
                                      unsigned interface_index = 0);
// Upgrade credentials contain four network-order IPv4 bytes, not IP text.
// Only directly attached unicast peers are accepted; bind the selected route.
ChannelConnectResult ConnectLanUpgradeChannel(const std::string& address, int port,
                                             base::TimeTicks deadline,
                                             const std::function<bool()>& active);
}  // namespace crquickshare
#endif
