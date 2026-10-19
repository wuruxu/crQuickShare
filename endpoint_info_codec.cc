// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/endpoint_info_codec.h"
#include <algorithm>
#include <cstdint>
#include <vector>
#include "base/rand_util.h"
#include "base/strings/string_util.h"
#include "third_party/nearby/src/sharing/advertisement.h"
namespace crquickshare {
std::optional<EndpointInfo> ParseEndpointInfo(std::string_view bytes, const char*& reason) {
  reason = "sharing_size";
  if (bytes.size() < 17 || bytes.size() > 512) return std::nullopt;
  const uint8_t header = static_cast<uint8_t>(bytes.front());
  reason = "sharing_version";
  if ((header >> 5) > 1) return std::nullopt;
  size_t end = 17;
  if (!(header & 0x10)) {
    reason = "sharing_public_name";
    if (bytes.size() < 18) return std::nullopt;
    const size_t length = static_cast<uint8_t>(bytes[17]);
    if (!length || length > bytes.size() - 18) return std::nullopt;
    const auto name = bytes.substr(18, length);
    if (!base::IsStringUTF8(name) || std::ranges::any_of(name,
        [](unsigned char c) { return c < 32 || c == 127; })) return std::nullopt;
    end = 18 + length;
  }
  EndpointInfo result;
  // Match upstream: ignore a lone trailing byte without a TLV length field.
  // Complete headers still require bounded values, including unknown types.
  auto extra = bytes.substr(end);
  reason = "sharing_extensions";
  while (extra.size() >= 2) {
    const uint8_t type = static_cast<uint8_t>(extra[0]);
    const size_t length = static_cast<uint8_t>(extra[1]);
    extra.remove_prefix(2);
    if (length > extra.size()) return std::nullopt;
    if (type == 2 && length != 1) { reason = "sharing_vendor_length"; return std::nullopt; }
    if (type != 1 && type != 2) ++result.unknown_tlvs;
    extra.remove_prefix(length);
  }
  // Only the core is needed for discovery. TLVs with complete headers were
  // validated above; the caller keeps all original bytes for association.
  std::vector<uint8_t> core(bytes.begin(), bytes.begin() + end);
  auto parsed = nearby::sharing::Advertisement::FromEndpointInfo(core);
  if (!parsed) return std::nullopt;
  result.name = parsed->device_name();
  result.device_type = static_cast<int>(parsed->device_type());
  reason = "recognized";
  return result;
}
std::string MakeEndpointInfo(const std::string& name) {
  std::string clipped;
  base::TruncateUTF8ToByteSize(name, 80, &clipped);
  if (clipped.empty() || !base::IsStringUTF8(clipped) || std::ranges::any_of(clipped,
      [](unsigned char c) { return c < 32 || c == 127; })) clipped = "crQuickShare";
  const auto random = base::RandBytesAsString(16);
  auto advertisement = nearby::sharing::Advertisement::NewInstance(
      std::vector<uint8_t>(random.begin(), random.begin() + 2),
      std::vector<uint8_t>(random.begin() + 2, random.end()),
      nearby::sharing::ShareTargetType::kLaptop, clipped, 0);
  const auto bytes = advertisement->ToEndpointInfo();
  return std::string(bytes.begin(), bytes.end());
}
std::string EndpointName(const std::string& info) {
  const char* reason = nullptr;
  auto parsed = ParseEndpointInfo(info, reason);
  return parsed ? parsed->name.value_or("") : std::string();
}
int EndpointDeviceType(const std::string& info) {
  const char* reason = nullptr;
  auto parsed = ParseEndpointInfo(info, reason);
  return parsed ? parsed->device_type : 0;
}
}  // namespace crquickshare
