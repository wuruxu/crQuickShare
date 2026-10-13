// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_CONNECTION_LISTENER_H_
#define CRQUICKSHARE_CONNECTION_LISTENER_H_
#include <cstdint>
#include <memory>
#include <string>
#include "crQuickShare/transport_channel.h"
namespace crquickshare {
// Owned by the discovery worker. Accepted channels transfer to a session worker.
// Null channel with no error means timeout/interruption; native handles never
// escape the backend. Destruction releases the listening socket.
class ConnectionListener {
 public:
  struct Accepted {
    std::unique_ptr<TransportChannel> channel;
    std::string address;
    const char* error = nullptr;
    int system_error = 0;
  };
  virtual ~ConnectionListener();
  virtual uint16_t port() const = 0;
  // Waits at most timeout_ms, allowing the caller to recheck shutdown.
  virtual Accepted Accept(int timeout_ms) = 0;
};
struct ListenerResult {
  std::unique_ptr<ConnectionListener> listener;
  const char* error = nullptr;
  int system_error = 0;
};
// Binds an ephemeral IPv4 LAN port. Access policy remains in the service.
ListenerResult CreateConnectionListener();
}  // namespace crquickshare
#endif
