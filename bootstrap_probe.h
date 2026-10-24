// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_BOOTSTRAP_PROBE_H_
#define CRQUICKSHARE_BOOTSTRAP_PROBE_H_
#include <memory>
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "crQuickShare/quick_share_service.h"
namespace device { class BluetoothAdapter; class BluetoothSocket; }
namespace crquickshare {
// Desktop UI-sequence Bluetooth socket bridge. Observe() is the optional one-shot
// diagnostic. Send() connects an explicitly selected peer to the normal service
// worker; the embedding owner must keep that service alive until Cancel().
class BootstrapProbe {
 public:
  explicit BootstrapProbe(bool command_line_probe = true);
  ~BootstrapProbe();
  void Observe(scoped_refptr<device::BluetoothAdapter> adapter, const Peer& peer);
  void Cancel();
  bool CanProbe(const Peer& peer) const;
  bool active() const;
  // Only macOS offers recovery, and only before a socket connected.
  bool CanRetryWithBluetoothReconnect() const;
  bool reconnecting_bluetooth() const;
  // Consumes the accepted socket, including on rejection. The service must
  // outlive this bridge, as for Send().
  bool Receive(scoped_refptr<device::BluetoothSocket> socket, QuickShareService& service, HotspotConnector hotspot = {});
  // Explicit UI send, independent of the one-shot command-line probe.
  // Optional registrar invokes the supplied closure on the UI sequence once
  // discovery has released GATT. The closure is safe after Cancel/destruction.
  // reconnect_bluetooth forces manual macOS recovery. macOS 27 sends also
  // prepare an already-connected target once, after GATT has been released.
  bool Send(scoped_refptr<device::BluetoothAdapter> adapter, const Peer& peer,
            QuickShareService& service, std::vector<base::FilePath> paths,
            HotspotConnector hotspot,
            std::function<void(base::OnceClosure)> when_connection_ready = {},
            bool reconnect_bluetooth = false);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
#endif
