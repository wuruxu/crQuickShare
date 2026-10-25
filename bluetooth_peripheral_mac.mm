// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/bluetooth_peripheral_mac.h"

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include <string>
#include <utility>
#include "base/base64url.h"
#include "base/logging.h"
#include "base/strings/sys_string_conversions.h"
#include "base/time/time.h"
#include "crQuickShare/advertisement_codec.h"

@interface CRQuickShareReceiverPeripheral : NSObject <CBPeripheralManagerDelegate> {
 @public
  BOOL ready_, advertising_, failed_;
 @private
  CBPeripheralManager* _manager;
  CBMutableService* _service;
  CBMutableCharacteristic* _slot;
  NSData* _value;
  BOOL _wanted, _adding, _advertising_pending;
  base::TimeTicks _deadline;
  base::OnceCallback<void(bool)> _prepare_callback, _advertise_callback;
  uint64_t _reads;
}
- (void)prepare:(const std::vector<uint8_t>&)value callback:(base::OnceCallback<void(bool)>)callback;
- (void)advertise:(const std::vector<uint8_t>&)header callback:(base::OnceCallback<void(bool)>)callback;
- (void)poll;
- (void)withdraw;
- (void)stop;
- (void)retire;
- (void)fail:(const char*)reason error:(NSError*)error;
@end

@implementation CRQuickShareReceiverPeripheral
- (void)retire {
  CBPeripheralManager* previous = _manager;
  _manager = nil;
  previous.delegate = nil;
  if (previous.state == CBManagerStatePoweredOn) {
    [previous stopAdvertising];
    [previous removeAllServices];
  }
  _service = nil;
  _slot = nil;
  _value = nil;
  ready_ = advertising_ = _wanted = _adding = _advertising_pending = NO;
  _deadline = {};
}
- (void)stop {
  DCHECK([NSThread isMainThread]);
  _prepare_callback.Reset();
  _advertise_callback.Reset();
  [self retire];
  failed_ = NO;
}
- (void)fail:(const char*)reason error:(NSError*)error {
  LOG(WARNING) << "crQuickShare: macOS receiver peripheral failed; reason=" << reason
               << "; error_domain=" << base::SysNSStringToUTF8(error.domain)
               << "; error_code=" << error.code;
  auto prepare = std::move(_prepare_callback);
  auto advertise = std::move(_advertise_callback);
  [self retire];
  failed_ = YES;
  // Callbacks may synchronously stop this peripheral through the receiver.
  if (prepare) std::move(prepare).Run(false);
  if (advertise) std::move(advertise).Run(false);
}
- (void)prepare:(const std::vector<uint8_t>&)value callback:(base::OnceCallback<void(bool)>)callback {
  [self stop];
  _reads = 0;
  _prepare_callback = std::move(callback);
  if (value.empty() || value.size() > 512 ||
      CBManager.authorization != CBManagerAuthorizationAllowedAlways) {
    [self fail:"permission_or_value_unavailable" error:nil];
    return;
  }
  _value = [NSData dataWithBytes:value.data() length:value.size()];
  _deadline = base::TimeTicks::Now() + base::Seconds(10);
  // Install the delegate after retaining the manager, so even an immediate
  // state notification can be checked against this attempt's manager.
  _manager = [[CBPeripheralManager alloc] initWithDelegate:nil queue:nil
      options:@{CBPeripheralManagerOptionShowPowerAlertKey : @NO}];
  if (!_manager) { [self fail:"manager_unavailable" error:nil]; return; }
  _manager.delegate = self;
  [self poll];
}
- (void)poll {
  DCHECK([NSThread isMainThread]);
  if (!_manager || failed_) return;
  if (!_deadline.is_null() && base::TimeTicks::Now() >= _deadline) {
    [self fail:(_advertising_pending ? "advertising_timeout" : "gatt_setup_timeout") error:nil];
    return;
  }
  if (CBManager.authorization != CBManagerAuthorizationAllowedAlways) {
    [self fail:"permission_revoked" error:nil]; return;
  }
  const auto state = _manager.state;
  if (state == CBManagerStateUnknown) {
    if (ready_) [self fail:"adapter_state_lost" error:nil];
    return;
  }
  if (state != CBManagerStatePoweredOn) {
    [self fail:"adapter_unavailable" error:nil]; return;
  }
  if (advertising_ && !_manager.isAdvertising) {
    [self fail:"advertising_lost" error:nil]; return;
  }
  if (ready_ || _adding) return;
  _service = [[CBMutableService alloc] initWithType:[CBUUID UUIDWithString:@"FEF3"] primary:YES];
  // Dynamic reads allow visibility checks and observable read/blob requests.
  _slot = [[CBMutableCharacteristic alloc]
      initWithType:[CBUUID UUIDWithString:@"00000000-0000-3000-8000-000000000000"]
      properties:CBCharacteristicPropertyRead value:nil permissions:CBAttributePermissionsReadable];
  _service.characteristics = @[_slot];
  _adding = YES;
  LOG(INFO) << "crQuickShare: macOS receiver GATT requested; slots=1; bytes=" << _value.length;
  [_manager addService:_service];
}
- (void)peripheralManagerDidUpdateState:(CBPeripheralManager*)peripheral {
  if (peripheral != _manager) return;
  LOG(INFO) << "crQuickShare: macOS receiver peripheral state; state=" << peripheral.state;
  [self poll];
}
- (void)peripheralManager:(CBPeripheralManager*)peripheral didAddService:(CBService*)service error:(NSError*)error {
  if (peripheral != _manager || service != _service || !_adding) return;
  if (error) { [self fail:"gatt_registration" error:error]; return; }
  if (peripheral.state != CBManagerStatePoweredOn ||
      CBManager.authorization != CBManagerAuthorizationAllowedAlways) {
    [self fail:"gatt_registration_state_lost" error:nil]; return;
  }
  _adding = NO;
  ready_ = YES;
  _deadline = {};
  LOG(INFO) << "crQuickShare: macOS receiver GATT ready; slots=1; bytes=" << _value.length;
  if (_prepare_callback) std::move(_prepare_callback).Run(true);
}
- (void)advertise:(const std::vector<uint8_t>&)header callback:(base::OnceCallback<void(bool)>)callback {
  DCHECK([NSThread isMainThread]);
  _advertise_callback = std::move(callback);
  const std::string encoded(header.begin(), header.end());
  std::string raw;
  // EncodeGattHeader already returns Base64URL of the 15-byte V2 header.
  // Nearby Apple uses raw V2 bytes -> Base64URL local name. Do not encode the
  // existing ASCII header a second time or truncate away the advertisement hash.
  const auto parsed = crquickshare::ParseGattHeader(encoded);
  if (!ready_ || _advertising_pending || advertising_ || encoded.size() != 20 ||
      !base::Base64UrlDecode(encoded, base::Base64UrlDecodePolicy::DISALLOW_PADDING, &raw) ||
      raw.size() != 15 || !parsed || parsed->slots != 1) {
    [self fail:"invalid_advertising_state_or_header" error:nil]; return;
  }
  _wanted = _advertising_pending = YES;
  _deadline = base::TimeTicks::Now() + base::Seconds(10);
  LOG(INFO) << "crQuickShare: macOS receiver advertising requested; service_uuid=FEF3"
            << "; encoding=local_name_base64url; header_bytes=15; local_name_bytes=20"
            << "; local_name=" << encoded
            << "; wire_service_data=0; peer_discovery_confirmed=0";
  [_manager startAdvertising:@{
      CBAdvertisementDataServiceUUIDsKey : @[[CBUUID UUIDWithString:@"FEF3"]],
      CBAdvertisementDataLocalNameKey : base::SysUTF8ToNSString(encoded)}];
}
- (void)peripheralManagerDidStartAdvertising:(CBPeripheralManager*)peripheral error:(NSError*)error {
  if (peripheral != _manager || !_advertising_pending) return;
  if (error) { [self fail:"advertising_callback" error:error]; return; }
  if (!_wanted || peripheral.state != CBManagerStatePoweredOn || !peripheral.isAdvertising ||
      CBManager.authorization != CBManagerAuthorizationAllowedAlways) {
    [self fail:"advertising_not_active" error:nil]; return;
  }
  _advertising_pending = NO;
  advertising_ = YES;
  _deadline = {};
  LOG(INFO) << "crQuickShare: macOS receiver advertising started; os_advertising=1"
            << "; wire_service_data=0; peer_discovery_confirmed=0";
  if (_advertise_callback) std::move(_advertise_callback).Run(true);
}
- (void)withdraw {
  DCHECK([NSThread isMainThread]);
  _wanted = NO;
  _advertise_callback.Reset();
  // A pending start callback has no request ID. Retire its manager so it cannot
  // complete a later publication. Poll asks the owner to recreate the listener.
  if (_advertising_pending) {
    [self retire];
    failed_ = YES;
    return;
  }
  if (advertising_) {
    [_manager stopAdvertising];
    LOG(INFO) << "crQuickShare: macOS receiver advertising stopped; gatt_reads=" << _reads;
  }
  advertising_ = NO;
}
- (void)peripheralManager:(CBPeripheralManager*)peripheral didReceiveReadRequest:(CBATTRequest*)request {
  if (peripheral != _manager) return;
  if (!_wanted || !ready_ || ![request.characteristic.UUID isEqual:_slot.UUID] ||
      ![request.characteristic.service.UUID isEqual:_service.UUID]) {
    [peripheral respondToRequest:request withResult:CBATTErrorReadNotPermitted]; return;
  }
  if (request.offset > _value.length) {
    [peripheral respondToRequest:request withResult:CBATTErrorInvalidOffset]; return;
  }
  request.value = [_value subdataWithRange:NSMakeRange(request.offset, _value.length - request.offset)];
  ++_reads;
  // A read confirms GATT access, not that the phone displayed this endpoint.
  if (_reads <= 8 || _reads % 64 == 0)
    LOG(INFO) << "crQuickShare: macOS receiver GATT read; slot=0; offset=" << request.offset
              << "; bytes=" << request.value.length << "; reads=" << _reads;
  [peripheral respondToRequest:request withResult:CBATTErrorSuccess];
}
- (void)peripheralManager:(CBPeripheralManager*)peripheral didReceiveWriteRequests:(NSArray<CBATTRequest*>*)requests {
  if (peripheral == _manager && requests.count)
    [peripheral respondToRequest:requests.firstObject withResult:CBATTErrorWriteNotPermitted];
}
@end

namespace crquickshare {
struct BluetoothPeripheralMac::Impl {
  CRQuickShareReceiverPeripheral* native = [[CRQuickShareReceiverPeripheral alloc] init];
  ~Impl() { [native stop]; }
};
BluetoothPeripheralMac::BluetoothPeripheralMac() : impl_(std::make_unique<Impl>()) {}
BluetoothPeripheralMac::~BluetoothPeripheralMac() = default;
void BluetoothPeripheralMac::Prepare(const std::vector<uint8_t>& value, base::OnceCallback<void(bool)> ready) {
  [impl_->native prepare:value callback:std::move(ready)];
}
void BluetoothPeripheralMac::Advertise(const std::vector<uint8_t>& header, base::OnceCallback<void(bool)> ready) {
  [impl_->native advertise:header callback:std::move(ready)];
}
void BluetoothPeripheralMac::Withdraw() { [impl_->native withdraw]; }
void BluetoothPeripheralMac::Stop() { [impl_->native stop]; }
bool BluetoothPeripheralMac::Poll() { [impl_->native poll]; return !impl_->native->failed_; }
bool BluetoothPeripheralMac::ready() const { return impl_->native->ready_; }
bool BluetoothPeripheralMac::advertising() const { return impl_->native->advertising_; }
}  // namespace crquickshare
