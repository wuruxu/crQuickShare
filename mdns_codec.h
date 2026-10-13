// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_MDNS_CODEC_H_
#define CRQUICKSHARE_MDNS_CODEC_H_
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace crquickshare::mdns {
constexpr size_t kMaxPacket = 9000;
struct Question { std::string name; uint16_t type = 0, klass = 1; };
struct Record {
  std::string name;
  uint16_t type = 0, klass = 1;
  uint32_t ttl = 0;
  // Uncompressed RDATA. target preserves the original spelling for Nearby's
  // case-sensitive base64 instance labels; comparisons use canonical DNS names.
  std::string data, target;
};
struct Packet {
  uint16_t id = 0, flags = 0;
  std::vector<Question> questions;
  std::vector<Record> answers, authority, additional;
};
std::string Canonical(std::string_view name);
void U16(std::string& out, uint16_t value);
void U32(std::string& out, uint32_t value);
uint16_t Get16(std::string_view bytes, size_t offset);
std::string Name(std::string_view name);
std::optional<Packet> Parse(std::string_view bytes);
std::string Encode(const Packet& packet);
std::string DataKey(const Record& record);
bool Same(const Record& first, const Record& second);
// RFC 6762 8.2: unsigned byte ordering, excluding the cache-flush bit.
std::string TieKey(const Record& record);
bool LessBytes(std::string_view first, std::string_view second);
}  // namespace crquickshare::mdns
#endif
