// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_ADVERTISEMENT_CODEC_H_
#define CRQUICKSHARE_ADVERTISEMENT_CODEC_H_
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include "crQuickShare/quick_share_service.h"
namespace crquickshare {
struct AdvertisementDetails {
  uint16_t psm = 0;
  size_t declared_payload = 0, available_payload = 0;
  size_t instant_connect_bytes = 0, unknown_tlvs = 0;
};
struct DctResult {
  Peer peer;
  uint16_t psm = 0;
  uint8_t dedup = 0;
  bool truncated = false;
  unsigned unknown_fields = 0;
};
struct GattHeader { std::string bytes; unsigned slots; };
std::optional<DctResult> ParseDct(std::string_view bytes, const char*& reason);
std::optional<Peer> ParseAdvertisement(std::string_view bytes, const char*& reason,
                                     AdvertisementDetails* details = nullptr);
bool IsOnLostAdvertisement(std::string_view bytes);
std::optional<GattHeader> ParseGattHeader(std::string_view bytes);
// Regular FEF3 service data, shared by the Linux and Windows receiver.
std::string EncodeAdvertisement(const Peer& peer, std::string_view mac);
std::string EncodeGattHeader();
}
#endif
