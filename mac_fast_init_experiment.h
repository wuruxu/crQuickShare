// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_MAC_FAST_INIT_EXPERIMENT_H_
#define CRQUICKSHARE_MAC_FAST_INIT_EXPERIMENT_H_
#include <memory>

namespace crquickshare {
inline constexpr char kMacFastInitExperimentSwitch[] = "quick-share-mac-fast-init-experiment";
inline constexpr char kDisableMacFastInitExperimentSwitch[] = "disable-quick-share-mac-fast-init-experiment";
// Enabled by default. The legacy enable switch is still accepted; disable wins.
bool IsMacFastInitExperimentEnabled();

// Experimental, default-on FE2C + Base64URL local-name advertising in Silent mode.
// Not a wire-equivalent replacement for Service Data or verified Android Fast Init.
// Main UI thread only. Construction does not create a CoreBluetooth manager.
// Update periodically during discovery for bounded retries/timeouts. Bluetooth
// permission must be resolved by the host's normal discovery flow first.
class MacFastInitExperiment {
 public:
  enum class State { kIdle, kStarting, kAdvertising, kUnavailable };
  MacFastInitExperiment();
  ~MacFastInitExperiment();
  MacFastInitExperiment(const MacFastInitExperiment&) = delete;
  MacFastInitExperiment& operator=(const MacFastInitExperiment&) = delete;
  void Update(bool enabled);
  State state() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace crquickshare
#endif
