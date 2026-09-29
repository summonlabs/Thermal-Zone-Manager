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

#ifndef THERMAL_ZONE_MANAGER_SRC_PLATFORM_HPP
#define THERMAL_ZONE_MANAGER_SRC_PLATFORM_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "thermal_zone_manager/errors.hpp"
#include "thermal_zone_manager/units.hpp"

// Thin native file and path layer. Everything the store needs from the host is
// declared here so the durable store itself is portable logic over these
// primitives.

namespace thermal_zone_manager {
namespace detail {

// An open native file. Null means "not open".
using NativeFile = void*;

// Canonicalises a directory path: absolute, fully resolved through reparse
// points and symlinks, with every component checked for traversal, reserved
// device names and trailing dots or spaces. When create_if_missing is set the
// directory is created first.
Result<std::string> canonical_directory(const std::string& path, bool create_if_missing);

// The final resolved path of an already-open handle.
Result<std::string> final_path_of(NativeFile file);

// Joins two path components with the platform separator.
std::string join_path(const std::string& base, const std::string& leaf);

// True when the path exists (file or directory).
bool path_exists(const std::string& path);

// The directory used for staging files during store creation. This is the
// store's own directory, never a shared temporary directory, so staging never
// crosses a volume during publication.
Result<std::string> staging_directory(const std::string& canonical_root);

// Opens (creating when asked) a file for reading and, optionally, writing.
// Concurrent readers are always permitted; the store's own lock file provides
// exclusion.
Result<NativeFile> open_file(const std::string& path, bool read_write, bool create_if_missing);

// Opens a lock file, creating it when missing. The handle is only useful for
// exclusion; the lock file carries no data.
Result<NativeFile> open_lock_file(const std::string& path);

// Attempts to take an exclusive advisory lock over the whole file. Returns
// StoreLocked when another handle already holds it and wait is zero.
Status lock_exclusive(NativeFile file, Nanoseconds wait);

Status unlock(NativeFile file);

void close_file(NativeFile file) noexcept;

Result<std::vector<std::byte>> read_at(NativeFile file, std::uint64_t offset, std::size_t length);

Status write_at(NativeFile file, std::uint64_t offset, std::span<const std::byte> data);

Status flush_file(NativeFile file);

Result<std::uint64_t> file_size(NativeFile file);

Status set_file_size(NativeFile file, std::uint64_t size);

// Creates the file with the given size when it does not exist yet.
Status create_file(const std::string& path, std::uint64_t size);

Status delete_file(const std::string& path) noexcept;

Status rename_file(const std::string& from, const std::string& to);

// A unique staging name inside the given directory.
std::string staging_name(const std::string& directory, const std::string& stem,
                         std::uint64_t token);

}  // namespace detail
}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_SRC_PLATFORM_HPP
