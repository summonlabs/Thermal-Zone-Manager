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

#ifndef THERMAL_ZONE_MANAGER_CANONICAL_HPP
#define THERMAL_ZONE_MANAGER_CANONICAL_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "thermal_zone_manager/errors.hpp"
#include "thermal_zone_manager/export.hpp"
#include "thermal_zone_manager/limits.hpp"

namespace thermal_zone_manager {

// Canonical encoding primitives.
//
// The encoding is little-endian, fixed width, and length prefixed. Every read
// is bounded: a declared length is checked against the caller's ceiling and
// against the bytes actually remaining before anything is allocated, and a
// reader that is not fully consumed reports TrailingBytes rather than
// ignoring the surplus.
//
// Two values with the same canonical bytes are the same value, and two values
// that differ in any encoded field have different canonical bytes. That is
// what makes the byte-for-byte determinism claims in this repository testable.

class ByteWriter {
 public:
  ByteWriter() = default;

  void put_u8(std::uint8_t value);
  void put_u16(std::uint16_t value);
  void put_u32(std::uint32_t value);
  void put_u64(std::uint64_t value);
  void put_i64(std::int64_t value);

  // Raw bytes with a 32-bit length prefix.
  void put_block(std::span<const std::byte> bytes);
  // UTF-8 text with a 32-bit byte-length prefix.
  void put_text(std::string_view text);
  // A present/absent marker followed, when present, by a length-prefixed
  // block. Absent and present-but-empty are therefore different encodings.
  void put_optional_block(bool present, std::span<const std::byte> bytes);
  void put_optional_text(bool present, std::string_view text);
  void put_optional_i64(bool present, std::int64_t value);

  std::size_t size() const noexcept { return bytes_.size(); }
  const std::vector<std::byte>& bytes() const noexcept { return bytes_; }
  std::vector<std::byte> take() && { return std::move(bytes_); }

  // Throws nothing; a writer that would exceed kMaxCanonicalBytes records that
  // fact and refuses further writes. Callers check overflowed().
  bool overflowed() const noexcept { return overflowed_; }

  static constexpr std::size_t max_bytes() noexcept { return kMaxCanonicalBytes; }

 private:
  void ensure(std::size_t extra);

  std::vector<std::byte> bytes_;
  bool overflowed_ = false;
};

class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  Result<std::uint8_t> u8();
  Result<std::uint16_t> u16();
  Result<std::uint32_t> u32();
  Result<std::uint64_t> u64();
  Result<std::int64_t> i64();

  // Reads a 32-bit length then that many bytes. The length is rejected before
  // allocation when it exceeds the ceiling or the remaining input.
  Result<std::span<const std::byte>> block(std::size_t max_length);
  Result<std::string> text(std::size_t max_length);
  Result<bool> optional_marker();
  Result<std::span<const std::byte>> optional_block(bool present, std::size_t max_length);

  std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
  bool exhausted() const noexcept { return offset_ == bytes_.size(); }
  std::size_t offset() const noexcept { return offset_; }

  // TrailingBytes unless every input byte was consumed.
  Status require_end() const;

 private:
  Status need(std::size_t count);

  std::span<const std::byte> bytes_;
  std::size_t offset_ = 0;
};

// Little-endian helpers used by the durable store for its fixed headers. Each
// honours the width of the field it belongs to; a wider store would silently
// overwrite the neighbouring field.
TZM_API void store_u16_le(std::span<std::byte> out, std::uint16_t value);
TZM_API void store_u32_le(std::span<std::byte> out, std::uint32_t value);
TZM_API void store_u64_le(std::span<std::byte> out, std::uint64_t value);
TZM_API std::uint16_t load_u16_le(std::span<const std::byte> in);
TZM_API std::uint32_t load_u32_le(std::span<const std::byte> in);
TZM_API std::uint64_t load_u64_le(std::span<const std::byte> in);

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_CANONICAL_HPP
