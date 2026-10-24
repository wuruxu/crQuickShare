// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/mac_fast_init_experiment.h"

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include <cstdint>
#include <string>
#include "base/base64url.h"
#include "base/command_line.h"
#include "base/logging.h"
#include "base/strings/sys_string_conversions.h"
#include "base/time/time.h"

using FastInitExperimentState = crquickshare::MacFastInitExperiment::State;

@interface CRQuickShareFastInitExperiment : NSObject <CBPeripheralManagerDelegate> {
  CBPeripheralManager* _manager;
  NSString* _encoded_name;
  BOOL _enabled, _wanted, _pending, _advertising, _shutdown;
  NSInteger _last_authorization, _last_manager_state;
  uint64_t _attempt;
  FastInitExperimentState _current;
  base::TimeTicks _window_started, _manager_created, _request_started;
  base::TimeTicks _advertising_started, _retry_after;
}
- (void)update:(BOOL)enabled;
- (FastInitExperimentState)state;
- (void)reconcile;
- (void)retire:(const char*)reason;
- (void)fail:(const char*)reason error:(NSInteger)code;
- (void)shutdown;
@end

@implementation CRQuickShareFastInitExperiment
- (instancetype)init {
  if ((self = [super init])) {
    DCHECK([NSThread isMainThread]);
    _enabled = crquickshare::IsMacFastInitExperimentEnabled();
    _last_authorization = _last_manager_state = -1;
    _current = FastInitExperimentState::kIdle;
    LOG(INFO) << "crQuickShare: macOS FastInit experiment configured; enabled=" << bool(_enabled)
              << "; mode=silent; encoding=local_name_base64url; wire_service_data=0; peer_response_confirmed=0";
  }
  return self;
}
- (FastInitExperimentState)state { return _current; }
- (void)update:(BOOL)enabled {
  DCHECK([NSThread isMainThread]);
  if (_shutdown) return;
  const BOOL wanted = enabled && _enabled;
  if (_wanted != wanted) {
    _wanted = wanted;
    _retry_after = {};
    if (wanted) {
      _window_started = base::TimeTicks::Now();
      LOG(INFO) << "crQuickShare: macOS FastInit experiment window; requested=1; time_ticks_us="
                << _window_started.since_origin().InMicroseconds();
    } else {
      LOG(INFO) << "crQuickShare: macOS FastInit experiment window; requested=0; time_ticks_us="
                << base::TimeTicks::Now().since_origin().InMicroseconds();
    }
  }
  [self reconcile];
}
- (void)retire:(const char*)reason {
  if (!_manager) return;
  const auto now = base::TimeTicks::Now();
  LOG(INFO) << "crQuickShare: macOS FastInit experiment stopped; attempt=" << _attempt
            << "; reason=" << reason << "; pending=" << bool(_pending)
            << "; was_advertising=" << bool(_advertising)
            << "; advertised_ms=" << (_advertising_started.is_null() ? 0 :
                (now - _advertising_started).InMilliseconds())
            << "; time_ticks_us=" << now.since_origin().InMicroseconds();
  // Retire the manager on cancellation/error. A callback from this attempt
  // cannot be mistaken for a later attempt on a newly created manager.
  CBPeripheralManager* previous = _manager;
  _manager = nil;
  _pending = _advertising = NO;
  _advertising_started = {};
  previous.delegate = nil;
  if (previous.state == CBManagerStatePoweredOn) [previous stopAdvertising];
}
- (void)fail:(const char*)reason error:(NSInteger)code {
  LOG(WARNING) << "crQuickShare: macOS FastInit experiment failed; attempt=" << _attempt
               << "; reason=" << reason << "; code=" << code << "; retry_delay_s=15";
  [self retire:reason];
  _retry_after = base::TimeTicks::Now() + base::Seconds(15);
  _current = FastInitExperimentState::kUnavailable;
}
- (void)reconcile {
  DCHECK([NSThread isMainThread]);
  if (_shutdown || !_wanted) {
    [self retire:"discovery_disabled"];
    _current = FastInitExperimentState::kIdle;
    return;
  }
  // Only inspect authorization here. Normal BLE discovery owns the permission
  // request; an opted-out experiment never allocates a peripheral manager.
  const auto authorization = CBManager.authorization;
  if (_last_authorization != authorization) {
    _last_authorization = authorization;
    LOG(INFO) << "crQuickShare: macOS FastInit experiment permission; status=" << authorization
              << "; allowed=" << (authorization == CBManagerAuthorizationAllowedAlways);
  }
  if (authorization != CBManagerAuthorizationAllowedAlways) {
    [self retire:"permission_unavailable"];
    _current = authorization == CBManagerAuthorizationNotDetermined ?
        FastInitExperimentState::kStarting : FastInitExperimentState::kUnavailable;
    return;
  }
  const auto now = base::TimeTicks::Now();
  if (now < _retry_after) return;
  if (!_manager) {
    _manager_created = now;
    _last_manager_state = -1;
    ++_attempt;
    _current = FastInitExperimentState::kStarting;
    LOG(INFO) << "crQuickShare: macOS FastInit experiment manager requested; attempt=" << _attempt;
    _manager = [[CBPeripheralManager alloc] initWithDelegate:self queue:nil
        options:@{CBPeripheralManagerOptionShowPowerAlertKey : @NO}];
    if (!_manager) { [self fail:"manager_unavailable" error:0]; return; }
  }
  const auto manager_state = _manager.state;
  if (_last_manager_state != manager_state) {
    _last_manager_state = manager_state;
    LOG(INFO) << "crQuickShare: macOS FastInit experiment adapter; attempt=" << _attempt
              << "; state=" << manager_state;
  }
  if (manager_state != CBManagerStatePoweredOn) {
    if (_pending || _advertising) { [self fail:"adapter_changed" error:0]; return; }
    const bool waiting = manager_state == CBManagerStateUnknown || manager_state == CBManagerStateResetting;
    if (waiting && now - _manager_created >= base::Seconds(10)) {
      [self fail:"manager_state_timeout" error:0]; return;
    }
    _current = waiting ? FastInitExperimentState::kStarting : FastInitExperimentState::kUnavailable;
    return;
  }
  if (_pending) {
    if (now - _request_started >= base::Seconds(10)) [self fail:"advertising_callback_timeout" error:0];
    return;
  }
  if (_advertising) {
    if (!_manager.isAdvertising) [self fail:"advertising_stopped_by_system" error:0];
    return;
  }
  if (!_encoded_name) {
    // Fast Init V1 Silent payload: type=1 in bits 4..2, other flags clear.
    // Match Nearby Apple's unpadded WebSafe Base64 local-name encoding.
    constexpr uint8_t payload[] = {0xfc, 0x12, 0x8e, 0x04, 0x42};
    std::string encoded;
    base::Base64UrlEncode(payload, base::Base64UrlEncodePolicy::OMIT_PADDING, &encoded);
    _encoded_name = base::SysUTF8ToNSString(encoded);
  }
  _pending = YES;
  _current = FastInitExperimentState::kStarting;
  _request_started = base::TimeTicks::Now();
  LOG(INFO) << "crQuickShare: macOS FastInit experiment advertising requested; attempt=" << _attempt
            << "; mode=silent; service_uuid=FE2C; payload_hex=fc128e0442; local_name="
            << base::SysNSStringToUTF8(_encoded_name)
            << "; encoding=local_name_base64url; wire_service_data=0; time_ticks_us="
            << _request_started.since_origin().InMicroseconds();
  // No manufacturer data or Service Data keys: both are unsupported by this
  // public API. The local name contains fixed protocol bytes, no user identity.
  [_manager startAdvertising:@{CBAdvertisementDataServiceUUIDsKey : @[[CBUUID UUIDWithString:@"FE2C"]],
                              CBAdvertisementDataLocalNameKey : _encoded_name}];
}
- (void)peripheralManagerDidUpdateState:(CBPeripheralManager*)peripheral {
  if (peripheral != _manager || _shutdown) return;
  [self reconcile];
}
- (void)peripheralManagerDidStartAdvertising:(CBPeripheralManager*)peripheral error:(NSError*)error {
  DCHECK([NSThread isMainThread]);
  if (peripheral != _manager || _shutdown || !_pending) return;
  if (!_wanted) { [self reconcile]; return; }
  if (error) { [self fail:"advertising_callback_error" error:error.code]; return; }
  if (CBManager.authorization != CBManagerAuthorizationAllowedAlways ||
      peripheral.state != CBManagerStatePoweredOn || !peripheral.isAdvertising) {
    [self fail:"advertising_not_active" error:0]; return;
  }
  _pending = NO;
  _advertising = YES;
  _current = FastInitExperimentState::kAdvertising;
  _advertising_started = base::TimeTicks::Now();
  LOG(INFO) << "crQuickShare: macOS FastInit experiment advertising started; attempt=" << _attempt
            << "; os_advertising=1; peer_response_confirmed=0; callback_ms="
            << (_advertising_started - _request_started).InMilliseconds()
            << "; window_elapsed_ms=" << (_advertising_started - _window_started).InMilliseconds()
            << "; advertising_time_ticks_us=" << _advertising_started.since_origin().InMicroseconds();
}
- (void)shutdown {
  DCHECK([NSThread isMainThread]);
  _shutdown = YES;
  _wanted = NO;
  [self retire:"shutdown"];
  _current = FastInitExperimentState::kIdle;
}
@end

namespace crquickshare {
bool IsMacFastInitExperimentEnabled() {
  return !base::CommandLine::ForCurrentProcess()->HasSwitch(kDisableMacFastInitExperimentSwitch);
}
struct MacFastInitExperiment::Impl {
  CRQuickShareFastInitExperiment* native = [[CRQuickShareFastInitExperiment alloc] init];
  ~Impl() { [native shutdown]; }
};
MacFastInitExperiment::MacFastInitExperiment() : impl_(std::make_unique<Impl>()) {}
MacFastInitExperiment::~MacFastInitExperiment() = default;
void MacFastInitExperiment::Update(bool enabled) { [impl_->native update:enabled]; }
MacFastInitExperiment::State MacFastInitExperiment::state() const { return [impl_->native state]; }
}  // namespace crquickshare
