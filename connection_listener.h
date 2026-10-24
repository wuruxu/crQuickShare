// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_CONNECTION_LISTENER_H_
#define CRQUICKSHARE_CONNECTION_LISTENER_H_
#include <cstdint>
#include <memory>
#include <string>
#include "crQuickShare/transport_channel.h"
namespace crquickshare {
// Owned by a discovery or session worker. Accepted channels transfer to a session.
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
// Session-owned desktop LAN upgrade listener. Publishes exactly one IPv4
// address, bound to a physical Wi-Fi/Ethernet interface; never switches Wi-Fi.
struct LanUpgradeListenerResult {
  std::unique_ptr<ConnectionListener> listener;
  TcpRoute route;
  std::string ip_address;  // Four network-order bytes for WifiLanSocket.
  const char* error = nullptr;
  int system_error = 0;
};
LanUpgradeListenerResult CreateLanUpgradeListener();
// Backend helpers used by the shared single-address selector. Binding failures
// are terminal for this offer; never fall back to a wildcard listener.
bool IsPhysicalLanInterface(const std::string& name, unsigned interface_index);
ListenerResult CreateBoundLanUpgradeListener(const TcpRoute& route);
}  // namespace crquickshare
#endif
