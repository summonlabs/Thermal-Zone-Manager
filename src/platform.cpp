// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "platform.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace thermal_zone_manager {
namespace detail {
namespace {

std::atomic<std::uint64_t> g_staging_counter{0};

bool is_reserved_device_name(const std::string& component) {
  std::string stem = component;
  const std::size_t dot = stem.find('.');
  if (dot != std::string::npos) {
    stem = stem.substr(0, dot);
  }
  std::string lowered;
  lowered.reserve(stem.size());
  for (const char item : stem) {
    lowered.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(item))));
  }
  if (lowered == "con" || lowered == "prn" || lowered == "aux" || lowered == "nul") {
    return true;
  }
  if (lowered.size() == 4 && (lowered.compare(0, 3, "com") == 0 || lowered.compare(0, 3, "lpt") == 0)) {
    const char digit = lowered[3];
    return digit >= '1' && digit <= '9';
  }
  return false;
}

// Rejects every component that makes a path ambiguous: empty, traversal,
// reserved device names, trailing dot or space, control characters, and
// over-long components.
Status validate_components(const std::string& path) {
  if (path.find('\0') != std::string::npos) {
    return Error(ErrorCode::StorePathInvalid, "a store path may not contain a NUL byte");
  }
  std::size_t index = 0;
  const std::size_t size = path.size();
  while (index < size) {
    while (index < size && (path[index] == '/' || path[index] == '\\')) {
      ++index;
    }
    const std::size_t start = index;
    while (index < size && path[index] != '/' && path[index] != '\\') {
      ++index;
    }
    if (index == start) {
      break;
    }
    const std::string component = path.substr(start, index - start);
    // A drive designator such as "C:" is a component on Windows.
    const bool drive = component.size() == 2 && component[1] == ':';
    if (!drive) {
      // ".." is traversal and is never accepted. A single "." is harmless:
      // it is removed by normalisation below and cannot escape the root.
      if (component == "..") {
        return Error(ErrorCode::StorePathInvalid,
                     "a store path may not contain a traversal component")
            .with("component", component);
      }
      if (component.size() > 255) {
        return Error(ErrorCode::StorePathInvalid, "a store path component is too long")
            .with("length", static_cast<std::uint64_t>(component.size()));
      }
      const char last = component.back();
      if (component != "." && (last == '.' || last == ' ')) {
        return Error(ErrorCode::StorePathInvalid,
                     "a store path component may not end with a dot or a space")
            .with("component", component);
      }
      for (const char item : component) {
        const auto raw = static_cast<unsigned char>(item);
        if (raw < 0x20 || item == ':' || item == '*' || item == '?' || item == '"' ||
            item == '<' || item == '>' || item == '|') {
          return Error(ErrorCode::StorePathInvalid,
                       "a store path component holds a character that is never valid")
              .with("component", component);
        }
      }
      if (is_reserved_device_name(component)) {
        return Error(ErrorCode::StorePathInvalid, "a store path component is a reserved device name")
            .with("component", component);
      }
    }
  }
  return Status();
}

#if defined(_WIN32)

std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
  if (needed <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), needed);
  return wide;
}

std::string narrow(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return std::string();
  }
  std::string narrow_text(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), narrow_text.data(),
                      needed, nullptr, nullptr);
  return narrow_text;
}

std::string system_error_text(unsigned long code) {
  LPWSTR buffer = nullptr;
  const DWORD written = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::string text = "system error " + std::to_string(code);
  if (written != 0 && buffer != nullptr) {
    std::wstring message(buffer, written);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                                message.back() == L' ')) {
      message.pop_back();
    }
    text = narrow(message);
  }
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  return text;
}

#endif

}  // namespace

Result<std::string> final_path_of(NativeFile file) {
#if defined(_WIN32)
  if (file == nullptr) {
    return Error(ErrorCode::IoFailure, "the file handle is not open");
  }
  std::wstring buffer(4096, L'\0');
  DWORD written = 0;
  for (;;) {
    written = GetFinalPathNameByHandleW(static_cast<HANDLE>(file), buffer.data(),
                                        static_cast<DWORD>(buffer.size()),
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0) {
      return Error(ErrorCode::IoFailure, "the resolved path of the store could not be read")
          .with("system_error", system_error_text(GetLastError()));
    }
    if (written < buffer.size()) {
      break;
    }
    buffer.resize(static_cast<std::size_t>(written) + 1U);
  }
  buffer.resize(written);
  std::wstring text = buffer;
  if (text.rfind(L"\\\\?\\", 0) == 0) {
    text = text.substr(4);
  }
  return narrow(text);
#else
  if (file == nullptr) {
    return Error(ErrorCode::IoFailure, "the file handle is not open");
  }
  std::vector<char> buffer(4096, '\0');
  char link[64] = {0};
  std::snprintf(link, sizeof(link), "/proc/self/fd/%d", static_cast<int>(reinterpret_cast<std::intptr_t>(file)));
  const ssize_t written = ::readlink(link, buffer.data(), buffer.size() - 1);
  if (written <= 0) {
    return Error(ErrorCode::IoFailure, "the resolved path of the store could not be read");
  }
  buffer[static_cast<std::size_t>(written)] = '\0';
  return std::string(buffer.data());
#endif
}

std::string join_path(const std::string& base, const std::string& leaf) {
  if (base.empty()) {
    return leaf;
  }
  const char last = base.back();
  if (last == '/' || last == '\\') {
    return base + leaf;
  }
#if defined(_WIN32)
  return base + "\\" + leaf;
#else
  return base + "/" + leaf;
#endif
}

bool path_exists(const std::string& path) {
  std::error_code code;
  return std::filesystem::exists(std::filesystem::path(path), code);
}

Result<std::string> canonical_directory(const std::string& path, bool create_if_missing) {
  if (path.empty()) {
    return Error(ErrorCode::StorePathInvalid, "a store root may not be empty");
  }
  if (path.size() > 4096) {
    return Error(ErrorCode::StorePathInvalid, "a store root is longer than any path may be")
        .with("length", static_cast<std::uint64_t>(path.size()));
  }
  const Status input_components = validate_components(path);
  if (!input_components.ok()) {
    return input_components.error();
  }

  std::error_code code;
  const std::filesystem::path raw(path);
  std::filesystem::path absolute = std::filesystem::absolute(raw, code);
  if (code) {
    return Error(ErrorCode::StorePathInvalid, "a store root could not be made absolute")
        .with("path", path);
  }
  absolute = absolute.lexically_normal();
  const std::string normalized = absolute.string();
  const Status normalized_components = validate_components(normalized);
  if (!normalized_components.ok()) {
    return normalized_components.error();
  }

  if (!std::filesystem::exists(absolute, code)) {
    if (!create_if_missing) {
      return Error(ErrorCode::StoreUnavailable, "the store directory does not exist")
          .with("path", normalized);
    }
    std::error_code create_code;
    if (!std::filesystem::create_directories(absolute, create_code) && create_code) {
      return Error(ErrorCode::IoFailure, "the store directory could not be created")
          .with("path", normalized)
          .with("system_error", create_code.message());
    }
  } else if (!std::filesystem::is_directory(absolute, code)) {
    return Error(ErrorCode::StoreUnavailable, "the store root exists but is not a directory")
        .with("path", normalized);
  }

  // Opening the directory and asking the kernel for its final path resolves
  // every reparse point, junction and symbolic link. Two processes naming the
  // same directory through different links therefore agree on one identity.
  Result<NativeFile> handle = open_file(normalized, false, false);
  if (!handle.ok()) {
    return handle.error();
  }
  Result<std::string> resolved = final_path_of(handle.value());
  close_file(handle.value());
  if (!resolved.ok()) {
    return resolved.error();
  }
  const Status resolved_components = validate_components(resolved.value());
  if (!resolved_components.ok()) {
    return resolved_components.error();
  }
  if (resolved.value().size() > 240) {
    return Error(ErrorCode::StorePathInvalid, "the resolved store root is too long to extend")
        .with("length", static_cast<std::uint64_t>(resolved.value().size()));
  }
  return resolved.value();
}

Result<std::string> staging_directory(const std::string& canonical_root) {
  if (canonical_root.empty()) {
    return Error(ErrorCode::StorePathInvalid, "a canonical store root is required");
  }
  return canonical_root;
}

Result<NativeFile> open_file(const std::string& path, bool read_write, bool create_if_missing) {
#if defined(_WIN32)
  const std::wstring wide = widen(path);
  if (wide.empty()) {
    return Error(ErrorCode::StorePathInvalid, "a file path could not be represented")
        .with("path", path);
  }
  const DWORD access = read_write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
  const DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
  const DWORD disposition = create_if_missing ? OPEN_ALWAYS : OPEN_EXISTING;
  const HANDLE handle = CreateFileW(wide.c_str(), access, sharing, nullptr, disposition,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return Error(ErrorCode::StoreUnavailable, "the file does not exist").with("path", path);
    }
    if (code == ERROR_ACCESS_DENIED) {
      return Error(ErrorCode::IoFailure, "the file could not be opened for this access")
          .with("path", path)
          .with("system_error", system_error_text(code));
    }
    return Error(ErrorCode::IoFailure, "the file could not be opened")
        .with("path", path)
        .with("system_error", system_error_text(code));
  }
  return static_cast<NativeFile>(handle);
#else
  const int flags = read_write ? O_RDWR : O_RDONLY;
  const int descriptor = ::open(path.c_str(), flags | (create_if_missing ? O_CREAT : 0), 0666);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      return Error(ErrorCode::StoreUnavailable, "the file does not exist").with("path", path);
    }
    return Error(ErrorCode::IoFailure, "the file could not be opened")
        .with("path", path)
        .with("system_error", std::strerror(errno));
  }
  return reinterpret_cast<NativeFile>(static_cast<std::intptr_t>(descriptor));
#endif
}

Result<NativeFile> open_lock_file(const std::string& path) {
  return open_file(path, true, true);
}

Status lock_exclusive(NativeFile file, Nanoseconds wait) {
  if (file == nullptr) {
    return Error(ErrorCode::IoFailure, "the lock file is not open");
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::nanoseconds(wait.value > 0 ? wait.value : 0);
  bool first = true;
  for (;;) {
#if defined(_WIN32)
    OVERLAPPED overlapped{};
    const DWORD flags = LOCKFILE_EXCLUSIVE_LOCK |
                        (wait.value > 0 ? 0U : LOCKFILE_FAIL_IMMEDIATELY);
    if (LockFileEx(static_cast<HANDLE>(file), flags, 0, 1, 0, &overlapped) != 0) {
      return Status();
    }
    const DWORD code = GetLastError();
    if (code != ERROR_LOCK_VIOLATION && code != ERROR_IO_PENDING) {
      return Error(ErrorCode::IoFailure, "the writer lock could not be taken")
          .with("path", "(lock file)")
          .with("system_error", system_error_text(code));
    }
#else
    struct flock operation {};
    operation.l_type = static_cast<short>(F_WRLCK);
    operation.l_whence = static_cast<short>(SEEK_SET);
    operation.l_start = 0;
    operation.l_len = 0;
    if (::fcntl(static_cast<int>(reinterpret_cast<std::intptr_t>(file)), F_SETLK, &operation) == 0) {
      return Status();
    }
    if (errno != EACCES && errno != EAGAIN) {
      return Error(ErrorCode::IoFailure, "the writer lock could not be taken")
          .with("system_error", std::strerror(errno));
    }
#endif
    if (wait.value <= 0 || std::chrono::steady_clock::now() >= deadline) {
      return Error(ErrorCode::StoreLocked,
                   first ? "another process holds the writer lock for this store"
                         : "the writer lock did not become available inside the wait budget");
    }
    first = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

Status unlock(NativeFile file) {
  if (file == nullptr) {
    return Status();
  }
#if defined(_WIN32)
  OVERLAPPED overlapped{};
  if (UnlockFileEx(static_cast<HANDLE>(file), 0, 1, 0, &overlapped) == 0) {
    return Error(ErrorCode::IoFailure, "the writer lock could not be released")
        .with("system_error", system_error_text(GetLastError()));
  }
  return Status();
#else
  struct flock operation {};
  operation.l_type = static_cast<short>(F_UNLCK);
  operation.l_whence = static_cast<short>(SEEK_SET);
  operation.l_start = 0;
  operation.l_len = 0;
  if (::fcntl(static_cast<int>(reinterpret_cast<std::intptr_t>(file)), F_SETLK, &operation) != 0) {
    return Error(ErrorCode::IoFailure, "the writer lock could not be released")
        .with("system_error", std::strerror(errno));
  }
  return Status();
#endif
}

void close_file(NativeFile file) noexcept {
  if (file == nullptr) {
    return;
  }
#if defined(_WIN32)
  CloseHandle(static_cast<HANDLE>(file));
#else
  ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(file)));
#endif
}

Result<std::vector<std::byte>> read_at(NativeFile file, std::uint64_t offset, std::size_t length) {
  if (file == nullptr) {
    return Error(ErrorCode::IoFailure, "the file is not open");
  }
  std::vector<std::byte> buffer(length);
  if (length == 0) {
    return buffer;
  }
#if defined(_WIN32)
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
  overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFFULL);
  DWORD read = 0;
  if (ReadFile(static_cast<HANDLE>(file), buffer.data(), static_cast<DWORD>(length), &read,
               &overlapped) == 0) {
    const DWORD code = GetLastError();
    if (code == ERROR_HANDLE_EOF) {
      return Error(ErrorCode::TruncatedRecord, "the file ended before the expected record")
          .with("offset", offset);
    }
    return Error(ErrorCode::IoFailure, "the file could not be read")
        .with("offset", offset)
        .with("system_error", system_error_text(code));
  }
  if (read != length) {
    return Error(ErrorCode::TruncatedRecord, "the file ended before the expected record")
        .with("offset", offset)
        .with("expected", static_cast<std::uint64_t>(length))
        .with("actual", static_cast<std::uint64_t>(read));
  }
#else
  std::size_t done = 0;
  while (done < length) {
    const ssize_t read = ::pread(static_cast<int>(reinterpret_cast<std::intptr_t>(file)),
                                 buffer.data() + done, length - done,
                                 static_cast<off_t>(offset + done));
    if (read <= 0) {
      if (read == 0) {
        return Error(ErrorCode::TruncatedRecord, "the file ended before the expected record")
            .with("offset", offset);
      }
      return Error(ErrorCode::IoFailure, "the file could not be read")
          .with("system_error", std::strerror(errno));
    }
    done += static_cast<std::size_t>(read);
  }
#endif
  return buffer;
}

Status write_at(NativeFile file, std::uint64_t offset, std::span<const std::byte> data) {
  if (file == nullptr) {
    return Error(ErrorCode::IoFailure, "the file is not open");
  }
  if (data.empty()) {
    return Status();
  }
#if defined(_WIN32)
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
  overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFFULL);
  DWORD written = 0;
  if (WriteFile(static_cast<HANDLE>(file), data.data(), static_cast<DWORD>(data.size()), &written,
                &overlapped) == 0) {
    return Error(ErrorCode::IoFailure, "the file could not be written")
        .with("offset", offset)
        .with("system_error", system_error_text(GetLastError()));
  }
  if (written != data.size()) {
    return Error(ErrorCode::IoFailure, "only part of the record was written")
        .with("offset", offset)
        .with("expected", static_cast<std::uint64_t>(data.size()))
        .with("actual", static_cast<std::uint64_t>(written));
  }
#else
  std::size_t done = 0;
  while (done < data.size()) {
    const ssize_t written = ::pwrite(static_cast<int>(reinterpret_cast<std::intptr_t>(file)),
                                     data.data() + done, data.size() - done,
                                     static_cast<off_t>(offset + done));
    if (written <= 0) {
      return Error(ErrorCode::IoFailure, "the file could not be written")
          .with("system_error", std::strerror(errno));
    }
    done += static_cast<std::size_t>(written);
  }
#endif
  return Status();
}

Status flush_file(NativeFile file) {
  if (file == nullptr) {
    return Error(ErrorCode::IoFailure, "the file is not open");
  }
#if defined(_WIN32)
  if (FlushFileBuffers(static_cast<HANDLE>(file)) == 0) {
    return Error(ErrorCode::IoFailure, "the file could not be flushed to the device")
        .with("system_error", system_error_text(GetLastError()));
  }
  return Status();
#else
  if (::fsync(static_cast<int>(reinterpret_cast<std::intptr_t>(file))) != 0) {
    return Error(ErrorCode::IoFailure, "the file could not be flushed to the device")
        .with("system_error", std::strerror(errno));
  }
  return Status();
#endif
}

Result<std::uint64_t> file_size(NativeFile file) {
  if (file == nullptr) {
    return Error(ErrorCode::IoFailure, "the file is not open");
  }
#if defined(_WIN32)
  LARGE_INTEGER size{};
  if (GetFileSizeEx(static_cast<HANDLE>(file), &size) == 0) {
    return Error(ErrorCode::IoFailure, "the file size could not be read")
        .with("system_error", system_error_text(GetLastError()));
  }
  return static_cast<std::uint64_t>(size.QuadPart);
#else
  struct ::stat information {};
  if (::fstat(static_cast<int>(reinterpret_cast<std::intptr_t>(file)), &information) != 0) {
    return Error(ErrorCode::IoFailure, "the file size could not be read")
        .with("system_error", std::strerror(errno));
  }
  return static_cast<std::uint64_t>(information.st_size);
#endif
}

Status set_file_size(NativeFile file, std::uint64_t size) {
  if (file == nullptr) {
    return Error(ErrorCode::IoFailure, "the file is not open");
  }
#if defined(_WIN32)
  LARGE_INTEGER target{};
  target.QuadPart = static_cast<LONGLONG>(size);
  if (SetFilePointerEx(static_cast<HANDLE>(file), target, nullptr, FILE_BEGIN) == 0) {
    return Error(ErrorCode::IoFailure, "the file position could not be set")
        .with("system_error", system_error_text(GetLastError()));
  }
  if (SetEndOfFile(static_cast<HANDLE>(file)) == 0) {
    return Error(ErrorCode::IoFailure, "the file could not be sized")
        .with("system_error", system_error_text(GetLastError()));
  }
  return Status();
#else
  if (::ftruncate(static_cast<int>(reinterpret_cast<std::intptr_t>(file)),
                  static_cast<off_t>(size)) != 0) {
    return Error(ErrorCode::IoFailure, "the file could not be sized")
        .with("system_error", std::strerror(errno));
  }
  return Status();
#endif
}

Status create_file(const std::string& path, std::uint64_t size) {
  Result<NativeFile> file = open_file(path, true, true);
  if (!file.ok()) {
    return file.error();
  }
  Status status = set_file_size(file.value(), size);
  if (status.ok()) {
    status = flush_file(file.value());
  }
  close_file(file.value());
  return status;
}

Status delete_file(const std::string& path) noexcept {
  std::error_code code;
  std::filesystem::remove(std::filesystem::path(path), code);
  return Status();
}

Status rename_file(const std::string& from, const std::string& to) {
  std::error_code code;
  std::filesystem::rename(std::filesystem::path(from), std::filesystem::path(to), code);
  if (code) {
    return Error(ErrorCode::IoFailure, "a staged file could not be published by rename")
        .with("from", from)
        .with("to", to)
        .with("system_error", code.message());
  }
  return Status();
}

std::string staging_name(const std::string& directory, const std::string& stem,
                         std::uint64_t token) {
  const std::uint64_t counter = g_staging_counter.fetch_add(1, std::memory_order_relaxed);
  const std::string leaf = stem + ".stage-" + std::to_string(token) + "-" +
                           std::to_string(counter);
  return join_path(directory, leaf);
}

}  // namespace detail
}  // namespace thermal_zone_manager
