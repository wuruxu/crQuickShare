// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/file_storage.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <map>
#include <set>
#include <utility>
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/system/sys_info.h"
namespace crquickshare {
namespace {
constexpr size_t kMaxFiles = kMaxTransferFiles;
constexpr int64_t kMaxTotal = kMaxTransferBytes;
bool StorageFail(StorageError& error, const char* message, Failure failure = Failure::kStorage) {
  error.message = message; error.failure = failure; return false;
}
base::ScopedFD ReceiveFolder(int root, const std::string& folder, bool create) {
  const auto parts = FolderParts(folder);
  if (!parts) return {};
  base::ScopedFD current(fcntl(root, F_DUPFD_CLOEXEC, 0));
  for (const auto& part : *parts) {
    if (!current.is_valid()) return {};
    if (create && mkdirat(current.get(), part.c_str(), 0700) != 0 && errno != EEXIST) return {};
    base::ScopedFD child(openat(current.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    current = std::move(child);
  }
  return current;
}
bool CollectFiles(const base::FilePath& path, base::File handle, const std::string& leaf, const std::string& parent,
                    std::vector<SourceFile>& files, int64_t& total, size_t& entries,
                    const std::function<bool()>& active, StorageError& error) {
    if (!active()) return false;
    if (++entries > 10000 || !FolderParts(parent)) return StorageFail(error, "Directory depth or entry limit exceeded", Failure::kUnsupported);
    if (!SafeTransferName(leaf) || leaf.find(':') != std::string::npos) return StorageFail(error, "Unsupported source filename", Failure::kSourceFile);
    struct stat st{};
    if (!handle.IsValid() || fstat(handle.GetPlatformFile(), &st)) return StorageFail(error, "Unable to inspect source entry", Failure::kSourceFile);
    if (S_ISDIR(st.st_mode)) {
      const auto folder = RelativeName(parent, leaf);
      if (!FolderParts(folder)) return StorageFail(error, "Directory path limit exceeded", Failure::kUnsupported);
      // Descendants are opened relative to the retained directory descriptor;
      // replacing a path with a symlink cannot redirect traversal.
      DIR* raw = fdopendir(handle.GetPlatformFile());
      if (!raw) return StorageFail(error, "Unable to enumerate source directory", Failure::kSourceFile);
      (void)handle.TakePlatformFile();  // fdopendir owns the descriptor on success.
      std::unique_ptr<DIR, decltype(&closedir)> directory(raw, &closedir);
      for (;;) {
        errno = 0;
        auto* entry = readdir(directory.get());
        if (!entry) {
          if (errno) return StorageFail(error, "Unable to enumerate source directory", Failure::kSourceFile);
          break;
        }
        const std::string child_name(entry->d_name);
        if (child_name == "." || child_name == "..") continue;
        struct stat child_stat{};
        if (fstatat(dirfd(directory.get()), child_name.c_str(), &child_stat, AT_SYMLINK_NOFOLLOW) != 0 ||
            (!S_ISREG(child_stat.st_mode) && !S_ISDIR(child_stat.st_mode)))
          return StorageFail(error, "Symbolic links and special files cannot be shared", Failure::kSourceFile);
        base::File child(openat(dirfd(directory.get()), child_name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (!CollectFiles(path.Append(child_name), std::move(child), child_name, folder, files, total, entries, active, error)) return false;
      }
      // The protocol has no directory payload; empty directories contribute no files.
      return true;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > kMaxTotal - total)
      return StorageFail(error, "Unreadable, unsupported or oversized source file", Failure::kSourceFile);
    if (files.size() >= kMaxFiles) return StorageFail(error, "Directory selection exceeds 1024 files", Failure::kUnsupported);
    SourceFile f; f.path = path; f.size = st.st_size; f.name = leaf; f.parent = parent;
    f.handle = std::move(handle); total += f.size;
    files.push_back(std::move(f));
    return true;
  }

class LinuxReceiveBatch final : public ReceiveBatch {
 public:
  LinuxReceiveBatch();
  ~LinuxReceiveBatch() override;
  bool Initialize(const base::FilePath& destination, int64_t total,
                  const std::vector<ReceivePath>& paths, StorageError& error);
  bool Write(const std::string& path, base::span<const uint8_t> bytes, StorageError& error) override;
  bool FinishFile(const std::string& path, StorageError& error) override;
  bool Commit(StorageError& error) override;
  base::FilePath saved_directory() const override;
  bool directory_durable() const override;
 private:
  base::FilePath destination_, saved_;
  // Destruction order: file handles, directory descriptor, staging rollback.
  base::ScopedTempDir staging_;
  base::ScopedFD root_;
  std::map<std::string, base::File> files_;
  std::set<std::string> folders_;
  bool durable_ = false;
};
LinuxReceiveBatch::LinuxReceiveBatch() = default;
LinuxReceiveBatch::~LinuxReceiveBatch() = default;
bool LinuxReceiveBatch::Initialize(const base::FilePath& destination, int64_t total,
                                  const std::vector<ReceivePath>& paths, StorageError& error) {
  destination_ = destination;
  if (total < 0 || total > kMaxTotal || paths.empty() || paths.size() > kMaxFiles)
    return StorageFail(error, "Invalid receive batch", Failure::kUnsupported);
  if (!base::CreateDirectory(destination_)) return StorageFail(error, "Unable to create receive directory");
  const auto free = base::SysInfo::AmountOfFreeDiskSpace(destination_);
  if (!free || *free < total + 1024 * 1024) {
    error.no_space = true; return StorageFail(error, "Not enough disk space");
  }
  if (!staging_.CreateUniqueTempDirUnderPath(destination_, ".incoming-")) return StorageFail(error, "Unable to create staging directory");
  root_.reset(open(staging_.GetPath().value().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!root_.is_valid()) return StorageFail(error, "Unable to open receive staging directory");
  for (const auto& path : paths) {
    const auto parts = FolderParts(path.parent);
    if (!parts || !SafeTransferName(path.name) || path.name.find(':') != std::string::npos)
      return StorageFail(error, "Unsafe receive path", Failure::kProtocol);
    std::string folder;
    for (const auto& part : *parts) { folder = RelativeName(folder, part); folders_.insert(folder); }
    auto directory = ReceiveFolder(root_.get(), path.parent, true);
    if (!directory.is_valid()) return StorageFail(error, "Unable to create receive folder");
    base::File file(openat(directory.get(), path.name.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!file.IsValid()) return StorageFail(error, "Unable to create received file");
    files_.emplace(RelativeName(path.parent, path.name), std::move(file));
  }
  return true;
}
bool LinuxReceiveBatch::Write(const std::string& path, base::span<const uint8_t> bytes, StorageError& error) {
  auto it = files_.find(path);
  if (it == files_.end() || !it->second.WriteAtCurrentPosAndCheck(bytes))
    return StorageFail(error, "Unable to write file");
  return true;
}
bool LinuxReceiveBatch::FinishFile(const std::string& path, StorageError& error) {
  auto it = files_.find(path);
  if (it == files_.end() || !it->second.Flush()) return StorageFail(error, "Unable to flush received file");
  it->second.Close(); return true;
}
bool LinuxReceiveBatch::Commit(StorageError& error) {
  if (!saved_.empty()) return StorageFail(error, "Receive batch already committed");
  for (const auto& [path, file] : files_)
    if (file.IsValid()) return StorageFail(error, "Receive file has not finished");
  for (auto it = folders_.rbegin(); it != folders_.rend(); ++it) {
    auto folder = ReceiveFolder(root_.get(), *it, false);
    if (!folder.is_valid() || fsync(folder.get()) != 0) return StorageFail(error, "Unable to flush received directory");
  }
  if (fsync(root_.get()) != 0) return StorageFail(error, "Unable to flush receive staging directory");
  auto final = destination_.Append("Transfer-" + base::HexEncode(base::RandBytesAsString(8)));
  if (syscall(SYS_renameat2, AT_FDCWD, staging_.GetPath().value().c_str(), AT_FDCWD, final.value().c_str(), RENAME_NOREPLACE) != 0)
    return StorageFail(error, "Unable to commit received files");
  staging_.Take(); saved_ = std::move(final);
  base::ScopedFD parent(open(destination_.value().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  durable_ = parent.is_valid() && fsync(parent.get()) == 0;
  return true;
}
base::FilePath LinuxReceiveBatch::saved_directory() const { return saved_; }
bool LinuxReceiveBatch::directory_durable() const { return durable_; }
class LinuxFileStorage final : public FileStorage {
 public:
  ~LinuxFileStorage() override;
  bool Collect(const std::vector<base::FilePath>& paths, const std::function<bool()>& active,
               SourceSelection& selection, StorageError& error) override;
  std::unique_ptr<ReceiveBatch> CreateBatch(const base::FilePath& destination, int64_t total,
      const std::vector<ReceivePath>& paths, StorageError& error) override;
};
LinuxFileStorage::~LinuxFileStorage() = default;
bool LinuxFileStorage::Collect(const std::vector<base::FilePath>& paths, const std::function<bool()>& active,
                              SourceSelection& selection, StorageError& error) {
  selection = {}; error = {};
  if (paths.empty() || paths.size() > kMaxFiles)
    return StorageFail(error, "Select between 1 and 1024 files or directories", Failure::kUnsupported);
  SourceSelection collected;
  std::set<std::string> roots;
  for (const auto& path : paths) {
    if (!active()) return false;
    std::string leaf = path.BaseName().value();
    for (unsigned suffix = 2; !roots.insert(leaf).second; ++suffix) {
      const auto tail = " (" + base::NumberToString(suffix) + ")";
      base::TruncateUTF8ToByteSize(path.BaseName().value(), 240 - tail.size(), &leaf); leaf += tail;
    }
    base::File handle(open(path.StripTrailingSeparators().value().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!CollectFiles(path, std::move(handle), leaf, "", collected.files, collected.total, collected.entries, active, error)) return false;
  }
  if (collected.files.empty()) return StorageFail(error, "Selected directories contain no transferable files", Failure::kUnsupported);
  selection = std::move(collected); return true;
}
std::unique_ptr<ReceiveBatch> LinuxFileStorage::CreateBatch(const base::FilePath& destination, int64_t total,
    const std::vector<ReceivePath>& paths, StorageError& error) {
  error = {};
  auto batch = std::make_unique<LinuxReceiveBatch>();
  if (!batch->Initialize(destination, total, paths, error)) return nullptr;
  return batch;
}
}  // namespace
std::unique_ptr<FileStorage> CreateFileStorage() { return std::make_unique<LinuxFileStorage>(); }
}  // namespace crquickshare
