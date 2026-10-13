// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/file_storage.h"
#include <utility>
#include "base/strings/string_util.h"
namespace crquickshare {
ReceiveBatch::~ReceiveBatch() = default;
FileStorage::~FileStorage() = default;
bool SafeTransferName(const std::string& name) {
  if (name.empty() || name.size() > 240 || !base::IsStringUTF8(name) || name == "." || name == "..") return false;
  for (unsigned char c : name) if (c < 32 || c == 127 || c == '/' || c == '\\') return false;
  return true;
}
// Only canonical, relative protocol paths are accepted. Backslashes and drive
// prefixes are rejected rather than interpreted differently on another OS.
std::optional<std::vector<std::string>> FolderParts(const std::string& folder) {
  std::vector<std::string> parts;
  if (folder.empty()) return parts;
  if (folder.size() > 1024) return std::nullopt;
  size_t start = 0;
  while (start <= folder.size()) {
    const auto end = folder.find('/', start);
    auto part = folder.substr(start, end == std::string::npos ? end : end - start);
    if (!SafeTransferName(part) || part.find(':') != std::string::npos || parts.size() >= 32) return std::nullopt;
    parts.push_back(std::move(part));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return parts;
}
std::string RelativeName(const std::string& folder, const std::string& leaf) {
  return folder.empty() ? leaf : folder + "/" + leaf;
}
}
