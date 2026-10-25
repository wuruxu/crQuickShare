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
namespace {
// Experimental application TLV, not an assigned Nearby type. Do not reuse
// standard QR/vendor/capabilities types. Magic guards our own parser against
// unrelated values; it cannot prevent a future upstream type collision.
// Wire: F0 08 "CRSH" version(1) application(1=Files,2=Photos) set(1) index(0..8).
// Index 255 means unavailable. Unknown versions/sets are display-only fallbacks.
constexpr uint8_t kPresentationType = 0xf0;
constexpr std::string_view kPresentationMagic = "CRSH";
}

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
  bool presentation_seen = false;
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
    if (type == kPresentationType && extra.substr(0, length).starts_with(kPresentationMagic)) {
      // Duplicate, future, or invalid display metadata must not hide a valid
      // standard Quick Share endpoint or affect file-transfer compatibility.
      if (presentation_seen) result.presentation = {};
      else if (length == 8 && uint8_t(extra[4]) == 1 &&
               (uint8_t(extra[5]) == 1 || uint8_t(extra[5]) == 2)) {
        result.presentation.application = static_cast<Application>(uint8_t(extra[5]));
        if (uint8_t(extra[6]) == 1 && uint8_t(extra[7]) < 9)
          result.presentation.avatar = uint8_t(extra[7]);
      }
      presentation_seen = true;
    } else if (type != 1 && type != 2) ++result.unknown_tlvs;
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
std::string MakeEndpointInfo(const std::string& name, Presentation presentation) {
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
  std::string result(bytes.begin(), bytes.end());
  if (presentation.application == Application::kFiles || presentation.application == Application::kPhotos) {
    // At most 108 bytes with the 80-byte name cap, below BLE's 131-byte limit.
    result += char(kPresentationType); result += char(8);
    result += kPresentationMagic; result += char(1);
    result += char(presentation.application); result += char(1);
    result += char(presentation.avatar >= 0 && presentation.avatar < 9 ? presentation.avatar : 255);
  }
  return result;
}
std::string EndpointName(const std::string& info) {
  const char* reason = nullptr;
  auto parsed = ParseEndpointInfo(info, reason);
  return parsed ? parsed->name.value_or("") : std::string();
}
Presentation EndpointPresentation(const std::string& info) {
  const char* reason = nullptr;
  auto parsed = ParseEndpointInfo(info, reason);
  return parsed ? parsed->presentation : Presentation();
}
int EndpointDeviceType(const std::string& info) {
  const char* reason = nullptr;
  auto parsed = ParseEndpointInfo(info, reason);
  return parsed ? parsed->device_type : 0;
}
}  // namespace crquickshare
