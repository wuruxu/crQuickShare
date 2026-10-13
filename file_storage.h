// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_FILE_STORAGE_H_
#define CRQUICKSHARE_FILE_STORAGE_H_
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "crQuickShare/quick_share_service.h"
namespace crquickshare {
inline constexpr size_t kMaxTransferFiles = 1024;
inline constexpr int64_t kMaxTransferBytes = int64_t{1} << 40;
// Protocol-relative paths, independent of the host path separator.
bool SafeTransferName(const std::string& name);
std::optional<std::vector<std::string>> FolderParts(const std::string& folder);
std::string RelativeName(const std::string& folder, const std::string& leaf);
struct StorageError {
  Failure failure = Failure::kStorage;
  std::string message;
  bool no_space = false;
};
struct SourceFile {
  std::string name, parent;
  int64_t size = 0;
  base::File handle;  // Opened once; never reopen the original path after consent.
};
struct SourceSelection {
  std::vector<SourceFile> files;
  int64_t total = 0;
  size_t entries = 0;
};
struct ReceivePath { std::string name, parent; };
// Owns all destination handles and an isolated staging directory. Destruction
// closes handles before rolling back an uncommitted batch. Commit never replaces
// an existing directory. After Commit succeeds, cleanup must retain saved files.
class ReceiveBatch {
 public:
  virtual ~ReceiveBatch();
  virtual bool Write(const std::string& relative_path, base::span<const uint8_t> bytes,
                     StorageError& error) = 0;
  virtual bool FinishFile(const std::string& relative_path, StorageError& error) = 0;
  virtual bool Commit(StorageError& error) = 0;
  virtual base::FilePath saved_directory() const = 0;
  // A successful commit may still have unconfirmed parent-directory durability.
  virtual bool directory_durable() const = 0;
};
class FileStorage {
 public:
  virtual ~FileStorage();
  // Worker-only, bounded recursive traversal, no symlink following. An inactive
  // callback returns false without an error. Partial selections are discarded.
  virtual bool Collect(const std::vector<base::FilePath>& paths,
                       const std::function<bool()>& active, SourceSelection& selection,
                       StorageError& error) = 0;
  // Paths have passed protocol validation; backends also enforce safe local IO.
  virtual std::unique_ptr<ReceiveBatch> CreateBatch(const base::FilePath& destination,
      int64_t total, const std::vector<ReceivePath>& paths, StorageError& error) = 0;
};
std::unique_ptr<FileStorage> CreateFileStorage();
}  // namespace crquickshare
#endif
