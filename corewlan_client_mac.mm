// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/corewlan_client_mac.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <CoreWLAN/CoreWLAN.h>
#import <CoreLocation/CoreLocation.h>
#include <dispatch/dispatch.h>
#include <net/if.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"

namespace {
std::atomic<bool> g_wifi_location_allowed{false};
struct WifiLocationSnapshot {
  bool initialized = false;
  int authorization_status = -1;
  bool services_enabled = false;
  bool allowed = false;
  uint64_t revision = 0;
  uint64_t request_checks = 0;
  uint64_t native_requests = 0;
  uint64_t authorization_callbacks = 0;
  bool request_pending = false;
  bool request_deferred = false;
  base::TimeTicks updated;
  const char* source = "uninitialized";
};
struct WifiLocationCache {
  std::mutex mutex;
  WifiLocationSnapshot snapshot;
};
WifiLocationCache& LocationCache() {
  static base::NoDestructor<WifiLocationCache> cache;
  return *cache;
}
const char* LocationStatusName(int status) {
  switch (status) {
    case kCLAuthorizationStatusNotDetermined: return "not_determined";
    case kCLAuthorizationStatusRestricted: return "restricted";
    case kCLAuthorizationStatusDenied: return "denied";
    case kCLAuthorizationStatusAuthorizedAlways: return "authorized_always";
    default: return "unknown";
  }
}
WifiLocationSnapshot ReadLocationCache() {
  auto& cache = LocationCache();
  std::lock_guard lock(cache.mutex);
  return cache.snapshot;
}
void LogLocationState(const char* event, const WifiLocationSnapshot& state) {
  const auto now = base::TimeTicks::Now();
  LOG(INFO) << "crQuickShare: Wi-Fi location state; event=" << event
            << "; initialized=" << state.initialized
            << "; services_enabled=" << state.services_enabled
            << "; authorization_status=" << state.authorization_status
            << "; authorization=" << LocationStatusName(state.authorization_status)
            << "; allowed=" << state.allowed << "; revision=" << state.revision
            << "; snapshot_source=" << state.source
            << "; age_ms=" << (state.updated.is_null() ? -1 : (now - state.updated).InMilliseconds())
            << "; request_checks=" << state.request_checks
            << "; native_requests=" << state.native_requests
            << "; authorization_callbacks=" << state.authorization_callbacks
            << "; request_pending=" << state.request_pending
            << "; request_deferred=" << state.request_deferred
            << "; main_thread=" << [NSThread isMainThread]
            << "; thread_id=" << base::PlatformThread::CurrentId()
            << "; time_ticks_us=" << now.since_origin().InMicroseconds();
}
std::mutex& WifiClientMutex() {
  static base::NoDestructor<std::mutex> mutex;
  return *mutex;
}
}

@interface CRQuickShareWifiPermission : NSObject <CLLocationManagerDelegate> {
  CLLocationManager* _manager;
  uint64_t _requestChecks;
  uint64_t _nativeRequests;
  uint64_t _authorizationCallbacks;
  bool _requestPending;
  bool _requestDeferred;
}
- (void)request;
- (void)cancelDeferredRequest;
- (void)applicationDidBecomeActive:(NSNotification*)notification;
- (CLAuthorizationStatus)refresh:(const char*)source;
- (void)authorizationChanged:(CLLocationManager*)manager;
@end

@implementation CRQuickShareWifiPermission
- (instancetype)init {
  if ((self = [super init])) {
    _manager = [[CLLocationManager alloc] init];
    _manager.delegate = self;
    [[NSNotificationCenter defaultCenter] addObserver:self
        selector:@selector(applicationDidBecomeActive:)
        name:NSApplicationDidBecomeActiveNotification object:NSApp];
    [self refresh:"manager_init"];
  }
  return self;
}
- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
}
- (void)request {
  DCHECK([NSThread isMainThread]);
  ++_requestChecks;
  const auto status = [self refresh:"request_preflight"];
  NSBundle* bundle = [NSBundle mainBundle];
  const bool when_in_use_usage =
      [bundle objectForInfoDictionaryKey:@"NSLocationWhenInUseUsageDescription"] != nil;
  const bool location_usage =
      [bundle objectForInfoDictionaryKey:@"NSLocationUsageDescription"] != nil;
  const bool application_active = NSApp.isActive;
  const char* disposition = status != kCLAuthorizationStatusNotDetermined
      ? "status_already_determined" : (!when_in_use_usage ? "missing_usage_description"
          : (application_active ? "request" : "wait_application_active"));
  LOG(INFO) << "crQuickShare: Wi-Fi location authorization request; disposition=" << disposition
            << "; api=requestWhenInUseAuthorization"
            << "; request_checks=" << _requestChecks << "; native_requests=" << _nativeRequests
            << "; request_pending=" << _requestPending
            << "; bundle_id=" << base::SysNSStringToUTF8(bundle.bundleIdentifier)
            << "; bundle_is_app=" << [bundle.bundleURL.pathExtension isEqualToString:@"app"]
            << "; when_in_use_usage_present=" << when_in_use_usage
            << "; location_usage_present=" << location_usage
            << "; application_present=" << (NSApp != nil)
            << "; application_active=" << application_active
            << "; activation_policy=" << (NSApp ? static_cast<int>(NSApp.activationPolicy) : -1)
            << "; manager_present=" << (_manager != nil)
            << "; delegate_matches=" << (_manager.delegate == self);
  if (status != kCLAuthorizationStatusNotDetermined) {
    _requestDeferred = false;
    return;
  }
  if (!when_in_use_usage) {
    LOG(WARNING) << "crQuickShare: Wi-Fi permission unavailable; reason=missing_location_usage_description";
    return;
  }
  if (!application_active) {
    _requestDeferred = true;
    [self refresh:"request_deferred"];
    return;
  }
  _requestDeferred = false;
  ++_nativeRequests;
  _requestPending = true;
  [self refresh:"request_issued"];
  [_manager requestWhenInUseAuthorization];
  // Returning from the API does not establish that a prompt was displayed.
  [self refresh:"request_returned"];
}
- (void)cancelDeferredRequest {
  DCHECK([NSThread isMainThread]);
  if (!_requestDeferred) return;
  _requestDeferred = false;
  [self refresh:"request_deferred_cancelled"];
}
- (void)applicationDidBecomeActive:(NSNotification*)notification {
  DCHECK([NSThread isMainThread]);
  if (!_requestDeferred) return;
  LOG(INFO) << "crQuickShare: Wi-Fi location authorization resumed; reason=application_active";
  [self request];
}
- (CLAuthorizationStatus)refresh:(const char*)source {
  DCHECK([NSThread isMainThread]);
  const auto status = _manager.authorizationStatus;
  const bool services_enabled = [CLLocationManager locationServicesEnabled];
  if (status != kCLAuthorizationStatusNotDetermined) {
    _requestPending = false;
    _requestDeferred = false;
  }
  auto& cache = LocationCache();
  WifiLocationSnapshot state;
  bool previous_allowed;
  {
    std::lock_guard lock(cache.mutex);
    previous_allowed = cache.snapshot.allowed;
    state = cache.snapshot;
    state.initialized = true;
    state.authorization_status = static_cast<int>(status);
    state.services_enabled = services_enabled;
    state.allowed = services_enabled && status == kCLAuthorizationStatusAuthorizedAlways;
    ++state.revision;
    state.request_checks = _requestChecks;
    state.native_requests = _nativeRequests;
    state.authorization_callbacks = _authorizationCallbacks;
    state.request_pending = _requestPending;
    state.request_deferred = _requestDeferred;
    state.updated = base::TimeTicks::Now();
    state.source = source;
    cache.snapshot = state;
    g_wifi_location_allowed.store(state.allowed);
  }
  LogLocationState(source, state);
  if (previous_allowed != state.allowed) {
    LOG(INFO) << "crQuickShare: Wi-Fi location cache changed; previous_allowed=" << previous_allowed
              << "; allowed=" << state.allowed << "; revision=" << state.revision
              << "; source=" << source;
  }
  return status;
}
- (void)locationManagerDidChangeAuthorization:(CLLocationManager*)manager {
  LOG(INFO) << "crQuickShare: Wi-Fi location native authorization callback; manager="
            << (__bridge const void*)manager << "; main_thread=" << [NSThread isMainThread]
            << "; thread_id=" << base::PlatformThread::CurrentId();
  if (![NSThread isMainThread]) {
    dispatch_async(dispatch_get_main_queue(), ^{ [self authorizationChanged:manager]; });
    return;
  }
  [self authorizationChanged:manager];
}
- (void)authorizationChanged:(CLLocationManager*)manager {
  DCHECK([NSThread isMainThread]);
  if (manager != _manager) {
    LOG(WARNING) << "crQuickShare: Wi-Fi location authorization callback ignored; reason=manager_mismatch";
    return;
  }
  ++_authorizationCallbacks;
  [self refresh:"authorization_callback"];
}
@end

namespace {
CRQuickShareWifiPermission* GetWifiPermission(bool create) {
  DCHECK([NSThread isMainThread]);
  static CRQuickShareWifiPermission* permission = nil;
  if (!permission && create) permission = [[CRQuickShareWifiPermission alloc] init];
  return permission;
}
void RefreshBlockedLocationAccess() {
  // Join runs on the session worker. Refresh on the manager's UI sequence,
  // without creating a manager or requesting authorization during a transfer.
  dispatch_async(dispatch_get_main_queue(), ^{
    CRQuickShareWifiPermission* permission = GetWifiPermission(false);
    if (permission) {
      [permission refresh:"join_blocked_refresh"];
    } else {
      LOG(WARNING) << "crQuickShare: Wi-Fi location refresh skipped; reason=manager_missing";
    }
  });
}
}

namespace crquickshare {
namespace {
void NativeError(NSError* error, const char* operation) {
  if (!error) return;
  // OS descriptions and userInfo can include SSIDs or authentication data.
  LOG(WARNING) << "crQuickShare: CoreWLAN operation failed; operation=" << operation
               << "; error_domain=" << base::SysNSStringToUTF8(error.domain)
               << "; code=" << error.code;
  id underlying = error.userInfo[NSUnderlyingErrorKey];
  if ([underlying isKindOfClass:[NSError class]]) {
    NSError* cause = underlying;
    LOG(WARNING) << "crQuickShare: CoreWLAN underlying error; operation=" << operation
                 << "; error_domain=" << base::SysNSStringToUTF8(cause.domain)
                 << "; code=" << cause.code;
  }
}
NSData* SsidData(const std::string& ssid) {
  return [NSData dataWithBytes:ssid.data() length:ssid.size()];
}
CWNetwork* FindNetwork(NSSet<CWNetwork*>* networks, NSData* ssid) {
  CWNetwork* best = nil;
  for (CWNetwork* network in networks) {
    if (![network.ssidData isEqualToData:ssid]) continue;
    if (!best || network.rssiValue > best.rssiValue) best = network;
  }
  return best;
}
}  // namespace

void RequestCoreWlanAccess() {
  LOG(INFO) << "crQuickShare: Wi-Fi location access requested; main_thread=" << [NSThread isMainThread]
            << "; thread_id=" << base::PlatformThread::CurrentId();
  DCHECK([NSThread isMainThread]);
  if (![NSThread isMainThread]) return;
  CRQuickShareWifiPermission* permission = GetWifiPermission(true);
  [permission request];
}
void CancelDeferredCoreWlanAccessRequest() {
  DCHECK([NSThread isMainThread]);
  if (![NSThread isMainThread]) return;
  CRQuickShareWifiPermission* permission = GetWifiPermission(false);
  [permission cancelDeferredRequest];
}

// Only the atomics and fields protected by mutex are shared with the session
// worker. CoreWLAN objects and network identities belong to Run()'s native task.
struct CoreWlanClient::State {
  explicit State(std::string name) : requested_interface(std::move(name)) {}
  std::string requested_interface;
  std::atomic<bool> stop{false}, current{false};
  std::atomic<unsigned> index{0};
  std::mutex mutex;
  std::condition_variable changed;
  bool task_started = false, join_done = false, done = true, cleanup_ok = true;
  const char* error = "wifi_association_failed";
  base::TimeTicks cleanup_deadline;

  CWWiFiClient* client = nil;
  CWInterface* interface = nil;
  NSData* previous_ssid = nil;
  NSString* previous_bssid = nil;
  bool changed_network = false;
  NSData* target_ssid = nil;
  NSString* target_bssid = nil;

  bool Running(base::TimeTicks deadline) const {
    return !stop.load() && g_wifi_location_allowed.load() &&
        base::TimeTicks::Now() < deadline;
  }
  void Pause() {
    std::unique_lock lock(mutex);
    changed.wait_for(lock, std::chrono::milliseconds(100), [&] { return stop.load(); });
  }
  void Stop(base::TimeTicks deadline) {
    {
      std::lock_guard lock(mutex);
      if (cleanup_deadline.is_null()) cleanup_deadline = deadline;
      stop.store(true);
    }
    changed.notify_all();
  }
  const char* Open(base::TimeTicks deadline) {
    if (!Running(deadline)) return "mac_wifi_permission_or_deadline";
    // Use CWWiFiClient-vended interfaces so App Sandbox access is supported.
    client = [CWWiFiClient sharedWiFiClient];
    if (requested_interface.empty()) {
      NSArray<CWInterface*>* interfaces = client.interfaces;
      if (interfaces.count != 1) return "mac_wifi_interface_selection_required";
      interface = interfaces.firstObject;
    } else {
      interface = [client interfaceWithName:base::SysUTF8ToNSString(requested_interface)];
    }
    if (!interface || !interface.powerOn || !interface.serviceActive) return "mac_wifi_interface_unavailable";
    const char* name = interface.interfaceName.UTF8String;
    if (!name) return "mac_wifi_interface_unavailable";
    index.store(if_nametoindex(name));
    if (!index.load()) return "mac_wifi_interface_unavailable";
    previous_ssid = [interface.ssidData copy];
    previous_bssid = [interface.bssid copy];
    const auto mode = interface.interfaceMode;
    if (mode != kCWInterfaceModeNone && mode != kCWInterfaceModeStation)
      return "mac_wifi_interface_busy";
    if (mode == kCWInterfaceModeStation && !previous_ssid) return "mac_wifi_network_identity_unavailable";
    if (previous_ssid && [previous_ssid isEqualToData:target_ssid]) return nullptr;
    if (previous_ssid && !previous_bssid) return "mac_wifi_network_identity_unavailable";
    // Keep only network identities. Restoring via system AutoJoin avoids reading
    // protected Wi-Fi passwords or enterprise identities before a transfer.
    LOG(INFO) << "crQuickShare: CoreWLAN recovery policy; mode=system_auto_join"
              << "; previous_network_present=" << (previous_ssid != nil)
              << "; keychain_lookup=0";
    return nullptr;
  }
  bool MatchesTarget() const {
    @autoreleasepool {
      if (!g_wifi_location_allowed.load() || !interface.powerOn ||
          ![interface.ssidData isEqualToData:target_ssid]) return false;
      return !target_bssid || [interface.bssid isEqualToString:target_bssid];
    }
  }
  const char* Associate(const std::string& password, base::TimeTicks deadline) {
    if (!Running(deadline)) return "wifi_association_cancelled";
    if (previous_ssid && [previous_ssid isEqualToData:target_ssid]) {
      target_bssid = previous_bssid;
      return MatchesTarget() ? nullptr : "wifi_connection_changed";
    }
    CWNetwork* target = nil;
    LOG(INFO) << "crQuickShare: CoreWLAN join stage; stage=scan_target";
    while (Running(deadline)) {
      NSError* native_error = nil;
      auto networks = [interface scanForNetworksWithSSID:target_ssid includeHidden:YES error:&native_error];
      NativeError(native_error, "scan_target");
      target = FindNetwork(networks, target_ssid);
      if (target) break;
      Pause();
    }
    if (!target || !Running(deadline)) return "wifi_target_not_found_or_cancelled";
    if (!target.bssid || target.ibss) return "wifi_target_identity_unavailable";
    // A user/AutoJoin selection made during scanning takes precedence.
    NSData* now_ssid = interface.ssidData;
    if ((now_ssid && ![now_ssid isEqualToData:previous_ssid] && ![now_ssid isEqualToData:target_ssid]) ||
        (previous_ssid && !now_ssid) || !interface.powerOn)
      return "wifi_connection_changed";
    target_bssid = [target.bssid copy];
    if (!Running(deadline)) return "wifi_association_cancelled";
    changed_network = true;  // Even a failed association can release the old link.
    LOG(INFO) << "crQuickShare: CoreWLAN join stage; stage=associate";
    NSError* native_error = nil;
    const bool associated = [interface associateToNetwork:target
        password:password.empty() ? nil : base::SysUTF8ToNSString(password) error:&native_error];
    NativeError(native_error, "associate");
    if (!associated) return "wifi_association_failed";
    if (!Running(deadline)) return "wifi_association_cancelled";
    return MatchesTarget() ? nullptr : "wifi_connection_changed";
  }
  bool Restore() {
    const auto started = base::TimeTicks::Now();
    auto finish = [&](bool success, const char* reason) {
      LOG(INFO) << "crQuickShare: CoreWLAN recovery finished; mode=system_auto_join"
                << "; success=" << success << "; reason=" << reason
                << "; previous_network_present=" << (previous_ssid != nil)
                << "; elapsed_ms=" << (base::TimeTicks::Now() - started).InMilliseconds();
      return success;
    };
    if (!changed_network) return finish(true, "network_unchanged");
    if (!interface.powerOn) return finish(true, "radio_disabled");
    if (!g_wifi_location_allowed.load()) return finish(false, "location_permission_lost");
    NSData* now_ssid = interface.ssidData;
    if (now_ssid && ![now_ssid isEqualToData:target_ssid])
      return finish(true, [now_ssid isEqualToData:previous_ssid] ? "previous_network_connected" : "other_network_preserved");
    if (now_ssid && target_bssid && ![interface.bssid isEqualToString:target_bssid])
      return finish(true, "target_selection_changed");
    base::TimeTicks deadline;
    {
      std::lock_guard lock(mutex);
      deadline = cleanup_deadline;
    }
    // If a native call returned after cancellation's wait budget, still undo
    // its late association. The caller reports that bounded wait as failed.
    deadline = std::max(deadline, base::TimeTicks::Now() + base::Seconds(8));
    // Disconnect only our hotspot, once. macOS owns saved network credentials
    // and AutoJoin selection. Do not interrupt an AutoJoin already in progress,
    // retry with a nil password, or toggle the radio to force a reconnection.
    if (now_ssid) {
      LOG(INFO) << "crQuickShare: CoreWLAN cleanup stage; stage=disconnect_target";
      [interface disassociate];
    }
    LOG(INFO) << "crQuickShare: CoreWLAN cleanup stage; stage=wait_system_auto_join";
    while (true) {
      if (!interface.powerOn) return finish(true, "radio_disabled");
      if (!g_wifi_location_allowed.load()) return finish(false, "location_permission_lost");
      now_ssid = interface.ssidData;
      if (now_ssid && ![now_ssid isEqualToData:target_ssid])
        return finish(true, [now_ssid isEqualToData:previous_ssid] ? "previous_network_connected" : "other_network_preserved");
      if (now_ssid && target_bssid && ![interface.bssid isEqualToString:target_bssid])
        return finish(true, "target_selection_changed");
      if (!now_ssid && !previous_ssid) return finish(true, "target_disconnected");
      if (base::TimeTicks::Now() >= deadline)
        return finish(false, now_ssid ? "target_still_connected" : "system_auto_join_timed_out");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  void Run(std::string ssid, std::string password, base::TimeTicks deadline) {
    @autoreleasepool {
      target_ssid = SsidData(ssid);
      const auto started = base::TimeTicks::Now();
      // Hold the lease through late native operations and restoration. A new
      // session must never race cleanup left behind by a cancelled association.
      std::unique_lock lease(WifiClientMutex(), std::try_to_lock);
      const char* failure = lease.owns_lock() ? Open(deadline) : "mac_wifi_client_busy";
      if (!failure) failure = Associate(password, deadline);
      password.clear();
      {
        std::lock_guard lock(mutex);
        error = failure;
        join_done = true;
        current.store(!failure);
      }
      changed.notify_all();
      LOG(INFO) << "crQuickShare: CoreWLAN association finished; success=" << !failure
                << "; reason=" << (failure ? failure : "associated")
                << "; interface_index=" << index.load()
                << "; elapsed_ms=" << (base::TimeTicks::Now() - started).InMilliseconds();
      while (!failure && !stop.load()) {
        Pause();
        if (!stop.load() && !MatchesTarget()) {
          current.store(false);
          failure = "wifi_connection_changed";
        }
      }
      current.store(false);
      const bool restored = Restore();
      if (lease.owns_lock()) lease.unlock();
      {
        std::lock_guard lock(mutex);
        cleanup_ok = restored;
        done = true;
      }
      changed.notify_all();
      LOG(INFO) << "crQuickShare: CoreWLAN cleanup finished; network_restored_or_preserved=" << restored;
    }
  }
};

CoreWlanClient::CoreWlanClient(std::string interface_name)
    : state_(std::make_shared<State>(std::move(interface_name))) {}
CoreWlanClient::~CoreWlanClient() {
  state_->Stop(base::TimeTicks::Now() + base::Seconds(8));
}
const char* CoreWlanClient::Join(const std::string& ssid, const std::string& password,
                               int frequency, base::TimeTicks deadline,
                               const std::function<bool()>& active) {
  const auto permission = ReadLocationCache();
  LogLocationState("join_entry", permission);
  if (!permission.allowed) {
    RefreshBlockedLocationAccess();
    return "mac_wifi_location_permission_required";
  }
  if (!state_->requested_interface.empty() &&
      (!base::IsStringASCII(state_->requested_interface) || state_->requested_interface.size() >= IFNAMSIZ ||
       state_->requested_interface.find('\0') != std::string::npos ||
       state_->requested_interface.find('/') != std::string::npos)) return "invalid_wifi_interface";
  if (ssid.empty() || ssid.size() > 32 || password.find('\0') != std::string::npos ||
      !base::IsStringUTF8(password) || !active() || base::TimeTicks::Now() >= deadline)
    return "invalid_wifi_credentials_or_cancelled";
  {
    std::lock_guard lock(state_->mutex);
    if (state_->task_started || state_->stop.load()) return "wifi_client_already_used";
    state_->task_started = true;
    state_->done = false;
  }
  LOG(INFO) << "crQuickShare: CoreWLAN association requested; frequency=" << frequency;
  if (!base::ThreadPool::PostTask(FROM_HERE,
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
          base::BindOnce([](std::shared_ptr<State> state, std::string ssid,
                            std::string password, base::TimeTicks deadline) {
            state->Run(std::move(ssid), std::move(password), deadline);
          }, state_, ssid, password, deadline))) {
    std::lock_guard lock(state_->mutex);
    state_->done = true;
    return "wifi_worker_unavailable";
  }
  while (active() && base::TimeTicks::Now() < deadline) {
    std::unique_lock lock(state_->mutex);
    if (state_->join_done) return state_->error;
    state_->changed.wait_for(lock, std::chrono::milliseconds(50));
  }
  state_->Stop(base::TimeTicks::Now() + base::Seconds(8));
  return "wifi_association_timed_out_or_cancelled";
}
bool CoreWlanClient::IsCurrent() const { return state_->current.load(); }
unsigned CoreWlanClient::interface_index() const { return state_->index.load(); }
bool CoreWlanClient::Cleanup(base::TimeTicks deadline,
                           const std::function<bool()>& keep_restoring) {
  state_->Stop(deadline);
  {
    std::lock_guard lock(state_->mutex);
    if (state_->done) return state_->cleanup_ok;
  }
  while (keep_restoring() && base::TimeTicks::Now() < deadline) {
    std::unique_lock lock(state_->mutex);
    if (state_->done) return state_->cleanup_ok;
    state_->changed.wait_for(lock, std::chrono::milliseconds(50));
  }
  return false;
}
}  // namespace crquickshare
