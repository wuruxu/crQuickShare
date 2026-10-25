// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_ENDPOINT_INFO_CODEC_H_
#define CRQUICKSHARE_ENDPOINT_INFO_CODEC_H_
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
namespace crquickshare {
// Display hints only: never authorize transfers or identify trusted devices.
// Avatar set 1: cat, dog, flower, horse, cupcake, awesome, soccer, ninja, sun/cloud.
enum class Application : uint8_t { kUnknown = 0, kFiles = 1, kPhotos = 2 };
struct Presentation {
  Application application = Application::kUnknown;
  int avatar = -1;  // Unknown/absent uses the standard device icon.
  bool operator==(const Presentation&) const = default;
};
struct EndpointInfo {
  Presentation presentation;
  std::optional<std::string> name;
  int device_type = 0;
  unsigned unknown_tlvs = 0;
};
// Validates core information and bounded extension TLVs, ignoring a lone
// trailing extension byte like upstream.
// Callers retain the original bytes: upstream normalizes the parsed version.
std::optional<EndpointInfo> ParseEndpointInfo(std::string_view bytes, const char*& reason);
std::string MakeEndpointInfo(const std::string& name, Presentation presentation = {});
Presentation EndpointPresentation(const std::string& info);
std::string EndpointName(const std::string& info);
int EndpointDeviceType(const std::string& info);
}
#endif
