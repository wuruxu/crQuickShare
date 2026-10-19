// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_ENDPOINT_INFO_CODEC_H_
#define CRQUICKSHARE_ENDPOINT_INFO_CODEC_H_
#include <optional>
#include <string>
#include <string_view>
namespace crquickshare {
struct EndpointInfo {
  std::optional<std::string> name;
  int device_type = 0;
  unsigned unknown_tlvs = 0;
};
// Validates core information and bounded extension TLVs, ignoring a lone
// trailing extension byte like upstream.
// Callers retain the original bytes: upstream normalizes the parsed version.
std::optional<EndpointInfo> ParseEndpointInfo(std::string_view bytes, const char*& reason);
std::string MakeEndpointInfo(const std::string& name);
std::string EndpointName(const std::string& info);
int EndpointDeviceType(const std::string& info);
}
#endif
