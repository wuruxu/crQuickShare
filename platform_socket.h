// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_PLATFORM_SOCKET_H_
#define CRQUICKSHARE_PLATFORM_SOCKET_H_
#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#endif
#include <cstdint>
#include <optional>
#include <vector>
#include "base/containers/span.h"
namespace crquickshare::platform {
#if BUILDFLAG(IS_WIN)
using Handle = SOCKET;
using Length = int;
using PollFd = WSAPOLLFD;
inline constexpr Handle kInvalid = INVALID_SOCKET;
#else
using Handle = int;
using Length = socklen_t;
using PollFd = pollfd;
inline constexpr Handle kInvalid = -1;
#endif
class Socket {
 public:
  Socket();
  explicit Socket(Handle handle);
  ~Socket();
  Socket(Socket&& other);
  Socket& operator=(Socket&& other);
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Handle get() const;
  bool is_valid() const;
  void reset(Handle handle = kInvalid);
 private:
  Handle handle_ = kInvalid;
};
Socket Open(int type, int family = AF_INET);
bool Configure(Handle socket);
int Error();
bool Pending(int error);
bool Interrupted(int error);
int Poll(PollFd& descriptor, int timeout_ms);
int SetOption(Handle socket, int level, int option, const void* value, size_t length);
int Read(Handle socket, base::span<char> bytes);
int Write(Handle socket, base::span<const char> bytes);
int SendTo(Handle socket, base::span<const char> bytes, const sockaddr_in& destination);
int SocketFailure(Handle socket);
struct Interface { in_addr address; in_addr mask; unsigned index = 0; };
std::optional<std::vector<Interface>> Interfaces();
bool EnablePacketInfo(Handle socket);
int ReceiveDatagram(Handle socket, base::span<char> bytes, sockaddr_in& source,
                    unsigned& interface_index, bool& truncated);
int SendUnicast(Handle socket, base::span<const char> bytes,
                const sockaddr_in& destination, unsigned interface_index);
}  // namespace crquickshare::platform
#endif
