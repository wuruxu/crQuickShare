// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_COREWLAN_CLIENT_MAC_H_
#define CRQUICKSHARE_COREWLAN_CLIENT_MAC_H_

#include <memory>
#include <string>
#include "crQuickShare/hotspot_upgrade.h"

namespace crquickshare {
// UI sequence only. Request access when the user opens nearby discovery, not
// during startup or a time-limited network association. Requires the native
// location usage descriptions in the application's Info.plist.
// Refreshes the cached authorization state; a blocked Join schedules a read-only
// UI refresh so a later retry can observe permission changes.
// Authorization waits for foreground activation. Call the cancellation function
// below when discovery closes before that activation occurs.
void RequestCoreWlanAccess();
// UI sequence only. Cancels waiting for foreground activation when discovery
// closes. An authorization request already submitted to macOS is unaffected.
void CancelDeferredCoreWlanAccessRequest();

// A station connection, independent of the Quick Share medium using it. Native
// scanning/association/restoration run on a background task. The caller remains
// on the session worker and services its existing transport through active().
// No P2P discovery or group-owner role is implied by this client.
// One instance owns one Join/Cleanup lifecycle; construct a new client to retry.
class CoreWlanClient final : public UpgradeNetwork {
 public:
  explicit CoreWlanClient(std::string interface_name);
  ~CoreWlanClient() override;
  CoreWlanClient(const CoreWlanClient&) = delete;
  CoreWlanClient& operator=(const CoreWlanClient&) = delete;

  // Returns a diagnostic literal on failure, nullptr on successful association.
  // SSID is raw network bytes. A password is not logged or written to a profile
  // by this client. The OS may retain credentials according to its own policy.
  // Previous Wi-Fi credentials are never read. After switching, Cleanup releases
  // this client's target and waits for system AutoJoin within its time budget;
  // reconnecting the exact previous network is not guaranteed.
  const char* Join(const std::string& ssid, const std::string& password,
                   int frequency, base::TimeTicks deadline,
                   const std::function<bool()>& active);
  bool IsCurrent() const;
  unsigned interface_index() const;
  bool Cleanup(base::TimeTicks deadline,
               const std::function<bool()>& keep_restoring) override;

 private:
  struct State;
  std::shared_ptr<State> state_;
};
}  // namespace crquickshare
#endif
