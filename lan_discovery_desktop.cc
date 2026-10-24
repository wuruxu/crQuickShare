// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/mdns_network.h"
#include <algorithm>
#include <array>
#include "base/containers/span.h"
#include <optional>
#include <tuple>
#include "base/logging.h"
#include "crQuickShare/platform_socket.h"
#if BUILDFLAG(IS_WIN)
#include <atomic>
#include <iphlpapi.h>
#elif BUILDFLAG(IS_MAC)
#include <SystemConfiguration/SystemConfiguration.h>
#include <atomic>
#include "base/apple/scoped_cftyperef.h"
#include "base/apple/scoped_dispatch_object.h"
#include "base/memory/ref_counted.h"
#endif

namespace crquickshare {
namespace {
#if BUILDFLAG(IS_MAC)
using NetworkChangeFlag = base::RefCountedData<std::atomic<bool>>;
const void* RetainNetworkChangeFlag(const void* context) {
  static_cast<const NetworkChangeFlag*>(context)->AddRef();
  return context;
}
void ReleaseNetworkChangeFlag(const void* context) {
  static_cast<const NetworkChangeFlag*>(context)->Release();
}
void NetworkChanged(SCDynamicStoreRef, CFArrayRef, void* context) {
  static_cast<NetworkChangeFlag*>(context)->data.store(true, std::memory_order_relaxed);
}
#endif
class NativeMdnsNetwork final : public MdnsNetwork {
 public:
  NativeMdnsNetwork();
  ~NativeMdnsNetwork() override;
  std::optional<std::vector<MdnsInterface>> Interfaces() override;
  bool Changed() override;
  bool Open(const std::vector<MdnsInterface>& interfaces) override;
  void Close() override;
  bool ready() const override;
  int Receive(MdnsDatagram& datagram) override;
  bool Send(const std::string& bytes, const MdnsInterface& link,
            const MdnsDestination* destination) override;
  int error() const override;
 private:
  platform::Socket fd_;
#if BUILDFLAG(IS_WIN)
  static void WINAPI InterfaceChanged(void* context, MIB_IPINTERFACE_ROW*, MIB_NOTIFICATION_TYPE) {
    static_cast<NativeMdnsNetwork*>(context)->network_dirty_.store(true, std::memory_order_relaxed);
  }
  static void WINAPI AddressChanged(void* context, MIB_UNICASTIPADDRESS_ROW*, MIB_NOTIFICATION_TYPE) {
    static_cast<NativeMdnsNetwork*>(context)->network_dirty_.store(true, std::memory_order_relaxed);
  }
  std::atomic<bool> network_dirty_{false};
  HANDLE interface_notification_ = nullptr;
  HANDLE address_notification_ = nullptr;
#elif BUILDFLAG(IS_MAC)
  // The store retains this flag independently of the owning discovery object,
  // so an in-flight callback remains safe during shutdown.
  scoped_refptr<NetworkChangeFlag> network_dirty_ = base::MakeRefCounted<NetworkChangeFlag>();
  base::apple::ScopedDispatchObject<dispatch_queue_t> network_queue_;
  base::apple::ScopedCFTypeRef<SCDynamicStoreRef> network_store_;
#endif
};
NativeMdnsNetwork::NativeMdnsNetwork() {
#if BUILDFLAG(IS_WIN)
  const auto interface_error = NotifyIpInterfaceChange(AF_INET, &InterfaceChanged, this, FALSE, &interface_notification_);
  const auto address_error = NotifyUnicastIpAddressChange(AF_INET, &AddressChanged, this, FALSE, &address_notification_);
  if (interface_error != NO_ERROR || address_error != NO_ERROR)
    LOG(WARNING) << "crQuickShare: mDNS network notification registration failed; interface_error="
                 << interface_error << "; address_error=" << address_error << "; polling_fallback=1";
#elif BUILDFLAG(IS_MAC)
  SCDynamicStoreContext context{0, network_dirty_.get(), &RetainNetworkChangeFlag,
                               &ReleaseNetworkChangeFlag, nullptr};
  network_store_.reset(SCDynamicStoreCreate(nullptr, CFSTR("crQuickShare.mDNS"), &NetworkChanged, &context));
  if (!network_store_) {
    LOG(WARNING) << "crQuickShare: mDNS network notification registration failed; operation=dynamic_store; system_error="
                 << SCError() << "; polling_fallback=1";
    return;
  }
  const void* keys[] = {CFSTR("State:/Network/Global/IPv4"), CFSTR("State:/Network/Interface")};
  // Include per-interface link and Wi-Fi changes even when IPv4 is unchanged,
  // as well as IPv4 changes on services that are not the primary route.
  const void* patterns[] = {CFSTR("^State:/Network/Interface/[^/]+/(IPv4|Link|AirPort)$"),
                            CFSTR("^State:/Network/Service/[^/]+/IPv4$")};
  base::apple::ScopedCFTypeRef<CFArrayRef> notification_keys(
      CFArrayCreate(nullptr, keys, std::size(keys), &kCFTypeArrayCallBacks));
  base::apple::ScopedCFTypeRef<CFArrayRef> notification_patterns(
      CFArrayCreate(nullptr, patterns, std::size(patterns), &kCFTypeArrayCallBacks));
  if (!notification_keys || !notification_patterns ||
      !SCDynamicStoreSetNotificationKeys(network_store_.get(), notification_keys.get(), notification_patterns.get())) {
    LOG(WARNING) << "crQuickShare: mDNS network notification registration failed; operation=notification_keys; system_error="
                 << SCError() << "; polling_fallback=1";
    network_store_.reset();
    return;
  }
  network_queue_.reset(dispatch_queue_create("crQuickShare.mDNS.network", DISPATCH_QUEUE_SERIAL));
  if (!network_queue_ || !SCDynamicStoreSetDispatchQueue(network_store_.get(), network_queue_.get())) {
    LOG(WARNING) << "crQuickShare: mDNS network notification registration failed; operation=dispatch_queue; system_error="
                 << SCError() << "; polling_fallback=1";
    network_store_.reset();
    network_queue_.reset();
    return;
  }
  LOG(INFO) << "crQuickShare: mDNS network notifications ready; backend=SystemConfiguration";
#endif
}
NativeMdnsNetwork::~NativeMdnsNetwork() {
#if BUILDFLAG(IS_WIN)
  // Cancel from the owning discovery thread, never from a notification callback.
  // Cancellation waits for callbacks while their atomic state is still alive.
  if (interface_notification_) CancelMibChangeNotify2(interface_notification_);
  if (address_notification_) CancelMibChangeNotify2(address_notification_);
#elif BUILDFLAG(IS_MAC)
  if (network_store_) SCDynamicStoreSetDispatchQueue(network_store_.get(), nullptr);
#endif
}
int NativeMdnsNetwork::error() const { return platform::Error(); }
std::optional<std::vector<MdnsInterface>> NativeMdnsNetwork::Interfaces() {
    auto native = platform::Interfaces();
    if (!native) return std::nullopt;
    std::vector<MdnsInterface> result;
    for (const auto& entry : *native) result.push_back({ntohl(entry.address.s_addr), ntohl(entry.mask.s_addr), entry.index});
    return result;
}
bool NativeMdnsNetwork::Changed() {
#if BUILDFLAG(IS_WIN)
  return network_dirty_.exchange(false, std::memory_order_relaxed);
#elif BUILDFLAG(IS_MAC)
  return network_dirty_->data.exchange(false, std::memory_order_relaxed);
#else
  return false;
#endif
}
bool NativeMdnsNetwork::Open(const std::vector<MdnsInterface>& interfaces) {
  Close();
    if (interfaces.empty()) { LOG(WARNING) << "crQuickShare: mDNS socket setup failed; operation=no_multicast_interface; error=" << 0; return false; }
    fd_ = platform::Open(SOCK_DGRAM);
    if (!fd_.is_valid()) { LOG(WARNING) << "crQuickShare: mDNS socket setup failed; operation=socket; error=" << platform::Error(); return false; }
    int yes = 1;
#if BUILDFLAG(IS_MAC)
    const unsigned char ttl = 255;
    platform::SetOption(fd_.get(), SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#else
    const int ttl = 255;
#endif
    platform::SetOption(fd_.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    platform::SetOption(fd_.get(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    if (!platform::EnablePacketInfo(fd_.get())) { LOG(WARNING) << "crQuickShare: mDNS socket setup failed; operation=packet_info; error=" << platform::Error(); fd_.reset(); return false; }
    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET; bind_address.sin_port = htons(5353);
    if (bind(fd_.get(), reinterpret_cast<sockaddr*>(&bind_address), sizeof(bind_address))) { LOG(WARNING) << "crQuickShare: mDNS socket setup failed; operation=bind_5353; error=" << platform::Error(); fd_.reset(); return false; }
    bool joined = false;
    for (const auto& i : interfaces) {
      ip_mreq group{}; inet_pton(AF_INET, "224.0.0.251", &group.imr_multiaddr);
      group.imr_interface.s_addr = htonl(i.address);
      if (platform::SetOption(fd_.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof(group)) == 0) joined = true;
      else LOG(WARNING) << "crQuickShare: mDNS multicast join failed; interface=" << i.index << "; system_error=" << platform::Error();
    }
    if (!joined) { LOG(WARNING) << "crQuickShare: mDNS socket setup failed; operation=no_joined_interface; error=" << 0; fd_.reset(); }
    else LOG(INFO) << "crQuickShare: mDNS ready; interface_addresses=" << interfaces.size();
  return ready();
}
void NativeMdnsNetwork::Close() { fd_.reset(); }
bool NativeMdnsNetwork::ready() const { return fd_.is_valid(); }
int NativeMdnsNetwork::Receive(MdnsDatagram& datagram) {
  std::array<char, 9000> bytes;
    sockaddr_in source{};
    unsigned interface_index = 0; bool truncated = false;
    const int size = platform::ReceiveDatagram(fd_.get(), base::span<char>(bytes), source, interface_index, truncated);
    if (size < 0) return platform::Pending(platform::Error()) ? 0 : -1;
    if (!size || truncated) return 0;
  if (!interface_index) return 0;
  datagram.bytes.assign(bytes.data(), static_cast<size_t>(size));
  datagram.source = {ntohl(source.sin_addr.s_addr), ntohs(source.sin_port)};
  datagram.interface_index = interface_index;
  return 1;
}
bool NativeMdnsNetwork::Send(const std::string& bytes, const MdnsInterface& link,
                             const MdnsDestination* destination) {
  sockaddr_in target{}; target.sin_family = AF_INET;
  target.sin_port = htons(destination ? destination->port : 5353);
  target.sin_addr.s_addr = htonl(destination ? destination->address : 0xe00000fb);
  in_addr local{}; local.s_addr = htonl(link.address);
  if (destination) return platform::SendUnicast(fd_.get(), base::span<const char>(bytes), target, link.index) == static_cast<int>(bytes.size());
  if (platform::SetOption(fd_.get(), IPPROTO_IP, IP_MULTICAST_IF, &local, sizeof(local))) return false;
  return platform::SendTo(fd_.get(), base::span<const char>(bytes), target) == static_cast<int>(bytes.size());
}
}  // namespace
std::unique_ptr<MdnsNetwork> CreateMdnsNetwork() { return std::make_unique<NativeMdnsNetwork>(); }
}  // namespace crquickshare
