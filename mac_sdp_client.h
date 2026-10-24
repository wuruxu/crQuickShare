// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_MAC_SDP_CLIENT_H_
#define CRQUICKSHARE_MAC_SDP_CLIENT_H_

#include <stdint.h>
#include <memory>
#include <optional>
#include <string>
#include "base/functional/callback.h"

namespace device { class BluetoothUUID; }
namespace crquickshare {

inline constexpr char kMacSdpProbeSwitch[] = "quick-share-mac-sdp-probe";
// Runs one query on an existing UI task executor, without discovery or RFCOMM.
int RunMacSdpProbe(const std::string& address);

struct MacSdpRoute {
  uint32_t record_handle = 0;
  uint8_t rfcomm_channel = 0;
};

struct MacSdpClientImpl;
// UI sequence only. Sends SDP ServiceSearchAttribute requests over its own
// classic L2CAP channel (PSM 1); never uses cached records as a query result.
class MacSdpClient {
 public:
  using ResultCallback = base::OnceCallback<void(std::optional<MacSdpRoute>,
                                                const std::string&)>;
  MacSdpClient();
  ~MacSdpClient();
  MacSdpClient(const MacSdpClient&) = delete;
  MacSdpClient& operator=(const MacSdpClient&) = delete;
  void Query(const std::string& address, const device::BluetoothUUID& uuid,
             ResultCallback callback);
  // Cancels without invoking the result callback; closes only our SDP channel.
  void Cancel();
 private:
  std::unique_ptr<MacSdpClientImpl> impl_;
};

}  // namespace crquickshare
#endif
