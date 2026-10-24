// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/platform_socket.h"
#include <algorithm>
#include <array>
#include <climits>
#include <tuple>
#include <utility>
#include "base/compiler_specific.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include <iphlpapi.h>
#include <mswsock.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <unistd.h>
#endif
namespace crquickshare::platform {
namespace {
bool Initialize() {
#if BUILDFLAG(IS_WIN)
  static const bool initialized = [] { WSADATA data{}; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
  return initialized;
#else
  return true;
#endif
}
}
Socket::Socket() = default;
Socket::Socket(Handle handle) : handle_(handle) {}
Socket::~Socket() { reset(); }
Socket::Socket(Socket&& other) : handle_(std::exchange(other.handle_, kInvalid)) {}
Socket& Socket::operator=(Socket&& other) {
  if (this != &other) reset(std::exchange(other.handle_, kInvalid));
  return *this;
}
Handle Socket::get() const { return handle_; }
bool Socket::is_valid() const { return handle_ != kInvalid; }
void Socket::reset(Handle handle) {
  if (is_valid()) {
#if BUILDFLAG(IS_WIN)
    closesocket(handle_);
#else
    close(handle_);
#endif
  }
  handle_ = handle;
}
int Error() {
#if BUILDFLAG(IS_WIN)
  return WSAGetLastError();
#else
  return errno;
#endif
}
bool Interrupted(int error) {
#if BUILDFLAG(IS_WIN)
  return error == WSAEINTR;
#else
  return error == EINTR;
#endif
}
bool Pending(int error) {
#if BUILDFLAG(IS_WIN)
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY || Interrupted(error);
#else
  return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS || Interrupted(error);
#endif
}
bool Configure(Handle socket) {
#if BUILDFLAG(IS_WIN)
  u_long nonblocking = 1;
  return ioctlsocket(socket, FIONBIO, &nonblocking) == 0 && SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0);
#else
  const int flags = fcntl(socket, F_GETFL);
  int yes = 1;
  return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0 &&
      fcntl(socket, F_SETFD, FD_CLOEXEC) == 0 && setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) == 0;
#endif
}
Socket Open(int type, int family) {
  if (!Initialize()) return {};
  Socket socket(::socket(family, type, 0));
  if (!socket.is_valid() || !Configure(socket.get())) return {};
  return socket;
}
int Poll(PollFd& descriptor, int timeout_ms) {
#if BUILDFLAG(IS_WIN)
  return WSAPoll(&descriptor, 1, std::max(0, timeout_ms));
#else
  return poll(&descriptor, 1, std::max(0, timeout_ms));
#endif
}
int SetOption(Handle socket, int level, int option, const void* value, size_t length) {
  return setsockopt(socket, level, option, reinterpret_cast<const char*>(value), static_cast<Length>(length));
}
int Read(Handle socket, base::span<char> bytes) {
  return static_cast<int>(recv(socket, bytes.data(), static_cast<int>(std::min<size_t>(bytes.size(), INT_MAX)), 0));
}
int Write(Handle socket, base::span<const char> bytes) {
  return static_cast<int>(send(socket, bytes.data(), static_cast<int>(std::min<size_t>(bytes.size(), INT_MAX)), 0));
}
int SendTo(Handle socket, base::span<const char> bytes, const sockaddr_in& destination) {
  return static_cast<int>(sendto(socket, bytes.data(), static_cast<int>(bytes.size()), 0,
      reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)));
}
int SocketFailure(Handle socket) {
  int error = 0; Length size = sizeof(error);
  return getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) == 0 ? error : Error();
}
std::optional<std::vector<Interface>> Interfaces() {
  if (!Initialize()) return std::nullopt;
  std::vector<Interface> result;
#if BUILDFLAG(IS_WIN)
  ULONG size = 16384;
  std::vector<uint8_t> buffer;
  IP_ADAPTER_ADDRESSES* adapters = nullptr;
  ULONG status = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
    if (size < sizeof(IP_ADAPTER_ADDRESSES) || size > 1024 * 1024) return std::nullopt;
    buffer.resize(size);
    // SAFETY: The API takes a byte-sized allocation through a structure pointer.
    // buffer owns at least size bytes, which is passed as the writable capacity.
    // Refresh the pointer after each resize; after success the allocation stays
    // alive and unchanged while traversing the API-provided linked records.
    adapters = UNSAFE_BUFFERS(reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()));
    status = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, adapters, &size);
  }
  if (status == ERROR_NO_DATA) return result;
  if (status != NO_ERROR) return std::nullopt;
  for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || (adapter->Flags & IP_ADAPTER_NO_MULTICAST)) continue;
    for (auto* entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
      if (!entry->Address.lpSockaddr || entry->Address.lpSockaddr->sa_family != AF_INET || entry->OnLinkPrefixLength > 32) continue;
      in_addr mask{}; mask.s_addr = htonl(entry->OnLinkPrefixLength ? 0xffffffffu << (32 - entry->OnLinkPrefixLength) : 0);
      result.push_back({reinterpret_cast<sockaddr_in*>(entry->Address.lpSockaddr)->sin_addr, mask, adapter->IfIndex});
    }
  }
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list)) return std::nullopt;
  for (auto* entry = list; entry; entry = entry->ifa_next) {
    if (!entry->ifa_addr || !entry->ifa_netmask || entry->ifa_addr->sa_family != AF_INET ||
        !(entry->ifa_flags & IFF_UP) || !(entry->ifa_flags & IFF_RUNNING) || !(entry->ifa_flags & IFF_MULTICAST) || (entry->ifa_flags & IFF_LOOPBACK)) continue;
    result.push_back({reinterpret_cast<sockaddr_in*>(entry->ifa_addr)->sin_addr,
        reinterpret_cast<sockaddr_in*>(entry->ifa_netmask)->sin_addr, if_nametoindex(entry->ifa_name)});
  }
  freeifaddrs(list);
#endif
  std::ranges::sort(result, [](const Interface& a, const Interface& b) {
    return std::tie(a.index, a.address.s_addr, a.mask.s_addr) < std::tie(b.index, b.address.s_addr, b.mask.s_addr);
  });
  return result;
}
bool EnablePacketInfo(Handle socket) {
  int yes = 1;
#if BUILDFLAG(IS_WIN)
  return SetOption(socket, IPPROTO_IP, IP_PKTINFO, &yes, sizeof(yes)) == 0;
#else
  return SetOption(socket, IPPROTO_IP, IP_RECVIF, &yes, sizeof(yes)) == 0;
#endif
}
int ReceiveDatagram(Handle socket, base::span<char> bytes, sockaddr_in& source,
                    unsigned& interface_index, bool& truncated) {
  interface_index = 0; truncated = false;
#if BUILDFLAG(IS_WIN)
  GUID guid = WSAID_WSARECVMSG; LPFN_WSARECVMSG receive = nullptr; DWORD returned = 0;
  if (WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &receive, sizeof(receive), &returned, nullptr, nullptr)) return -1;
  WSABUF buffer{static_cast<ULONG>(bytes.size()), bytes.data()};
  alignas(WSACMSGHDR) char control[256]{};
  WSAMSG message{}; message.name = reinterpret_cast<sockaddr*>(&source); message.namelen = sizeof(source);
  message.lpBuffers = &buffer; message.dwBufferCount = 1; message.Control = {sizeof(control), control};
  DWORD count = 0;
  if (receive(socket, &message, &count, nullptr, nullptr)) return -1;
  truncated = (message.dwFlags & (MSG_TRUNC | MSG_CTRUNC)) != 0;
  // SAFETY: Winsock wrote the bounded control buffer; validate each record before accessing its body.
  UNSAFE_BUFFERS({
    for (auto* c = WSA_CMSG_FIRSTHDR(&message); c; c = WSA_CMSG_NXTHDR(&message, c)) {
      if (c->cmsg_len < WSA_CMSG_LEN(0) || reinterpret_cast<char*>(c) + c->cmsg_len > control + sizeof(control)) break;
      if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO && c->cmsg_len >= WSA_CMSG_LEN(sizeof(IN_PKTINFO)))
        interface_index = reinterpret_cast<const IN_PKTINFO*>(WSA_CMSG_DATA(c))->ipi_ifindex;
    }
  });
#else
  iovec buffer{bytes.data(), bytes.size()};
  alignas(cmsghdr) char control[256]{};
  msghdr message{}; message.msg_name = &source; message.msg_namelen = sizeof(source);
  message.msg_iov = &buffer; message.msg_iovlen = 1; message.msg_control = control; message.msg_controllen = sizeof(control);
  const auto count = recvmsg(socket, &message, 0);
  if (count < 0) return -1;
  truncated = (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0;
  // SAFETY: recvmsg bounds the control buffer; each sockaddr_dl must fit its record.
  UNSAFE_BUFFERS({
    for (auto* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
      if (c->cmsg_len < CMSG_LEN(0) || reinterpret_cast<char*>(c) + c->cmsg_len > control + sizeof(control)) break;
      if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVIF && c->cmsg_len >= CMSG_LEN(offsetof(sockaddr_dl, sdl_data)))
        interface_index = reinterpret_cast<const sockaddr_dl*>(CMSG_DATA(c))->sdl_index;
    }
  });
#endif
  return static_cast<int>(count);
}
int SendUnicast(Handle socket, base::span<const char> bytes, const sockaddr_in& destination, unsigned interface_index) {
#if BUILDFLAG(IS_WIN)
  const ULONG index = htonl(interface_index);
  constexpr int option = IP_UNICAST_IF;
#else
  const unsigned index = interface_index;
  constexpr int option = IP_BOUND_IF;
#endif
  if (SetOption(socket, IPPROTO_IP, option, &index, sizeof(index))) return -1;
  const int sent = SendTo(socket, bytes, destination);
  const int send_error = sent < 0 ? Error() : 0;
  const unsigned reset = 0;
  SetOption(socket, IPPROTO_IP, option, &reset, sizeof(reset));
  if (sent < 0) {
#if BUILDFLAG(IS_WIN)
    WSASetLastError(send_error);
#else
    errno = send_error;
#endif
  }
  return sent;
}
}  // namespace crquickshare::platform
