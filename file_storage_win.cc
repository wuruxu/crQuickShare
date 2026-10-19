// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/file_storage.h"
#include <windows.h>
#include <winternl.h>
#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include "base/compiler_specific.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/system/sys_info.h"
#include "base/win/scoped_handle.h"
namespace crquickshare {
namespace {
bool FailStorage(StorageError& error, const char* message, Failure failure = Failure::kStorage) {
  error.message = message; error.failure = failure; return false;
}
// FILE_RENAME_INFORMATION is not exposed by the user-mode Windows SDK.
struct NativeRenameInformation {
  BOOLEAN replace_if_exists;
  HANDLE root_directory;
  ULONG file_name_length;
  WCHAR file_name[1];
};
bool RenameInParent(HANDLE directory, const base::FilePath& leaf, StorageError& error) {
  // The user-mode SDK exposes only a subset of FILE_INFORMATION_CLASS. Use
  // its 32-bit ABI representation so FileRenameInformation is not converted
  // to an out-of-range C++ enum value.
  static_assert(sizeof(FILE_INFORMATION_CLASS) == sizeof(ULONG));
  using SetInformationFunction = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
  using StatusToErrorFunction = ULONG(NTAPI*)(NTSTATUS);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) {
    const DWORD system_error = GetLastError();
    LOG(ERROR) << "crQuickShare: Receive commit failed; operation=load_ntdll; system_error=" << system_error;
    return FailStorage(error, "Unable to commit received files");
  }
  const auto set_information = reinterpret_cast<SetInformationFunction>(GetProcAddress(ntdll, "NtSetInformationFile"));
  if (!set_information) {
    const DWORD system_error = GetLastError();
    LOG(ERROR) << "crQuickShare: Receive commit failed; operation=resolve_rename; system_error=" << system_error;
    return FailStorage(error, "Unable to commit received files");
  }
  const auto status_to_error = reinterpret_cast<StatusToErrorFunction>(GetProcAddress(ntdll, "RtlNtStatusToDosError"));
  // A leaf name and NULL RootDirectory instruct the native API to rename in
  // the existing parent. Unlike a full-path Win32 rename, this does not reopen
  // the target parent for writing and conflict with our retained read-only
  // sharing handles. Keep all ancestor locks and never overwrite a target.
  const auto name_bytes = base::as_byte_span(leaf.value());
  std::vector<uint8_t> data(sizeof(NativeRenameInformation) + name_bytes.size());
  // SAFETY: data owns the complete structure and filename, is zero initialized,
  // and is not resized before the synchronous native call consumes it.
  auto* info = UNSAFE_BUFFERS(reinterpret_cast<NativeRenameInformation*>(data.data()));
  info->replace_if_exists = FALSE; info->root_directory = nullptr;
  info->file_name_length = static_cast<ULONG>(name_bytes.size());
  base::span(data).subspan(offsetof(NativeRenameInformation, file_name), name_bytes.size()).copy_from(name_bytes);
  IO_STATUS_BLOCK io_status{};
  constexpr ULONG kFileRenameInformation = 10;
  const NTSTATUS status = set_information(directory, &io_status, info, static_cast<ULONG>(data.size()), kFileRenameInformation);
  if (status < 0) {
    const ULONG system_error = status_to_error ? status_to_error(status) : ERROR_MR_MID_NOT_FOUND;
    LOG(ERROR) << "crQuickShare: Receive commit failed; operation=rename_in_parent; system_error=" << system_error
               << "; ntstatus=" << static_cast<ULONG>(status) << "; error_mapping_available=" << !!status_to_error;
    error.no_space = system_error == ERROR_DISK_FULL || system_error == ERROR_HANDLE_DISK_FULL;
    return FailStorage(error, "Unable to commit received files");
  }
  return true;
}
bool SafeWindowsName(const std::string& name) {
  if (!SafeTransferName(name) || name.back() == '.' || name.back() == ' ' || name.find_first_of(":*?\"<>|") != std::string::npos) return false;
  const auto stem = base::ToLowerASCII(name.substr(0, name.find('.')));
  if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || stem == "clock$" || stem == "conin$" || stem == "conout$") return false;
  if (stem.starts_with("com") || stem.starts_with("lpt")) {
    const auto suffix = stem.substr(3);
    if ((suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '9') || suffix == "¹" || suffix == "²" || suffix == "³") return false;
  }
  return true;
}
base::win::ScopedHandle OpenDirectory(const base::FilePath& path, bool for_rename = false) {
  // Deny delete/write sharing: retained ancestors cannot be replaced, renamed,
  // or converted to reparse points while path-based child operations run.
  base::win::ScopedHandle handle(CreateFileW(path.value().c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | (for_rename ? DELETE : 0),
      FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  BY_HANDLE_FILE_INFORMATION info{};
  if (!handle.is_valid() || !GetFileInformationByHandle(handle.get(), &info) ||
      !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
  return handle;
}
bool LockAncestors(const base::FilePath& path, std::vector<base::win::ScopedHandle>& handles) {
  // Local drive paths only; reject Win32 device namespaces and remote shares.
  if (!path.IsAbsolute() || path.IsNetwork() || path.ReferencesParent()) return false;
  std::vector<base::FilePath> chain;
  auto current = path.StripTrailingSeparators();
  for (size_t depth = 0; depth < 256; ++depth) {
    chain.push_back(current);
    const auto parent = current.DirName();
    if (parent == current) break;
    current = parent;
  }
  if (chain.back().DirName() != chain.back()) return false;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    auto handle = OpenDirectory(*it);
    if (!handle.is_valid()) return false;
    handles.push_back(std::move(handle));
  }
  return true;
}
bool CollectEntry(const base::FilePath& path, const std::string& leaf, const std::string& parent,
                  const std::function<bool()>& active, SourceSelection& result, StorageError& error) {
  if (!active()) return false;
  if (++result.entries > 10000 || !FolderParts(parent)) return FailStorage(error, "Directory limit exceeded", Failure::kUnsupported);
  if (!SafeWindowsName(leaf)) return FailStorage(error, "Unsupported source filename", Failure::kSourceFile);
  base::win::ScopedHandle handle(CreateFileW(path.value().c_str(), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  BY_HANDLE_FILE_INFORMATION info{};
  if (!handle.is_valid() || GetFileType(handle.get()) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle.get(), &info) ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return FailStorage(error, "Unreadable source or reparse point", Failure::kSourceFile);
  if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    const auto folder = RelativeName(parent, leaf);
    if (!FolderParts(folder)) return FailStorage(error, "Directory path limit exceeded", Failure::kUnsupported);
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW(path.Append(FILE_PATH_LITERAL("*")).value().c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
      if (GetLastError() == ERROR_FILE_NOT_FOUND) return true;
      return FailStorage(error, "Unable to enumerate source directory", Failure::kSourceFile);
    }
    bool ok = true;
    for (;;) {
      const std::wstring child(entry.cFileName);
      if (child != L"." && child != L"..") {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !CollectEntry(path.Append(child), base::WideToUTF8(child), folder, active, result, error)) {
          if (error.message.empty() && active()) FailStorage(error, "Reparse points cannot be shared", Failure::kSourceFile);
          ok = false; break;
        }
      }
      if (!FindNextFileW(search, &entry)) {
        if (GetLastError() != ERROR_NO_MORE_FILES) ok = FailStorage(error, "Unable to enumerate source directory", Failure::kSourceFile);
        break;
      }
    }
    FindClose(search); return ok;
  }
  const uint64_t size = (uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow;
  if (size > static_cast<uint64_t>(kMaxTransferBytes - result.total) || result.files.size() >= kMaxTransferFiles)
    return FailStorage(error, "Source size or file count limit exceeded", Failure::kUnsupported);
  SourceFile file; file.path = path; file.name = leaf; file.parent = parent; file.size = static_cast<int64_t>(size);
  file.handle = base::File(handle.release()); result.total += file.size; result.files.push_back(std::move(file));
  return true;
}
class WinReceiveBatch final : public ReceiveBatch {
 public:
  WinReceiveBatch();
  ~WinReceiveBatch() override;
  bool Initialize(const base::FilePath& destination, int64_t total, const std::vector<ReceivePath>& paths, StorageError& error);
  bool Write(const std::string& path, base::span<const uint8_t> bytes, StorageError& error) override;
  bool FinishFile(const std::string& path, StorageError& error) override;
  bool Commit(StorageError& error) override;
  base::FilePath saved_directory() const override;
  bool directory_durable() const override;
 private:
  base::FilePath destination_, saved_;
  base::ScopedTempDir staging_;
  std::vector<base::win::ScopedHandle> ancestors_;
  base::win::ScopedHandle root_;
  std::map<std::string, base::win::ScopedHandle> directories_;
  std::map<std::string, base::File> files_;
};
WinReceiveBatch::WinReceiveBatch() = default;
WinReceiveBatch::~WinReceiveBatch() = default;
bool WinReceiveBatch::Initialize(const base::FilePath& destination, int64_t total,
                                const std::vector<ReceivePath>& paths, StorageError& error) {
  destination_ = destination;
  if (total < 0 || total > kMaxTransferBytes || paths.empty() || paths.size() > kMaxTransferFiles)
    return FailStorage(error, "Invalid receive batch", Failure::kUnsupported);
  if (!base::CreateDirectory(destination_) || !LockAncestors(destination_, ancestors_))
    return FailStorage(error, "Receive directory is unavailable or contains reparse points");
  const auto free = base::SysInfo::AmountOfFreeDiskSpace(destination_);
  if (!free || *free < total + 1024 * 1024) { error.no_space = true; return FailStorage(error, "Not enough disk space"); }
  if (!staging_.CreateUniqueTempDirUnderPath(destination_, FILE_PATH_LITERAL(".incoming-"))) return FailStorage(error, "Unable to create staging directory");
  root_ = OpenDirectory(staging_.GetPath(), true);
  if (!root_.is_valid()) return FailStorage(error, "Unable to open staging directory");
  std::set<std::tuple<DWORD, DWORD, DWORD>> identities;
  for (const auto& path : paths) {
    const auto parts = FolderParts(path.parent);
    if (!parts || !SafeWindowsName(path.name)) return FailStorage(error, "Unsupported Windows receive path", Failure::kUnsupported);
    auto directory = staging_.GetPath(); std::string relative;
    for (const auto& part : *parts) {
      if (!SafeWindowsName(part)) return FailStorage(error, "Unsupported Windows folder name", Failure::kUnsupported);
      directory = directory.Append(base::FilePath::FromUTF8Unsafe(part)); relative = RelativeName(relative, part);
      if (directories_.contains(relative)) continue;
      if (!CreateDirectoryW(directory.value().c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return FailStorage(error, "Unable to create receive folder");
      auto handle = OpenDirectory(directory);
      BY_HANDLE_FILE_INFORMATION info{};
      if (!handle.is_valid() || !GetFileInformationByHandle(handle.get(), &info)) return FailStorage(error, "Unable to open receive folder");
      if (!identities.emplace(info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow).second)
        return FailStorage(error, "Case-insensitive folder collision", Failure::kUnsupported);
      directories_.emplace(relative, std::move(handle));
    }
    auto file_path = directory.Append(base::FilePath::FromUTF8Unsafe(path.name));
    base::win::ScopedHandle handle(CreateFileW(file_path.value().c_str(), GENERIC_WRITE,
        0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.is_valid()) return FailStorage(error, "Unable to create received file or conflicting filename");
    files_.emplace(RelativeName(path.parent, path.name), base::File(handle.release()));
  }
  return true;
}
bool WinReceiveBatch::Write(const std::string& path, base::span<const uint8_t> bytes, StorageError& error) {
  auto it = files_.find(path);
  if (it == files_.end() || !it->second.WriteAtCurrentPosAndCheck(bytes)) return FailStorage(error, "Unable to write file");
  return true;
}
bool WinReceiveBatch::FinishFile(const std::string& path, StorageError& error) {
  auto it = files_.find(path);
  if (it == files_.end() || !it->second.Flush()) return FailStorage(error, "Unable to flush received file");
  it->second.Close(); return true;
}
bool WinReceiveBatch::Commit(StorageError& error) {
  if (!saved_.empty()) return FailStorage(error, "Receive batch already committed");
  for (const auto& [path, file] : files_) if (file.IsValid()) return FailStorage(error, "Receive file has not finished");
  directories_.clear();
  const auto leaf = base::FilePath::FromASCII("Transfer-" + base::HexEncode(base::RandBytesAsString(8)));
  const auto final = destination_.Append(leaf);
  if (!RenameInParent(root_.get(), leaf, error)) return false;
  staging_.Take(); saved_ = final; return true;
}
base::FilePath WinReceiveBatch::saved_directory() const { return saved_; }
bool WinReceiveBatch::directory_durable() const { return false; }
class WinFileStorage final : public FileStorage {
 public:
  ~WinFileStorage() override;
  bool Collect(const std::vector<base::FilePath>& paths, const std::function<bool()>& active,
               SourceSelection& selection, StorageError& error) override;
  std::unique_ptr<ReceiveBatch> CreateBatch(const base::FilePath& destination, int64_t total,
      const std::vector<ReceivePath>& paths, StorageError& error) override;
};
WinFileStorage::~WinFileStorage() = default;
bool WinFileStorage::Collect(const std::vector<base::FilePath>& paths, const std::function<bool()>& active,
                            SourceSelection& selection, StorageError& error) {
  selection = {}; error = {};
  if (paths.empty() || paths.size() > kMaxTransferFiles) return FailStorage(error, "Too many selected items", Failure::kUnsupported);
  SourceSelection collected; std::set<std::string> roots;
  for (const auto& path : paths) {
    if (!active()) return false;
    std::vector<base::win::ScopedHandle> ancestors;
    if (!LockAncestors(path.DirName(), ancestors)) return FailStorage(error, "Source path is unavailable or contains reparse points", Failure::kSourceFile);
    std::string leaf = path.BaseName().AsUTF8Unsafe();
    for (unsigned suffix = 2; !roots.insert(base::ToLowerASCII(leaf)).second; ++suffix) {
      const auto tail = " (" + base::NumberToString(suffix) + ")";
      base::TruncateUTF8ToByteSize(path.BaseName().AsUTF8Unsafe(), 240 - tail.size(), &leaf); leaf += tail;
    }
    if (!CollectEntry(path, leaf, "", active, collected, error)) return false;
  }
  if (collected.files.empty()) return FailStorage(error, "Selected directories contain no transferable files", Failure::kUnsupported);
  selection = std::move(collected); return true;
}
std::unique_ptr<ReceiveBatch> WinFileStorage::CreateBatch(const base::FilePath& destination, int64_t total,
    const std::vector<ReceivePath>& paths, StorageError& error) {
  error = {}; auto batch = std::make_unique<WinReceiveBatch>();
  if (!batch->Initialize(destination, total, paths, error)) return nullptr;
  return batch;
}
}  // namespace
std::unique_ptr<FileStorage> CreateFileStorage() { return std::make_unique<WinFileStorage>(); }
}  // namespace crquickshare
