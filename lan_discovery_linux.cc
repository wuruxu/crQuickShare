// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/mdns_network.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include "base/containers/span.h"
#include <optional>
#include <tuple>
#include "base/logging.h"
#include "base/files/scoped_file.h"

namespace crquickshare {
namespace {
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
  base::ScopedFD fd_, network_events_;
};
NativeMdnsNetwork::NativeMdnsNetwork() {
    network_events_.reset(socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE));
    if (network_events_.is_valid()) {
      sockaddr_nl address{}; address.nl_family = AF_NETLINK;
      address.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE;
      if (bind(network_events_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address))) network_events_.reset();
    }
}
NativeMdnsNetwork::~NativeMdnsNetwork() = default;
int NativeMdnsNetwork::error() const { return errno; }
std::optional<std::vector<MdnsInterface>> NativeMdnsNetwork::Interfaces() {
    std::vector<MdnsInterface> result;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return std::nullopt;
    for (auto* i = list; i; i = i->ifa_next) {
      if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET ||
          !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_RUNNING) || !(i->ifa_flags & IFF_MULTICAST) || (i->ifa_flags & IFF_LOOPBACK)) continue;
      MdnsInterface v{ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr),
                  ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr), if_nametoindex(i->ifa_name)};
      result.push_back(v);
    }
    freeifaddrs(list);
    std::ranges::sort(result, [](const MdnsInterface& a, const MdnsInterface& b) {
      return std::tie(a.index, a.address, a.mask) < std::tie(b.index, b.address, b.mask);
    });
    return result;
}
bool NativeMdnsNetwork::Changed() {
  bool changed = false;
  if (network_events_.is_valid()) {
    std::array<char, 8192> events;
    for (int n = 0; n < 16; ++n) {
      const auto count = recv(network_events_.get(), events.data(), events.size(), 0);
      if (count <= 0) { if (count < 0 && errno == ENOBUFS) changed = true; break; }
      changed = true;
    }
  }
  return changed;
}
bool NativeMdnsNetwork::Open(const std::vector<MdnsInterface>& interfaces) {
  Close();
    if (interfaces.empty()) { LOG(WARNING) << "crQuickShare: mDNS socket setup failed; operation=no_multicast_interface; error=" << 0; return false; }
    fd_.reset(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!fd_.is_valid()) { LOG(WARNING) << "crQuickShare: mDNS socket setup failed; operation=socket; error=" << errno; return false; }
    int yes = 1, ttl = 255;
    setsockopt(fd_.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(fd_.get(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    if (setsockopt(fd_.get(), IPPROTO_IP, IP_PKTINFO, &yes, sizeof(yes))) { LOG(WARNING) << "crQuickShare: mDNS socket setup failed; operation=packet_info; error=" << errno; fd_.reset(); return false; }
    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET; bind_address.sin_port = htons(5353);
    if (bind(fd_.get(), reinterpret_cast<sockaddr*>(&bind_address), sizeof(bind_address))) { LOG(WARNING) << "crQuickShare: mDNS socket setup failed; operation=bind_5353; error=" << errno; fd_.reset(); return false; }
    bool joined = false;
    for (const auto& i : interfaces) {
      ip_mreq group{}; inet_pton(AF_INET, "224.0.0.251", &group.imr_multiaddr);
      group.imr_interface.s_addr = htonl(i.address);
      if (setsockopt(fd_.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof(group)) == 0) joined = true;
      else LOG(WARNING) << "crQuickShare: mDNS multicast join failed; interface=" << i.index << "; errno=" << errno;
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
    iovec io{bytes.data(), bytes.size()};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(in_pktinfo))]{};
    msghdr msg{}; msg.msg_name = &source; msg.msg_namelen = sizeof(source);
    msg.msg_iov = &io; msg.msg_iovlen = 1; msg.msg_control = control; msg.msg_controllen = sizeof(control);
    auto size = recvmsg(fd_.get(), &msg, 0);
    if (size < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    if (!size) return 0;
    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || msg.msg_controllen > sizeof(control)) return 0;
    auto control_bytes = base::as_byte_span(control).first(msg.msg_controllen);
    unsigned interface_index = 0;
    for (auto* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
      size_t offset = reinterpret_cast<const char*>(c) - control;
      if (offset > control_bytes.size() || c->cmsg_len > control_bytes.size() - offset) break;
      if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO && c->cmsg_len >= CMSG_LEN(sizeof(in_pktinfo))) {
        in_pktinfo packet_info{};
        base::byte_span_from_ref(packet_info)
            .copy_from(control_bytes.subspan(offset + CMSG_LEN(0), sizeof(packet_info)));
        interface_index = packet_info.ipi_ifindex;
      }
    }
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
  if (!destination) {
    if (setsockopt(fd_.get(), IPPROTO_IP, IP_MULTICAST_IF, &local, sizeof(local))) return false;
    return sendto(fd_.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL,
        reinterpret_cast<sockaddr*>(&target), sizeof(target)) == static_cast<ssize_t>(bytes.size());
  }
  iovec io{const_cast<char*>(bytes.data()), bytes.size()};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(in_pktinfo))]{};
  msghdr msg{}; msg.msg_name = &target; msg.msg_namelen = sizeof(target);
  msg.msg_iov = &io; msg.msg_iovlen = 1; msg.msg_control = control; msg.msg_controllen = sizeof(control);
  auto* c = CMSG_FIRSTHDR(&msg); c->cmsg_level = IPPROTO_IP; c->cmsg_type = IP_PKTINFO; c->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
  in_pktinfo packet_info{}; packet_info.ipi_ifindex = link.index; packet_info.ipi_spec_dst = local;
  base::as_writable_byte_span(control).subspan(CMSG_LEN(0), sizeof(packet_info)).copy_from(base::byte_span_from_ref(packet_info));
  return sendmsg(fd_.get(), &msg, MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size());
}
}  // namespace
std::unique_ptr<MdnsNetwork> CreateMdnsNetwork() { return std::make_unique<NativeMdnsNetwork>(); }
}  // namespace crquickshare
