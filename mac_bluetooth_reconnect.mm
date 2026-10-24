// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/mac_bluetooth_reconnect.h"

#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>
#include <atomic>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/sequence_checker.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "device/bluetooth/public/cpp/bluetooth_address.h"

namespace crquickshare { class MacReconnectOperation; }
@interface CrQuickShareReconnectDelegate : NSObject {
  base::WeakPtr<crquickshare::MacReconnectOperation> _owner;
  scoped_refptr<base::SingleThreadTaskRunner> _runner;
  CrQuickShareReconnectDelegate* __strong _pendingOpen;
  uint64_t _operationId;
}
- (instancetype)initWithOwner:(base::WeakPtr<crquickshare::MacReconnectOperation>)owner
                 operationId:(uint64_t)operationId;
- (void)deviceDisconnected:(IOBluetoothUserNotification*)notification device:(IOBluetoothDevice*)device;
- (void)connectionComplete:(IOBluetoothDevice*)device status:(IOReturn)status;
- (void)beginOpen;
- (void)openReturned:(IOReturn)status;
@end

namespace crquickshare {
namespace {
uint64_t NextRecoveryId() {
  static std::atomic<uint64_t> next{0};
  return ++next;
}
const void* ObjectAddress(id object) { return (__bridge const void*)object; }
base::TimeTicks LogRecoveryCallback(uint64_t operation_id, const char* event, const char* phase,
                                   id delegate, IOBluetoothDevice* peer, std::optional<IOReturn> status = {},
                                   base::TimeTicks entered = {}, int owner_present = -1) {
  const auto now = base::TimeTicks::Now();
  // Native callback threads must not read device properties or the owner's
  // WeakPtr. Identity, status and timestamps are safe to record before posting.
  LOG(INFO) << "crQuickShare: macOS Bluetooth recovery callback; operation=" << operation_id
            << "; event=" << event << "; phase=" << phase
            << "; delegate=" << ObjectAddress(delegate) << "; callback_device=" << ObjectAddress(peer)
            << "; has_status=" << status.has_value() << "; status=" << status.value_or(kIOReturnSuccess)
            << "; owner_present=" << owner_present
            << "; callback_age_us=" << (entered.is_null() ? 0 : (now - entered).InMicroseconds())
            << "; main_thread=" << bool([NSThread isMainThread])
            << "; thread_id=" << base::PlatformThread::CurrentId()
            << "; time_ticks_us=" << now.since_origin().InMicroseconds();
  return now;
}
std::set<std::string>& PendingReconnects() {
  static base::NoDestructor<std::set<std::string>> addresses;
  return *addresses;
}
}  // namespace
class MacReconnectOperation : public base::RefCounted<MacReconnectOperation> {
 public:
  MacReconnectOperation(std::string address, base::OnceCallback<void(bool, const std::string&)> done)
      : address_(device::CanonicalizeBluetoothAddress(address)), done_(std::move(done)) {
    if (!address_.empty()) owns_slot_ = PendingReconnects().insert(address_).second;
  }
  void Start() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_);
    scoped_refptr<MacReconnectOperation> keep_alive(this);
    started_ = base::TimeTicks::Now();
    if (cancelled_) { Finish(false, "Bluetooth reconnection cancelled before start"); return; }
    if (address_.empty()) { Finish(false, "Invalid classic Bluetooth address"); return; }
    if (!owns_slot_) { Finish(false, "Bluetooth reconnection is already in progress"); return; }
    peer_ = [IOBluetoothDevice deviceWithAddressString:base::SysUTF8ToNSString(address_)];
    if (!peer_) { Finish(false, "Unable to create Bluetooth reconnection target"); return; }
    delegate_ = [[CrQuickShareReconnectDelegate alloc] initWithOwner:weak_.GetWeakPtr()
                                                     operationId:operation_id_];
    initially_connected_ = [peer_ isConnected];
    LOG(INFO) << "crQuickShare: macOS Bluetooth recovery started; address=" << address_
              << "; connected=" << initially_connected_ << "; pairing_changed=0; operation=" << operation_id_;
    LogState("start", "observe");
    // isConnected can reflect the peripheral before the classic peer catches
    // up. Always request close for this explicitly authorized recovery; use the
    // disconnect notification to establish the classic transition.
    notification_ = [peer_ registerForDisconnectNotification:delegate_
        selector:@selector(deviceDisconnected:device:)];
    if (!notification_) { Finish(false, "Unable to observe Bluetooth disconnection"); return; }
    stage_ = Stage::kDisconnecting;
    deadline_.Start(FROM_HERE, base::Seconds(8),
        base::BindOnce(&MacReconnectOperation::TimedOut, keep_alive));
    LogState("close_requested", "observe");
    const IOReturn status = [peer_ closeConnection];
    LOG(INFO) << "crQuickShare: macOS Bluetooth recovery close returned; status=" << status
              << "; awaiting_disconnect_notification=1; operation=" << operation_id_;
    LogState("close_returned", "observe", nil, status);
    if (status != kIOReturnSuccess) {
      if (initially_connected_)
        failure_ = "Bluetooth disconnection failed (" + base::NumberToString(status) + ")";
      Open();  // Attempt to restore the link even if close reported an error.
    }
  }
  void Cancel() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_);
    cancelled_ = true;
    LogState("cancel", "observe");
    done_.Reset();
    // Keep the deadline/native operation alive until its reconnect attempt has
    // completed. Cancelling a transfer must not strand an intentionally closed link.
  }
  void Disconnected(IOBluetoothDevice* peer) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_);
    scoped_refptr<MacReconnectOperation> keep_alive(this);
    ++disconnect_callbacks_;
    const char* disposition = stage_ != Stage::kDisconnecting ? "ignored_stage_mismatch" :
        peer != peer_ ? "ignored_device_mismatch" : "accept";
    LogState("disconnect_callback", disposition, peer);
    if (stage_ != Stage::kDisconnecting || peer != peer_) return;
    LOG(INFO) << "crQuickShare: macOS Bluetooth recovery disconnect confirmed; address=" << address_
              << "; operation=" << operation_id_;
    Open();
  }
  void Connected(IOBluetoothDevice* peer, IOReturn status) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_);
    scoped_refptr<MacReconnectOperation> keep_alive(this);
    ++connect_callbacks_;
    const char* disposition = stage_ != Stage::kConnecting ? "ignored_stage_mismatch" :
        peer != peer_ ? "ignored_device_mismatch" : "accept";
    LogState("connect_callback", disposition, peer, status);
    if (stage_ != Stage::kConnecting || peer != peer_) return;
    LOG(INFO) << "crQuickShare: macOS Bluetooth recovery connection callback; status=" << status
              << "; connected=" << bool([peer_ isConnected]) << "; cancelled=" << cancelled_
              << "; operation=" << operation_id_;
    if (status != kIOReturnSuccess)
      Finish(false, "Bluetooth reconnection failed (" + base::NumberToString(status) + ")");
    else Finish(failure_.empty(), failure_);
  }
 private:
  friend class base::RefCounted<MacReconnectOperation>;
  ~MacReconnectOperation() = default;
  enum class Stage { kPending, kDisconnecting, kConnecting, kDone };
  const char* StageName() const {
    switch (stage_) {
      case Stage::kPending: return "pending";
      case Stage::kDisconnecting: return "disconnecting";
      case Stage::kConnecting: return "connecting";
      case Stage::kDone: return "done";
    }
    return "unknown";
  }
  void LogState(const char* event, const char* disposition, IOBluetoothDevice* callback_peer = nil,
                std::optional<IOReturn> status = {}) const {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_);
    const auto callback_address = device::CanonicalizeBluetoothAddress(
        base::SysNSStringToUTF8([callback_peer addressString]));
    LOG(INFO) << "crQuickShare: macOS Bluetooth recovery state; operation=" << operation_id_
              << "; event=" << event << "; stage=" << StageName() << "; disposition=" << disposition
              << "; address=" << address_ << "; device=" << ObjectAddress(peer_)
              << "; device_present=" << bool(peer_) << "; delegate=" << ObjectAddress(delegate_)
              << "; notification_present=" << bool(notification_)
              << "; connected=" << bool([peer_ isConnected]) << "; paired=" << bool([peer_ isPaired])
              << "; connection_handle=" << [peer_ connectionHandle]
              << "; callback_device=" << ObjectAddress(callback_peer) << "; callback_address=" << callback_address
              << "; device_matches=" << (callback_peer && callback_peer == peer_)
              << "; address_matches=" << (!callback_address.empty() && callback_address == address_)
              << "; callback_connected=" << bool([callback_peer isConnected])
              << "; callback_paired=" << bool([callback_peer isPaired])
              << "; callback_connection_handle=" << [callback_peer connectionHandle]
              << "; has_status=" << status.has_value() << "; status=" << status.value_or(kIOReturnSuccess)
              << "; disconnect_callbacks=" << disconnect_callbacks_ << "; connect_callbacks=" << connect_callbacks_
              << "; cancelled=" << cancelled_ << "; has_result=" << bool(done_)
              << "; elapsed_ms=" << (started_.is_null() ? -1 : (base::TimeTicks::Now() - started_).InMilliseconds())
              << "; main_thread=" << bool([NSThread isMainThread])
              << "; thread_id=" << base::PlatformThread::CurrentId();
  }
  void Open() {
    scoped_refptr<MacReconnectOperation> keep_alive(this);
    deadline_.Stop();
    [notification_ unregister]; notification_ = nil;
    stage_ = Stage::kConnecting;
    deadline_.Start(FROM_HERE, base::Seconds(15),
        base::BindOnce(&MacReconnectOperation::TimedOut, keep_alive));
    [delegate_ beginOpen];
    LogState("open_requested", "observe");
    const IOReturn status = [peer_ openConnection:delegate_];
    [delegate_ openReturned:status];
    LOG(INFO) << "crQuickShare: macOS Bluetooth recovery open requested; status=" << status
              << "; operation=" << operation_id_;
    LogState("open_returned", "observe", nil, status);
    if (status != kIOReturnSuccess)
      Finish(false, "Bluetooth reconnection request failed (" + base::NumberToString(status) + ")");
  }
  void TimedOut() {
    scoped_refptr<MacReconnectOperation> keep_alive(this);
    LogState("deadline", "observe");
    if (stage_ == Stage::kDisconnecting) {
      const bool connected = [peer_ isConnected];
      // An initially disconnected device need not emit a disconnect event.
      // Its new connection still has to complete, followed by a wire SDP query.
      if (initially_connected_ || connected) failure_ = "Bluetooth disconnection confirmation timed out";
      LOG(WARNING) << "crQuickShare: macOS Bluetooth recovery disconnect timed out; restoring_link=1"
                   << "; initially_connected=" << initially_connected_ << "; connected=" << connected
                   << "; operation=" << operation_id_;
      Open();
    } else Finish(false, "Bluetooth reconnection timed out");
  }
  void Finish(bool success, const std::string& error) {
    scoped_refptr<MacReconnectOperation> keep_alive(this);
    if (stage_ == Stage::kDone) return;
    LogState("finish", success ? "success" : "failed");
    stage_ = Stage::kDone;
    if (owns_slot_) { PendingReconnects().erase(address_); owns_slot_ = false; }
    weak_.InvalidateWeakPtrs();
    deadline_.Stop();
    [notification_ unregister]; notification_ = nil;
    LOG(INFO) << "crQuickShare: macOS Bluetooth recovery finished; success=" << success
              << "; cancelled=" << cancelled_ << "; reason=" << error
              << "; operation=" << operation_id_ << "; disconnect_callbacks=" << disconnect_callbacks_
              << "; connect_callbacks=" << connect_callbacks_
              << "; elapsed_ms=" << (base::TimeTicks::Now() - started_).InMilliseconds();
    // A notification can already have queued an unretained observer target.
    CrQuickShareReconnectDelegate* keep_delegate = delegate_;
    dispatch_async(dispatch_get_main_queue(), ^{ (void)keep_delegate; });
    delegate_ = nil;
    auto done = std::move(done_);
    if (done) std::move(done).Run(success, error);
  }
  std::string address_, failure_;
  const uint64_t operation_id_ = NextRecoveryId();
  base::OnceCallback<void(bool, const std::string&)> done_;
  IOBluetoothDevice* __strong peer_ = nil;
  CrQuickShareReconnectDelegate* __strong delegate_ = nil;
  IOBluetoothUserNotification* __weak notification_ = nil;
  Stage stage_ = Stage::kPending;
  bool cancelled_ = false;
  bool initially_connected_ = false;
  bool owns_slot_ = false;
  unsigned disconnect_callbacks_ = 0, connect_callbacks_ = 0;
  base::TimeTicks started_;
  base::OneShotTimer deadline_;
  SEQUENCE_CHECKER(sequence_);
  base::WeakPtrFactory<MacReconnectOperation> weak_{this};
};
bool MacBluetoothReconnectInProgress(const std::string& address) {
  return PendingReconnects().contains(device::CanonicalizeBluetoothAddress(address));
}
base::OnceClosure ReconnectMacBluetoothDevice(const std::string& address,
    base::OnceCallback<void(bool, const std::string&)> done) {
  auto operation = base::MakeRefCounted<MacReconnectOperation>(address, std::move(done));
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce(&MacReconnectOperation::Start, operation));
  return base::BindOnce(&MacReconnectOperation::Cancel, std::move(operation));
}
}  // namespace crquickshare

@implementation CrQuickShareReconnectDelegate
- (instancetype)initWithOwner:(base::WeakPtr<crquickshare::MacReconnectOperation>)owner
                 operationId:(uint64_t)operationId {
  if ((self = [super init])) {
    _owner = std::move(owner); _runner = base::SingleThreadTaskRunner::GetCurrentDefault();
    _operationId = operationId;
  }
  return self;
}
- (void)deviceDisconnected:(IOBluetoothUserNotification*)notification device:(IOBluetoothDevice*)device {
  [[maybe_unused]] NS_VALID_UNTIL_END_OF_SCOPE CrQuickShareReconnectDelegate* keepAlive = self;
  const auto entered = crquickshare::LogRecoveryCallback(_operationId, "disconnect", "native_entry", self, device);
  const bool posted = _runner->PostNonNestableTask(FROM_HERE, base::BindOnce(
      [](base::WeakPtr<crquickshare::MacReconnectOperation> owner,
         CrQuickShareReconnectDelegate* delegate, IOBluetoothDevice* device,
         uint64_t operation_id, base::TimeTicks entered) {
        crquickshare::LogRecoveryCallback(operation_id, "disconnect", "ui_delivery", delegate, device,
                                          {}, entered, bool(owner));
        if (owner) owner->Disconnected(device);
      }, _owner, self, device, _operationId, entered));
  crquickshare::LogRecoveryCallback(_operationId, "disconnect", posted ? "posted" : "post_failed",
                                    self, device, {}, entered);
}
- (void)beginOpen { _pendingOpen = self; }
- (void)openReturned:(IOReturn)status { if (status != kIOReturnSuccess) _pendingOpen = nil; }
- (void)connectionComplete:(IOBluetoothDevice*)device status:(IOReturn)status {
  [[maybe_unused]] NS_VALID_UNTIL_END_OF_SCOPE CrQuickShareReconnectDelegate* keepAlive = self;
  const auto entered = crquickshare::LogRecoveryCallback(_operationId, "connect", "native_entry", self, device, status);
  const bool posted = _runner->PostNonNestableTask(FROM_HERE, base::BindOnce(
      [](base::WeakPtr<crquickshare::MacReconnectOperation> owner,
         CrQuickShareReconnectDelegate* delegate, IOBluetoothDevice* device, IOReturn status,
         uint64_t operation_id, base::TimeTicks entered) {
        crquickshare::LogRecoveryCallback(operation_id, "connect", "ui_delivery", delegate, device,
                                          status, entered, bool(owner));
        if (owner) owner->Connected(device, status);
      }, _owner, self, device, status, _operationId, entered));
  crquickshare::LogRecoveryCallback(_operationId, "connect", posted ? "posted" : "post_failed",
                                    self, device, status, entered);
  // A late native completion remains safe after timeout/cancellation; only the
  // small delegate is retained while the native open request is outstanding.
  _pendingOpen = nil;
}
@end
