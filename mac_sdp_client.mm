// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/mac_sdp_client.h"

#import <IOBluetooth/IOBluetooth.h>
#include <array>
#include <utility>
#include <vector>
#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/containers/span_reader.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/run_loop.h"
#include "base/sequence_checker.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "device/bluetooth/public/cpp/bluetooth_address.h"
#include "device/bluetooth/public/cpp/bluetooth_uuid.h"

namespace {

constexpr size_t kMaxPduBytes = 1024;
constexpr size_t kMaxAttributeBytes = 16384;
constexpr uint16_t kAttributeBytesPerResponse = 512;
constexpr size_t kMaxResponses = 64;
constexpr size_t kMaxRecords = 32;
using Bytes = base::span<const uint8_t>;
using UuidBytes = std::array<uint8_t, 16>;

const void* ObjectAddress(id object) { return (__bridge const void*)object; }

struct NativeChannelState {
  const void* device = nullptr;
  const void* delegate = nullptr;
  std::string address;
  BluetoothL2CAPPSM psm = 0;
  BluetoothL2CAPChannelID local_cid = 0;
  BluetoothL2CAPChannelID remote_cid = 0;
  bool incoming = false;
};

NativeChannelState InspectChannel(IOBluetoothL2CAPChannel* channel) {
  IOBluetoothDevice* native_device = channel.device;
  return {ObjectAddress(native_device), ObjectAddress([channel delegate]),
      device::CanonicalizeBluetoothAddress(base::SysNSStringToUTF8(native_device.addressString)),
      channel.PSM, channel.localChannelID, channel.remoteChannelID, bool([channel isIncoming])};
}

base::TimeTicks LogNativeEvent(int64_t query_id, const char* event,
                               id delegate, id channel, IOReturn status, size_t bytes = 0) {
  const auto now = base::TimeTicks::Now();
  // Do not inspect channel properties or dereference the owner's WeakPtr on a
  // native callback thread. Only immutable identity is read here.
  LOG(INFO) << "crQuickShare: Live SDP native event; query_id=" << query_id
            << "; event=" << event << "; delegate=" << ObjectAddress(delegate)
            << "; channel=" << ObjectAddress(channel) << "; status=" << status
            << "; bytes=" << bytes << "; thread_id=" << base::PlatformThread::CurrentId()
            << "; main_thread=" << bool([NSThread isMainThread])
            << "; time_ticks_us=" << now.since_origin().InMicroseconds();
  return now;
}

void LogDelivery(int64_t query_id, const char* event, id delegate, id channel,
                 base::TimeTicks enqueued, bool owner_present) {
  LOG(INFO) << "crQuickShare: Live SDP UI delivery; query_id=" << query_id
            << "; event=" << event << "; delegate=" << ObjectAddress(delegate)
            << "; channel=" << ObjectAddress(channel) << "; owner_present=" << owner_present
            << "; queue_us=" << (base::TimeTicks::Now() - enqueued).InMicroseconds()
            << "; thread_id=" << base::PlatformThread::CurrentId();
}

Bytes DataBytes(NSData* data) {
  // SAFETY: NSData owns exactly length accessible bytes for this call.
  return UNSAFE_BUFFERS(Bytes(static_cast<const uint8_t*>(data.bytes), data.length));
}

struct Element {
  uint8_t type = 0;
  Bytes value;
};

// Read one bounded SDP data element; callers explicitly walk only the schema
// they need, so an untrusted nested sequence never causes recursive parsing.
bool ReadElement(Bytes& input, Element& element) {
  base::SpanReader reader(input);
  auto header = reader.ReadU8BigEndian();
  if (!header) return false;
  uint8_t type = *header >> 3;
  uint8_t size = *header & 7;
  if (type > 8 || (type == 0 && size != 0) ||
      ((type == 1 || type == 2) && size > 4) ||
      (type == 3 && size != 1 && size != 2 && size != 4) ||
      (type == 5 && size != 0) ||
      ((type == 4 || type == 6 || type == 7 || type == 8) && size < 5)) return false;
  uint32_t length = type == 0 ? 0 : 1u << size;
  if (size == 5) {
    auto n = reader.ReadU8BigEndian();
    if (!n) return false;
    length = *n;
  } else if (size == 6) {
    auto n = reader.ReadU16BigEndian();
    if (!n) return false;
    length = *n;
  } else if (size == 7) {
    if (!reader.ReadU32BigEndian(length)) return false;
  }
  auto value = reader.Read(length);
  if (!value) return false;
  element = {type, *value};
  input = reader.remaining_span();
  return true;
}

bool ReadUnsigned(const Element& element, size_t size, uint32_t& value) {
  if (element.type != 1 || element.value.size() != size || size > 4) return false;
  value = 0;
  for (uint8_t byte : element.value) value = (value << 8) | byte;
  return true;
}

std::optional<UuidBytes> ReadUuid(const Element& element) {
  if (element.type != 3) return std::nullopt;
  UuidBytes uuid = {0, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0x80, 0x5f, 0x9b, 0x34, 0xfb};
  if (element.value.size() == 16) {
    base::span(uuid).copy_from(element.value);
  } else if (element.value.size() == 4) {
    base::span(uuid).first<4>().copy_from(element.value);
  } else if (element.value.size() == 2) {
    base::span(uuid).subspan<2, 2>().copy_from(element.value);
  } else return std::nullopt;
  return uuid;
}

UuidBytes ProtocolUuid(uint16_t value) {
  UuidBytes uuid = {0, 0, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value),
                   0, 0, 0x10, 0, 0x80, 0, 0, 0x80, 0x5f, 0x9b, 0x34, 0xfb};
  return uuid;
}

bool HasServiceClass(const Element& classes, const UuidBytes& wanted) {
  if (classes.type != 6) return false;
  Bytes input = classes.value;
  bool found = false;
  while (!input.empty()) {
    Element element;
    if (!ReadElement(input, element)) return false;
    auto uuid = ReadUuid(element);
    if (!uuid) return false;
    found |= *uuid == wanted;
  }
  return found;
}

std::optional<uint8_t> RfcommChannel(const Element& protocols) {
  if (protocols.type != 6) return std::nullopt;
  Bytes input = protocols.value;
  bool l2cap = false;
  std::optional<uint8_t> channel;
  while (!input.empty()) {
    Element descriptor, protocol;
    if (!ReadElement(input, descriptor) || descriptor.type != 6) return std::nullopt;
    Bytes parameters = descriptor.value;
    if (!ReadElement(parameters, protocol)) return std::nullopt;
    auto uuid = ReadUuid(protocol);
    if (!uuid) return std::nullopt;
    if (*uuid == ProtocolUuid(0x0100)) {
      if (l2cap || channel) return std::nullopt;
      l2cap = true;
      if (!parameters.empty()) {
        Element psm;
        uint32_t value;
        if (!ReadElement(parameters, psm) || !ReadUnsigned(psm, 2, value) ||
            value != 3 || !parameters.empty()) return std::nullopt;
      }
    } else if (*uuid == ProtocolUuid(3)) {
      Element port;
      uint32_t value;
      if (!l2cap || channel || !ReadElement(parameters, port) ||
          !ReadUnsigned(port, 1, value) || value == 0 || value > 30 ||
          !parameters.empty()) return std::nullopt;
      channel = static_cast<uint8_t>(value);
    }
  }
  return l2cap ? channel : std::nullopt;
}

bool ParseRoutes(Bytes bytes, const UuidBytes& wanted,
                 std::vector<crquickshare::MacSdpRoute>& routes, size_t& record_count) {
  Element root;
  if (!ReadElement(bytes, root) || root.type != 6 || !bytes.empty()) return false;
  Bytes records = root.value;
  while (!records.empty()) {
    if (++record_count > kMaxRecords) return false;
    Element record;
    if (!ReadElement(records, record) || record.type != 6) return false;
    Bytes attributes = record.value;
    std::optional<uint32_t> handle;
    std::optional<uint8_t> channel;
    std::optional<uint32_t> previous_id;
    bool service_matches = false;
    while (!attributes.empty()) {
      Element id, value;
      uint32_t attribute;
      if (!ReadElement(attributes, id) || !ReadUnsigned(id, 2, attribute) ||
          (previous_id && attribute <= *previous_id) || !ReadElement(attributes, value)) return false;
      previous_id = attribute;
      if (attribute == 0) {
        uint32_t number;
        if (!ReadUnsigned(value, 4, number)) return false;
        handle = number;
      } else if (attribute == 1) service_matches = HasServiceClass(value, wanted);
      else if (attribute == 4) channel = RfcommChannel(value);
    }
    if (handle && service_matches && channel) routes.push_back({*handle, *channel});
  }
  return true;
}

void AppendU16(std::vector<uint8_t>& bytes, uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value >> 8));
  bytes.push_back(static_cast<uint8_t>(value));
}

}  // namespace

@class CrQuickShareSdpDelegate;

namespace crquickshare {

struct MacSdpClientImpl {
  void Query(const std::string& address, const device::BluetoothUUID& uuid,
              MacSdpClient::ResultCallback callback);
  void Cancel(const char* reason);
  void LogState(const char* event, IOBluetoothL2CAPChannel* value = nil,
                 const char* disposition = "observe", CrQuickShareSdpDelegate* source = nil,
                 int64_t query_id = 0);
  bool CurrentCallback(CrQuickShareSdpDelegate* source, int64_t query_id) const;
  const char* ChannelOwnershipError(IOBluetoothL2CAPChannel* value) const;
  const char* HandoffError(IOBluetoothL2CAPChannel* value) const;
  void Opened(IOBluetoothL2CAPChannel* channel, IOReturn status,
               CrQuickShareSdpDelegate* source, int64_t query_id);
  void Received(IOBluetoothL2CAPChannel* channel, NSData* data,
                 CrQuickShareSdpDelegate* source, int64_t query_id);
  void Written(IOBluetoothL2CAPChannel* channel, IOReturn status,
                CrQuickShareSdpDelegate* source, int64_t query_id);
  void Closed(IOBluetoothL2CAPChannel* channel, CrQuickShareSdpDelegate* source, int64_t query_id);
  void SendRequest();
  void Advance();
  void Finish(std::optional<MacSdpRoute> route, const std::string& error);
  void Timeout();

  MacSdpClient::ResultCallback callback;
  IOBluetoothDevice* __strong peer = nil;
  IOBluetoothL2CAPChannel* __strong channel = nil;
  IOBluetoothL2CAPChannel* __strong superseded_channel = nil;
  CrQuickShareSdpDelegate* __strong delegate = nil;
  UuidBytes wanted{};
  std::string address;
  std::vector<uint8_t> request;
  std::vector<uint8_t> received;
  std::vector<uint8_t> attributes;
  std::vector<uint8_t> continuation;
  std::optional<MacSdpRoute> resolved;
  std::optional<uint8_t> cached_channel;
  size_t responses = 0;
  uint16_t transaction = 0;
  bool opened = false;
  bool write_pending = false;
  bool awaiting_response = false;
  bool response_complete = false;
  bool wire_response_confirmed = false;
  bool channel_handoff = false;
  base::TimeTicks started;
  base::OneShotTimer deadline;
  SEQUENCE_CHECKER(sequence);
  base::WeakPtrFactory<MacSdpClientImpl> weak{this};
};

}  // namespace crquickshare

@interface CrQuickShareSdpDelegate : NSObject <IOBluetoothL2CAPChannelDelegate> {
 @private
  base::WeakPtr<crquickshare::MacSdpClientImpl> _owner;
  scoped_refptr<base::SingleThreadTaskRunner> _runner;
  CrQuickShareSdpDelegate* __strong _strongSelf;
  NSMutableArray<IOBluetoothL2CAPChannel*>* _retiringChannels;
  int64_t _queryId;
}
- (instancetype)initWithOwner:(base::WeakPtr<crquickshare::MacSdpClientImpl>)owner queryId:(int64_t)queryId;
- (void)retireChannel:(IOBluetoothL2CAPChannel*)channel reason:(const char*)reason;
- (void)retirementFinished:(IOBluetoothL2CAPChannel*)channel;
@end

@implementation CrQuickShareSdpDelegate
- (instancetype)initWithOwner:(base::WeakPtr<crquickshare::MacSdpClientImpl>)owner queryId:(int64_t)queryId {
  if ((self = [super init])) {
    _owner = std::move(owner);
    _runner = base::SingleThreadTaskRunner::GetCurrentDefault();
    _queryId = queryId;
  }
  return self;
}
- (void)l2capChannelOpenComplete:(IOBluetoothL2CAPChannel*)channel status:(IOReturn)status {
  [[maybe_unused]] NS_VALID_UNTIL_END_OF_SCOPE CrQuickShareSdpDelegate* keepAlive = self;
  const auto entered = LogNativeEvent(_queryId, "open", self, channel, status);
  // Native events may arrive off-sequence or before the open API returns.
  const bool posted = _runner->PostNonNestableTask(FROM_HERE, base::BindOnce(
      [](base::WeakPtr<crquickshare::MacSdpClientImpl> owner,
         CrQuickShareSdpDelegate* delegate, IOBluetoothL2CAPChannel* channel, IOReturn status,
         int64_t query_id, base::TimeTicks entered) {
        LogDelivery(query_id, "open", delegate, channel, entered, bool(owner));
        if (owner) owner->Opened(channel, status, delegate, query_id);
        else [delegate retireChannel:channel reason:"open_callback_owner_gone"];
      }, _owner, self, channel, status, _queryId, entered));
  if (!posted) LogNativeEvent(_queryId, "open_post_failed", self, channel, status);
}
- (void)l2capChannelData:(IOBluetoothL2CAPChannel*)channel data:(void*)data length:(size_t)length {
  [[maybe_unused]] NS_VALID_UNTIL_END_OF_SCOPE CrQuickShareSdpDelegate* keepAlive = self;
  const auto entered = LogNativeEvent(_queryId, "data", self, channel, kIOReturnSuccess, length);
  NSData* copy = length <= kMaxPduBytes ? [NSData dataWithBytes:data length:length] : nil;
  const bool posted = _runner->PostNonNestableTask(FROM_HERE, base::BindOnce(
      [](base::WeakPtr<crquickshare::MacSdpClientImpl> owner,
         CrQuickShareSdpDelegate* delegate, IOBluetoothL2CAPChannel* channel, NSData* data,
         int64_t query_id, base::TimeTicks entered) {
        LogDelivery(query_id, "data", delegate, channel, entered, bool(owner));
        if (owner) owner->Received(channel, data, delegate, query_id);
      }, _owner, self, channel, copy, _queryId, entered));
  if (!posted) LogNativeEvent(_queryId, "data_post_failed", self, channel, kIOReturnSuccess, length);
}
- (void)l2capChannelWriteComplete:(IOBluetoothL2CAPChannel*)channel refcon:(void*)refcon status:(IOReturn)status {
  [[maybe_unused]] NS_VALID_UNTIL_END_OF_SCOPE CrQuickShareSdpDelegate* keepAlive = self;
  const auto entered = LogNativeEvent(_queryId, "write", self, channel, status);
  const bool posted = _runner->PostNonNestableTask(FROM_HERE, base::BindOnce(
      [](base::WeakPtr<crquickshare::MacSdpClientImpl> owner,
         CrQuickShareSdpDelegate* delegate, IOBluetoothL2CAPChannel* channel, IOReturn status,
         int64_t query_id, base::TimeTicks entered) {
        LogDelivery(query_id, "write", delegate, channel, entered, bool(owner));
        if (owner) owner->Written(channel, status, delegate, query_id);
      }, _owner, self, channel, status, _queryId, entered));
  if (!posted) LogNativeEvent(_queryId, "write_post_failed", self, channel, status);
}
- (void)l2capChannelClosed:(IOBluetoothL2CAPChannel*)channel {
  [[maybe_unused]] NS_VALID_UNTIL_END_OF_SCOPE CrQuickShareSdpDelegate* keepAlive = self;
  const auto entered = LogNativeEvent(_queryId, "closed", self, channel, kIOReturnSuccess);
  const bool posted = _runner->PostNonNestableTask(FROM_HERE, base::BindOnce(
      [](base::WeakPtr<crquickshare::MacSdpClientImpl> owner,
         CrQuickShareSdpDelegate* delegate, IOBluetoothL2CAPChannel* channel,
         int64_t query_id, base::TimeTicks entered) {
        LogDelivery(query_id, "closed", delegate, channel, entered, bool(owner));
        if (owner) owner->Closed(channel, delegate, query_id);
        [delegate retirementFinished:channel];
      }, _owner, self, channel, _queryId, entered));
  if (!posted) LogNativeEvent(_queryId, "closed_post_failed", self, channel, kIOReturnSuccess);
}
- (void)retireChannel:(IOBluetoothL2CAPChannel*)channel reason:(const char*)reason {
  CrQuickShareSdpDelegate* __strong keepAlive = self;
  id current = [channel delegate];
  LOG(INFO) << "crQuickShare: Live SDP cleanup requested; query_id=" << _queryId
            << "; reason=" << reason << "; channel=" << ObjectAddress(channel)
            << "; delegate=" << ObjectAddress(self) << "; native_delegate=" << ObjectAddress(current)
            << "; local_cid=" << channel.localChannelID
            << "; disposition=" << (!channel ? "no_channel" :
                (!current || current == self) ? "close_owned_channel" : "skip_foreign_delegate")
            << "; thread_id=" << base::PlatformThread::CurrentId();
  if (channel && (!current || current == self)) {
    IOReturn detach = [channel setDelegate:nil];
    IOReturn close = [channel closeChannel];
    LOG(INFO) << "crQuickShare: Live SDP channel cleanup; query_id=" << _queryId
              << "; channel=" << ObjectAddress(channel) << "; detach_status=" << detach
              << "; close_status=" << close << "; local_cid=" << channel.localChannelID;
  }
  if (channel && [channel delegate] == self) {
    if (!_retiringChannels) _retiringChannels = [[NSMutableArray alloc] init];
    if ([_retiringChannels indexOfObjectIdenticalTo:channel] == NSNotFound)
      [_retiringChannels addObject:channel];
  } else {
    [self retirementFinished:channel];
  }
  if (_retiringChannels.count) _strongSelf = self;
  LOG(INFO) << "crQuickShare: Live SDP retirement state; query_id=" << _queryId
            << "; channel=" << ObjectAddress(channel)
            << "; pending_channels=" << _retiringChannels.count
            << "; self_retained=" << bool(_strongSelf);
  // Keep an already-enqueued, unretained native target alive for one UI turn
  // after successful detachment, matching Chromium's FB13705522 workaround.
  dispatch_async(dispatch_get_main_queue(), ^{ (void)keepAlive; (void)channel; });
}
- (void)retirementFinished:(IOBluetoothL2CAPChannel*)channel {
  if (channel && _retiringChannels) {
    const auto index = [_retiringChannels indexOfObjectIdenticalTo:channel];
    if (index != NSNotFound) [_retiringChannels removeObjectAtIndex:index];
  }
  if (!_retiringChannels.count) _strongSelf = nil;
}
- (void)dealloc {
  LOG(INFO) << "crQuickShare: Live SDP delegate destroyed; query_id=" << _queryId
            << "; delegate=" << (__bridge const void*)self;
}
@end

namespace crquickshare {

void MacSdpClientImpl::LogState(const char* event, IOBluetoothL2CAPChannel* value,
                               const char* disposition, CrQuickShareSdpDelegate* source,
                               int64_t query_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence);
  const auto stored = InspectChannel(channel);
  const auto observed = InspectChannel(value);
  LOG(INFO) << "crQuickShare: Live SDP state; query_id=" << started.since_origin().InMicroseconds()
            << "; event=" << event << "; disposition=" << disposition << "; owner=" << this
            << "; channel=" << ObjectAddress(channel) << "; callback_channel=" << ObjectAddress(value)
            << "; superseded_channel=" << ObjectAddress(superseded_channel)
            << "; channel_handoff=" << channel_handoff
            << "; delegate=" << ObjectAddress(delegate) << "; native_delegate=" << stored.delegate
            << "; delegate_matches=" << bool(delegate && stored.delegate == ObjectAddress(delegate))
            << "; stored_device=" << stored.device << "; stored_address=" << stored.address
            << "; local_cid=" << stored.local_cid << "; remote_cid=" << stored.remote_cid
            << "; psm=" << stored.psm << "; incoming=" << stored.incoming
            << "; callback_device=" << observed.device << "; callback_address=" << observed.address
            << "; callback_delegate=" << observed.delegate
            << "; callback_delegate_matches=" << bool(delegate && observed.delegate == ObjectAddress(delegate))
            << "; callback_local_cid=" << observed.local_cid
            << "; callback_remote_cid=" << observed.remote_cid
            << "; callback_psm=" << observed.psm << "; callback_incoming=" << observed.incoming
            << "; callback_source=" << ObjectAddress(source) << "; callback_query_id=" << query_id
            << "; callback_query_matches=" << CurrentCallback(source, query_id)
            << "; target_address=" << address
            << "; has_callback=" << bool(callback) << "; opened=" << opened
            << "; write_pending=" << write_pending << "; awaiting_response=" << awaiting_response
            << "; elapsed_ms=" << (base::TimeTicks::Now() - started).InMilliseconds()
            << "; thread_id=" << base::PlatformThread::CurrentId();
}

bool MacSdpClientImpl::CurrentCallback(CrQuickShareSdpDelegate* source, int64_t query_id) const {
  return callback && source && source == delegate && query_id == started.since_origin().InMicroseconds();
}

const char* MacSdpClientImpl::ChannelOwnershipError(IOBluetoothL2CAPChannel* value) const {
  if (!value) return "missing_channel";
  const auto state = InspectChannel(value);
  if (!delegate || state.delegate != ObjectAddress(delegate)) return "delegate_mismatch";
  if (state.psm != 1) return "psm_mismatch";
  if (state.incoming) return "incoming_channel";
  if (state.address.empty() || state.address != address) return "device_address_mismatch";
  return nullptr;
}

const char* MacSdpClientImpl::HandoffError(IOBluetoothL2CAPChannel* value) const {
  if (channel_handoff || superseded_channel) return "handoff_already_used";
  if (opened || write_pending || awaiting_response || transaction != 0) return "request_already_started";
  if (ChannelOwnershipError(channel)) return "stored_channel_ownership_mismatch";
  return ChannelOwnershipError(value);
}

void MacSdpClientImpl::Query(const std::string& target, const device::BluetoothUUID& uuid,
                            MacSdpClient::ResultCallback result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence);
  CHECK(!callback);
  callback = std::move(result);
  address = device::CanonicalizeBluetoothAddress(target);
  started = base::TimeTicks::Now();
  if (address.empty()) { Finish(std::nullopt, "Invalid live SDP Bluetooth address"); return; }
  auto bytes = uuid.GetBytes();
  if (!uuid.IsValid() || bytes.size() != wanted.size()) {
    Finish(std::nullopt, "Invalid SDP service UUID");
    return;
  }
  base::span(wanted).copy_from(bytes);
  peer = [IOBluetoothDevice deviceWithAddressString:base::SysUTF8ToNSString(address)];
  if (!peer) { Finish(std::nullopt, "Unable to create live SDP target"); return; }
  IOBluetoothSDPUUID* native_uuid = [IOBluetoothSDPUUID uuidWithBytes:wanted.data()
                                                           length:static_cast<unsigned>(wanted.size())];
  auto* cached = [peer getServiceRecordForUUID:native_uuid];
  BluetoothRFCOMMChannelID cached_port = 0;
  if (cached && [cached getRFCOMMChannelID:&cached_port] == kIOReturnSuccess &&
      cached_port > 0 && cached_port <= 30) cached_channel = cached_port;
  LOG(INFO) << "crQuickShare: Live SDP query started; query_id=" << started.since_origin().InMicroseconds()
            << "; address=" << address << "; service_uuid=" << uuid.canonical_value()
            << "; psm=1; cached_channel=" << (cached_channel ? int(*cached_channel) : -1)
            << "; wall_time_ms=" << base::Time::Now().InMillisecondsSinceUnixEpoch();
  delegate = [[CrQuickShareSdpDelegate alloc] initWithOwner:weak.GetWeakPtr()
                                                 queryId:started.since_origin().InMicroseconds()];
  deadline.Start(FROM_HERE, base::Seconds(10), base::BindOnce(&MacSdpClientImpl::Timeout, weak.GetWeakPtr()));
  IOBluetoothL2CAPChannel* native_channel = nil;
  LogState("open_requested");
  IOReturn status = [peer openL2CAPChannelAsync:&native_channel withPSM:1 delegate:delegate];
  channel = native_channel;
  LogState("open_returned");
  LOG(INFO) << "crQuickShare: Live SDP native open returned; query_id=" << started.since_origin().InMicroseconds()
            << "; status=" << status << "; channel_present=" << bool(channel);
  if (status != kIOReturnSuccess || !channel) {
    Finish(std::nullopt, "Live SDP L2CAP open failed (" + base::NumberToString(status) + ")");
  }
}

void MacSdpClientImpl::Opened(IOBluetoothL2CAPChannel* value, IOReturn status,
                             CrQuickShareSdpDelegate* source, int64_t query_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence);
  const bool current = CurrentCallback(source, query_id);
  const bool superseded = value && value == superseded_channel;
  const char* disposition = !current ? "inactive_or_foreign_query" : superseded ? "superseded_channel" :
      opened && value == channel ? "duplicate_open" : value != channel ? "handoff_candidate" : "accept";
  LogState("open_callback", value, disposition, source, query_id);
  if (!current || superseded || (opened && value == channel)) return;
  LOG(INFO) << "crQuickShare: Live SDP native open callback; query_id=" << started.since_origin().InMicroseconds()
            << "; status=" << status << "; elapsed_ms=" << (base::TimeTicks::Now() - started).InMilliseconds();
  if (status != kIOReturnSuccess) {
    if (value != channel && !ChannelOwnershipError(value))
      [delegate retireChannel:value reason:"open_callback_failed"];
    Finish(std::nullopt, "Live SDP L2CAP connection failed (" + base::NumberToString(status) + ")");
    return;
  }
  if (value != channel) {
    const char* error = HandoffError(value);
    if (error) {
      LogState("channel_handoff_rejected", value, error, source, query_id);
      if (!ChannelOwnershipError(value)) [delegate retireChannel:value reason:"handoff_rejected"];
      Finish(std::nullopt, "Live SDP channel handoff rejected (" + std::string(error) + ")");
      return;
    }
    // On macOS 27, the device-connection completion path may deliver a new
    // wrapper. Keep the returned wrapper alive until both channels are retired;
    // closing it here could interfere with the replacement's native channel.
    superseded_channel = channel;
    channel = value;
    channel_handoff = true;
    LogState("channel_handoff", value, "accepted", source, query_id);
  } else if (const char* error = ChannelOwnershipError(value)) {
    LogState("open_callback_rejected", value, error, source, query_id);
    Finish(std::nullopt, "Live SDP channel ownership mismatch (" + std::string(error) + ")");
    return;
  }
  opened = true;
  SendRequest();
}

void MacSdpClientImpl::SendRequest() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence);
  CHECK(!write_pending && !awaiting_response);
  ++transaction;
  // SDP ServiceSearchAttributeRequest: UUID128, attributes 0x0000/1/4 only.
  request = {0x06};
  AppendU16(request, transaction);
  AppendU16(request, static_cast<uint16_t>(33 + continuation.size()));
  request.insert(request.end(), {0x35, 0x11, 0x1c});
  request.insert(request.end(), wanted.begin(), wanted.end());
  AppendU16(request, kAttributeBytesPerResponse);
  request.insert(request.end(), {0x35, 0x09, 0x09, 0, 0, 0x09, 0, 1, 0x09, 0, 4});
  request.push_back(static_cast<uint8_t>(continuation.size()));
  request.insert(request.end(), continuation.begin(), continuation.end());
  if (request.size() > [channel outgoingMTU]) {
    Finish(std::nullopt, "Live SDP request exceeds L2CAP MTU");
    return;
  }
  write_pending = true;
  awaiting_response = true;
  LOG(INFO) << "crQuickShare: Live SDP write requested; query_id=" << started.since_origin().InMicroseconds()
            << "; transaction=" << transaction << "; bytes=" << request.size()
            << "; continuation_bytes=" << continuation.size();
  IOReturn status = [channel writeAsync:request.data() length:static_cast<UInt16>(request.size()) refcon:nullptr];
  LOG(INFO) << "crQuickShare: Live SDP write returned; query_id=" << started.since_origin().InMicroseconds()
            << "; transaction=" << transaction << "; status=" << status;
  if (status != kIOReturnSuccess) {
    Finish(std::nullopt, "Live SDP write failed (" + base::NumberToString(status) + ")");
  }
}

void MacSdpClientImpl::Received(IOBluetoothL2CAPChannel* value, NSData* data,
                               CrQuickShareSdpDelegate* source, int64_t query_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence);
  const bool current = CurrentCallback(source, query_id);
  LogState("data_callback", value, !current ? "inactive_or_foreign_query" :
      value && value == superseded_channel ? "superseded_channel" :
      value != channel ? "channel_mismatch" : "accept", source, query_id);
  if (!current || !value || value != channel) return;
  if (!awaiting_response || !data || data.length > kMaxPduBytes - received.size()) {
    Finish(std::nullopt, "Invalid or oversized live SDP response"); return;
  }
  Bytes bytes = DataBytes(data);
  received.insert(received.end(), bytes.begin(), bytes.end());
  if (received.size() < 5) return;
  base::SpanReader reader{Bytes(received)};
  uint8_t pdu;
  uint16_t id, length;
  if (!reader.ReadU8BigEndian(pdu) || !reader.ReadU16BigEndian(id) || !reader.ReadU16BigEndian(length) ||
      length > kMaxPduBytes - 5 || id != transaction) {
    Finish(std::nullopt, "Live SDP response header mismatch"); return;
  }
  if (reader.remaining() < length) return;
  if (reader.remaining() != length) { Finish(std::nullopt, "Trailing live SDP response bytes"); return; }
  if (pdu == 1) {
    uint16_t error;
    if (!reader.ReadU16BigEndian(error)) { Finish(std::nullopt, "Malformed live SDP error response"); return; }
    wire_response_confirmed = true;
    Finish(std::nullopt, "Live SDP server error (" + base::NumberToString(error) + ")"); return;
  }
  uint16_t count;
  uint8_t state_size;
  if (pdu != 7 || !reader.ReadU16BigEndian(count) || count > kAttributeBytesPerResponse ||
      count > kMaxAttributeBytes - attributes.size()) {
    Finish(std::nullopt, "Invalid live SDP attribute count"); return;
  }
  auto fragment = reader.Read(count);
  if (!fragment || !reader.ReadU8BigEndian(state_size) || state_size > 16 ||
      reader.remaining() != state_size || (state_size && count == 0) ||
      ++responses > kMaxResponses || (state_size && responses == kMaxResponses)) {
    Finish(std::nullopt, "Invalid live SDP continuation"); return;
  }
  attributes.insert(attributes.end(), fragment->begin(), fragment->end());
  wire_response_confirmed = true;
  auto state = reader.remaining_span();
  continuation.assign(state.begin(), state.end());
  received.clear();
  awaiting_response = false;
  LOG(INFO) << "crQuickShare: Live SDP response received; query_id=" << started.since_origin().InMicroseconds()
            << "; transaction=" << id << "; attribute_bytes=" << count
            << "; total_attribute_bytes=" << attributes.size() << "; continuation_bytes=" << unsigned(state_size);
  if (!state_size) {
    std::vector<MacSdpRoute> routes;
    size_t records = 0;
    if (!ParseRoutes(attributes, wanted, routes, records)) {
      Finish(std::nullopt, "Malformed live SDP service records"); return;
    }
    LOG(INFO) << "crQuickShare: Live SDP inventory; query_id=" << started.since_origin().InMicroseconds()
              << "; records=" << records << "; usable_records=" << routes.size();
    for (const auto& route : routes) {
      LOG(INFO) << "crQuickShare: Live SDP service record; query_id=" << started.since_origin().InMicroseconds()
                << "; handle=" << route.record_handle << "; rfcomm_channel=" << unsigned(route.rfcomm_channel);
      if (resolved && resolved->rfcomm_channel != route.rfcomm_channel) {
        Finish(std::nullopt, "Ambiguous live SDP RFCOMM service channels"); return;
      }
      resolved = route;
    }
    if (!resolved) { Finish(std::nullopt, "Target RFCOMM service not present in live SDP response"); return; }
    response_complete = true;
  }
  Advance();
}

void MacSdpClientImpl::Written(IOBluetoothL2CAPChannel* value, IOReturn status,
                              CrQuickShareSdpDelegate* source, int64_t query_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence);
  const bool current = CurrentCallback(source, query_id);
  LogState("write_callback", value, !current ? "inactive_or_foreign_query" :
      value && value == superseded_channel ? "superseded_channel" : value != channel ? "channel_mismatch" :
      !write_pending ? "no_pending_write" : "accept", source, query_id);
  if (!current || !value || value != channel || !write_pending) return;
  write_pending = false;
  LOG(INFO) << "crQuickShare: Live SDP write callback; query_id=" << started.since_origin().InMicroseconds()
            << "; transaction=" << transaction << "; status=" << status;
  if (status != kIOReturnSuccess) {
    Finish(std::nullopt, "Live SDP asynchronous write failed (" + base::NumberToString(status) + ")"); return;
  }
  Advance();
}

void MacSdpClientImpl::Advance() {
  // Preserve request storage until the native write completion, even if the
  // response arrived first. Never pipeline SDP transactions on this channel.
  if (write_pending || awaiting_response) return;
  if (response_complete) { Finish(resolved, ""); return; }
  SendRequest();
}

void MacSdpClientImpl::Closed(IOBluetoothL2CAPChannel* value,
                             CrQuickShareSdpDelegate* source, int64_t query_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence);
  const bool current = CurrentCallback(source, query_id);
  LogState("closed_callback", value, !current ? "inactive_or_foreign_query" :
      value && value == superseded_channel ? "superseded_channel" :
      value != channel ? "channel_mismatch" : "accept", source, query_id);
  if (current && value && value == channel) Finish(std::nullopt, "Live SDP channel closed before completion");
}

void MacSdpClientImpl::Timeout() {
  LogState("deadline");
  LOG(WARNING) << "crQuickShare: Live SDP deadline; opened=" << opened
               << "; write_pending=" << write_pending << "; awaiting_response=" << awaiting_response;
  Finish(std::nullopt, "Live SDP query timed out");
}

void MacSdpClientImpl::Finish(std::optional<MacSdpRoute> route, const std::string& error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence);
  if (!callback) return;
  LOG(INFO) << "crQuickShare: Live SDP query finished; query_id=" << started.since_origin().InMicroseconds()
            << "; success=" << bool(route) << "; reason=" << error
            << "; live_channel=" << (route ? int(route->rfcomm_channel) : -1)
            << "; cached_channel=" << (cached_channel ? int(*cached_channel) : -1)
            << "; mapping_changed=" << (route && cached_channel ? int(route->rfcomm_channel != *cached_channel) : -1)
            << "; wire_response_confirmed=" << wire_response_confirmed
            << "; channel_handoff=" << channel_handoff
            << "; elapsed_ms=" << (base::TimeTicks::Now() - started).InMilliseconds();
  auto done = std::move(callback);
  Cancel(route ? "query_succeeded" : "query_failed");
  // The callback may destroy the client. Do not access members afterwards.
  std::move(done).Run(route, error);
}

void MacSdpClientImpl::Cancel(const char* reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence);
  if (callback || delegate || channel || superseded_channel) LogState("cancel", nil, reason);
  weak.InvalidateWeakPtrs();
  deadline.Stop();
  callback.Reset();
  [delegate retireChannel:channel reason:reason];
  [delegate retireChannel:superseded_channel reason:"superseded_channel_cleanup"];
  channel = nil;
  superseded_channel = nil;
  delegate = nil;
  peer = nil;
  // Close the native channel before releasing an outstanding write's storage.
  request.clear(); received.clear(); attributes.clear(); continuation.clear();
  resolved.reset(); cached_channel.reset();
  responses = 0; transaction = 0;
  opened = false; write_pending = false; awaiting_response = false; response_complete = false;
  wire_response_confirmed = false;
  channel_handoff = false;
}

MacSdpClient::MacSdpClient() : impl_(std::make_unique<MacSdpClientImpl>()) {}
MacSdpClient::~MacSdpClient() { impl_->Cancel("client_destroyed"); }
void MacSdpClient::Query(const std::string& address, const device::BluetoothUUID& uuid,
                          ResultCallback callback) {
  impl_->Query(address, uuid, std::move(callback));
}
void MacSdpClient::Cancel() { impl_->Cancel("cancel_requested"); }

int RunMacSdpProbe(const std::string& address) {
  const auto target = device::CanonicalizeBluetoothAddress(address);
  if (target.empty()) {
    LOG(ERROR) << "crQuickShare: Live SDP probe requires a valid classic Bluetooth address";
    return 2;
  }
  LOG(INFO) << "crQuickShare: Live SDP probe started; address=" << target
            << "; discovery=0; fast_init=0; rfcomm=0";
  int result = 1;
  base::RunLoop loop;
  MacSdpClient client;
  client.Query(target, device::BluetoothUUID("a82efa21-ae5c-3dde-9bbc-f16da7b16c5a"),
      base::BindOnce([](int* result, base::OnceClosure quit, std::optional<MacSdpRoute> route,
                       const std::string& error) {
        *result = route ? 0 : 1;
        LOG(INFO) << "crQuickShare: Live SDP probe finished; success=" << bool(route) << "; reason=" << error;
        std::move(quit).Run();
      }, &result, loop.QuitClosure()));
  loop.Run();
  // Drain the delegate's one-turn retirement grace before leaving the UI executor.
  base::RunLoop().RunUntilIdle();
  return result;
}

}  // namespace crquickshare
