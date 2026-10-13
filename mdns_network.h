// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_MDNS_NETWORK_H_
#define CRQUICKSHARE_MDNS_NETWORK_H_
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace crquickshare {
struct MdnsInterface {
  uint32_t address = 0, mask = 0;  // Host-order IPv4, independent of native APIs.
  unsigned index = 0;
  bool operator==(const MdnsInterface&) const = default;
};
struct MdnsDestination { uint32_t address = 0; uint16_t port = 5353; };
struct MdnsDatagram {
  std::string bytes;
  MdnsDestination source;
  unsigned interface_index = 0;
};
class MdnsNetwork {
 public:
  virtual ~MdnsNetwork();
  virtual std::optional<std::vector<MdnsInterface>> Interfaces() = 0;
  virtual bool Changed() = 0;
  virtual bool Open(const std::vector<MdnsInterface>& interfaces) = 0;
  virtual void Close() = 0;
  virtual bool ready() const = 0;
  // Nonblocking, bounded by one datagram: 1 = packet, 0 = drained/ignored,
  // -1 = native error. Truncated packets must never reach the DNS parser.
  virtual int Receive(MdnsDatagram& datagram) = 0;
  virtual bool Send(const std::string& bytes, const MdnsInterface& link,
                    const MdnsDestination* destination = nullptr) = 0;
  virtual int error() const = 0;
};
std::unique_ptr<MdnsNetwork> CreateMdnsNetwork();
}  // namespace crquickshare
#endif
