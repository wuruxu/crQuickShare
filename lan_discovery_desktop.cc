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
  platform::Socket fd_;
};
NativeMdnsNetwork::NativeMdnsNetwork() {
}
NativeMdnsNetwork::~NativeMdnsNetwork() = default;
int NativeMdnsNetwork::error() const { return platform::Error(); }
std::optional<std::vector<MdnsInterface>> NativeMdnsNetwork::Interfaces() {
    auto native = platform::Interfaces();
    if (!native) return std::nullopt;
    std::vector<MdnsInterface> result;
    for (const auto& entry : *native) result.push_back({ntohl(entry.address.s_addr), ntohl(entry.mask.s_addr), entry.index});
    return result;
}
bool NativeMdnsNetwork::Changed() {
  bool network_dirty = false;
  return network_dirty;
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
