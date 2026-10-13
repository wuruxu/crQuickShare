// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/lan_discovery.h"
#include "base/rand_util.h"
#include "base/strings/string_util.h"
namespace crquickshare {
LanDiscovery::~LanDiscovery() = default;
std::string MakeEndpointInfo(const std::string& name) {
  // Public visibility, laptop; two salt bytes and fourteen metadata-key bytes.
  std::string out(1, char(3 << 1));
  out += base::RandBytesAsString(16);
  std::string clipped;
  base::TruncateUTF8ToByteSize(name, 80, &clipped);
  if (clipped.empty()) clipped = "crQuickShare";
  out += char(clipped.size()); out += clipped;
  return out;
}
std::string EndpointName(const std::string& info) {
  if (info.size() < 18 || (uint8_t(info[0]) >> 5) > 1 || (info[0] & 0x10)) return {};
  size_t n = uint8_t(info[17]);
  if (!n || info.size() < 18 + n) return {};
  auto name = info.substr(18, n);
  if (!base::IsStringUTF8(name)) return {};
  for (unsigned char c : name) if (c < 32 || c == 127) return {};
  return name;
}
}
