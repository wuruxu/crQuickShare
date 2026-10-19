// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/advertisement_codec.h"
#include <algorithm>
#include <set>
#include <utility>
#include "third_party/nearby/src/connections/implementation/mediums/advertisements/data_element.h"
#include "base/base64url.h"
#include "base/rand_util.h"
#include "base/strings/string_util.h"
#include "crQuickShare/endpoint_info_codec.h"
#include "third_party/nearby/src/connections/implementation/ble_advertisement.h"
#include "third_party/nearby/src/connections/implementation/mediums/ble_v2/ble_advertisement.h"
#include "third_party/nearby/src/connections/implementation/mediums/ble_v2/instant_on_lost_advertisement.h"
namespace crquickshare {
namespace {
constexpr std::string_view kServiceHash = "\xfc\x9f\x5e";
}
// Nearby advertisements/dct_advertisement.cc and data_element.cc. DCT has
// no Sharing salt/key hash: keep its local identity separate from FEF3/LAN.
std::optional<DctResult> ParseDct(std::string_view bytes, const char*& reason) {
  reason = "dct_size";
  if (bytes.empty() || bytes.size() > 512) return std::nullopt;
  reason = "dct_version_or_reserved_bits";
  if (static_cast<uint8_t>(bytes.front()) != 0x20) return std::nullopt;
  bytes.remove_prefix(1);
  std::set<unsigned> fields;
  DctResult result;
  const nearby::ByteArray data{std::string(bytes)};
  nearby::StreamReader reader(&data);
  while (reader.IsAvailable(1)) {
    reason = "dct_element_header";
    const auto element = nearby::connections::advertisements::DataElement::FromStreamReader(reader);
    if (!element) return std::nullopt;
    const unsigned type = element->type();
    const size_t length = element->length();
    const std::string value = element->value();
    reason = "dct_element_bounds_or_duplicate";
    if (fields.size() >= 32 || !fields.insert(type).second) return std::nullopt;
    if (type == 5) {
      // HKDF-SHA256(secret="NearbySharing", salt="DCT Protocol",
      // info="Service ID Hash", length=2), matching upstream.
      reason = "dct_service_hash";
      if (value != "\x6d\xfd") return std::nullopt;
    } else if (type == 4) {
      reason = "dct_psm";
      if (length != 2) return std::nullopt;
      result.psm = (static_cast<uint8_t>(value[0]) << 8) | static_cast<uint8_t>(value[1]);
      if (!result.psm) return std::nullopt;
    } else if (type == 7) {
      reason = "dct_device_information";
      if (length < 2 || length > 8) return std::nullopt;
      result.truncated = static_cast<uint8_t>(value[0]) & 0x80;
      result.dedup = static_cast<uint8_t>(value[0]) & 0x7f;
      result.peer.name = std::string(value.substr(1));
      if (!base::IsStringUTF8(result.peer.name) || std::ranges::any_of(result.peer.name,
          [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; })) return std::nullopt;
    } else {
      // DE has explicit lengths, unlike unknown FEF3 extra-mask bits.
      ++result.unknown_fields;
    }
  }
  reason = "dct_required_fields";
  if (!fields.contains(5) || !fields.contains(4) || !fields.contains(7)) return std::nullopt;
  result.peer.ble_only = true;
  if (result.truncated) result.peer.name += "…";
  reason = "recognized";
  return result;
}

// Upstream medium and Connections parsers, with bounded compatibility adapters.
// FEF3 is shared by Nearby services. UUID presence alone is never sufficient.
// GATT headers are resolved separately; a header, DCT or an unknown format
// alone must not produce a selectable Quick Share device.
std::optional<Peer> ParseAdvertisement(std::string_view bytes, const char*& reason,
                                     AdvertisementDetails* details) {
  if (details) *details = {};
  reason = "medium_size";
  if (bytes.size() < 2 || bytes.size() > 512) return std::nullopt;
  const std::string original(bytes);
  const uint8_t header = static_cast<uint8_t>(bytes.front());
  const unsigned version = header >> 5, socket_version = (header >> 2) & 7;
  reason = "medium_version";
  if (version < 1 || version > 2) return std::nullopt;
  reason = "socket_version";
  if (socket_version < 1 || socket_version > 2) return std::nullopt;
  const bool fast = header & 2;
  bytes.remove_prefix(1);
  size_t length = 0;
  if (fast) {
    // The compact layout flag does not prove this came from a legacy 31-byte
    // advertising packet. BlueZ has reported 92-byte service data with it set.
    // Use the encoded length and the overall cap, not the legacy 27-byte cap.
    length = static_cast<uint8_t>(bytes.front()); bytes.remove_prefix(1);
  } else {
    reason = "regular_medium_service_hash";
    if (bytes.size() < 7 || bytes.substr(0, 3) != kServiceHash) return std::nullopt;
    bytes.remove_prefix(3);
    for (size_t i = 0; i < 4; ++i) length = (length << 8) | static_cast<uint8_t>(bytes[i]);
    bytes.remove_prefix(4);
  }
  reason = "medium_payload_length";
  if (details) { details->declared_payload = length; details->available_payload = bytes.size(); }
  if (length > bytes.size()) return std::nullopt;
  // The upstream fast length reader promotes signed char on some platforms.
  // Normalize lengths >=128 to the regular medium envelope without changing
  // the embedded Connections payload or its compact-layout flag below.
  std::string medium_bytes = original;
  if (fast && length >= 128) {
    medium_bytes.assign(1, static_cast<char>(header & ~2));
    medium_bytes += kServiceHash;
    for (int shift : {24, 16, 8, 0}) medium_bytes += static_cast<char>((length >> shift) & 255);
    medium_bytes += bytes;
  }
  const auto medium = nearby::connections::mediums::BleAdvertisement::CreateBleAdvertisement(
      nearby::ByteArray(std::move(medium_bytes)));
  reason = "medium_extensions";
  if (!medium.ok()) return std::nullopt;
  const std::string payload(medium->GetData());
  std::string_view connection(payload);
  auto extra = bytes.substr(length);
  // Upstream accepts unknown masks/trailing bytes. Retain our strict envelope
  // policy; field values and their length checks are handled by upstream.
  if (!extra.empty()) {
    if (extra.size() < 2) return std::nullopt;
    extra.remove_prefix(2);
    if (!extra.empty()) {
      const uint8_t mask = static_cast<uint8_t>(extra.front());
      reason = "unknown_medium_extension_mask";
      if (mask & ~3) return std::nullopt;
      const size_t expected = 1 + ((mask & 1) ? 2 : 0) +
          ((mask & 2) ? 1 + medium->GetRxInstantConnectionAdv().size() : 0);
      reason = "medium_extension_trailing_bytes";
      if (extra.size() != expected) return std::nullopt;
    }
  }
  if (details) {
    details->psm = medium->GetPsm();
    details->instant_connect_bytes = medium->GetRxInstantConnectionAdv().size();
  }
  // V1, P2P_POINT_TO_POINT is the strategy used by NearbySharing.
  reason = "connection_version_or_strategy";
  if (connection.empty() || static_cast<uint8_t>(connection.front()) != 0x23) return std::nullopt;
  // Upstream's fast parser caps endpoint info at 17 bytes. Extended BLE
  // packets from phones use the compact layout with a public name as well.
  // Adapt only that wire layout to regular form, then use the same parser.
  std::string regular;
  bool parse_fast = fast;
  if (fast && connection.size() >= 6 &&
      static_cast<uint8_t>(connection[5]) > 17) {
    const size_t info_end = 6 + static_cast<uint8_t>(connection[5]);
    reason = "endpoint_info_length";
    if (info_end > connection.size()) return std::nullopt;
    regular = std::string(connection.substr(0, 1));
    regular += kServiceHash;
    regular += connection.substr(1, info_end - 1);
    regular.append(6, '\0');  // Compact advertisements carry no classic address.
    regular += connection.substr(info_end);
    parse_fast = false;
  }
  using Advertisement = nearby::connections::BleAdvertisement;
  const auto parsed = Advertisement::CreateBleAdvertisement(
      parse_fast, nearby::ByteArray(regular.empty() ? std::string(connection) : regular));
  reason = "connection_payload";
  if (!parsed.ok()) return std::nullopt;
  reason = "connection_service_hash";
  if (!fast && std::string(parsed->GetServiceIdHash()) != kServiceHash) return std::nullopt;
  Peer result;
  result.id = parsed->GetEndpointId();
  result.endpoint_info = std::string(parsed->GetEndpointInfo());
  // Upstream ignores trailing bytes. Keep our bounded extension policy, using
  // decoded sizes rather than decoding the connection fields a second time.
  const size_t core_size = (fast ? 6 : 15) + result.endpoint_info.size();
  const size_t extension_size = connection.size() - core_size;
  const size_t uwb_size = parsed->GetUwbAddress().size();
  reason = "connection_extensions";
  if (extension_size && extension_size != 1 + uwb_size &&
      (fast || extension_size != 2 + uwb_size)) return std::nullopt;
  if (!fast) {
    result.classic_bluetooth_address = parsed->GetBluetoothMacAddress();
    if (result.classic_bluetooth_address == "FF:FF:FF:FF:FF:FF")
      result.classic_bluetooth_address.clear();
  }
  auto info = ParseEndpointInfo(result.endpoint_info, reason);
  if (!info) return std::nullopt;
  result.name = info->name.value_or("");
  if (details) details->unknown_tlvs = info->unknown_tlvs;
  reason = "recognized";
  result.ble_only = true;
  return result;
}
// Nearby instant_on_lost_advertisement.cc uses a different header layout:
// type=1, version=1, then a count and four bytes per advertisement hash.
// Classify it before attempting the medium header. It contains no device name.
bool IsOnLostAdvertisement(std::string_view bytes) {
  if (bytes.size() < 6 || bytes.size() > 22) return false;
  return nearby::connections::mediums::InstantOnLostAdvertisement::CreateFromBytes(
      absl::string_view(bytes.data(), bytes.size())).ok();
}
// Nearby ble_advertisement_header.cc: V2, 10-byte bloom filter, 4-byte
// advertisement hash, optional PSM. Older implementations base64url-encode it.
std::optional<GattHeader> ParseGattHeader(std::string_view bytes) {
  std::string decoded;
  if (bytes.size() != 15 && bytes.size() != 17) {
    if (bytes.size() > 24 || !base::Base64UrlDecode(bytes,
        base::Base64UrlDecodePolicy::IGNORE_PADDING, &decoded)) return std::nullopt;
    bytes = decoded;
  }
  if (bytes.size() != 15 && bytes.size() != 17) return std::nullopt;
  const uint8_t header = static_cast<uint8_t>(bytes[0]);
  const unsigned slots = header & 15;
  if ((header >> 5) != 2 || !slots) return std::nullopt;
  // bloom_filter.cc uses MurmurHash3_x64_128("NearbySharing", seed=0),
  // low64=0x798579c252d5fe64, five combined signed-32 hashes modulo 80.
  // The bloom match is only a connection hint; each read must match both
  // service hashes in the full advertisement before it is exposed to the UI.
  for (unsigned bit : {57u, 56u, 5u, 28u, 33u}) {
    if (!(static_cast<uint8_t>(bytes[1 + bit / 8]) & (1u << (bit % 8)))) return std::nullopt;
  }
  return GattHeader{std::string(bytes), slots};
}

std::string EncodeAdvertisement(const Peer& peer, std::string_view mac) {
  const char* reason = nullptr;
  if (peer.id.size() != 4 || peer.endpoint_info.size() > 131 ||
      mac.size() != 6 || !ParseEndpointInfo(peer.endpoint_info, reason)) return {};
  std::string payload(1, '\x23');
  payload += kServiceHash; payload += peer.id;
  payload += static_cast<char>(peer.endpoint_info.size()); payload += peer.endpoint_info;
  payload += mac; payload.append(2, '\0');
  std::string bytes(1, '\x48'); bytes += kServiceHash;
  const uint32_t size = payload.size();
  for (int shift : {24, 16, 8, 0}) bytes += static_cast<char>((size >> shift) & 255);
  bytes += payload; bytes += base::RandBytesAsString(2);
  return bytes;
}
std::string EncodeGattHeader() {
  std::string raw(15, '\0'); raw[0] = '\x41';
  for (unsigned bit : {57u, 56u, 5u, 28u, 33u}) raw[1 + bit / 8] |= 1u << (bit % 8);
  raw.replace(11, 4, base::RandBytesAsString(4));
  std::string encoded;
  base::Base64UrlEncode(raw, base::Base64UrlEncodePolicy::OMIT_PADDING, &encoded);
  return encoded;
}
}  // namespace crquickshare
