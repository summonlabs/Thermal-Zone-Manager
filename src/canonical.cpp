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

#include "thermal_zone_manager/canonical.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "thermal_zone_manager/limits.hpp"

namespace thermal_zone_manager {
namespace {

constexpr std::size_t kMaxDeclaredLength = kMaxCanonicalBytes;

}  // namespace

void ByteWriter::ensure(std::size_t extra) {
  if (overflowed_) {
    return;
  }
  if (extra > kMaxCanonicalBytes || bytes_.size() > kMaxCanonicalBytes - extra) {
    overflowed_ = true;
  }
}

void ByteWriter::put_u8(std::uint8_t value) {
  ensure(1);
  if (overflowed_) {
    return;
  }
  bytes_.push_back(static_cast<std::byte>(value));
}

void ByteWriter::put_u16(std::uint16_t value) {
  put_u8(static_cast<std::uint8_t>(value & 0xFFU));
  put_u8(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
}

void ByteWriter::put_u32(std::uint32_t value) {
  for (unsigned index = 0; index < 4U; ++index) {
    put_u8(static_cast<std::uint8_t>((value >> (8U * index)) & 0xFFU));
  }
}

void ByteWriter::put_u64(std::uint64_t value) {
  for (unsigned index = 0; index < 8U; ++index) {
    put_u8(static_cast<std::uint8_t>((value >> (8U * index)) & 0xFFU));
  }
}

void ByteWriter::put_i64(std::int64_t value) {
  put_u64(static_cast<std::uint64_t>(value));
}

void ByteWriter::put_block(std::span<const std::byte> bytes) {
  if (bytes.size() > kMaxDeclaredLength) {
    overflowed_ = true;
    return;
  }
  put_u32(static_cast<std::uint32_t>(bytes.size()));
  ensure(bytes.size());
  if (overflowed_) {
    return;
  }
  bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::put_text(std::string_view text) { put_block(std::as_bytes(std::span(text))); }

void ByteWriter::put_optional_block(bool present, std::span<const std::byte> bytes) {
  put_u8(present ? 1U : 0U);
  if (present) {
    put_block(bytes);
  }
}

void ByteWriter::put_optional_text(bool present, std::string_view text) {
  put_u8(present ? 1U : 0U);
  if (present) {
    put_text(text);
  }
}

void ByteWriter::put_optional_i64(bool present, std::int64_t value) {
  put_u8(present ? 1U : 0U);
  if (present) {
    put_i64(value);
  }
}

Status ByteReader::need(std::size_t count) {
  if (count > remaining()) {
    return Error(ErrorCode::UnexpectedEndOfInput, "the input ended before the record was complete")
        .with("needed", static_cast<std::uint64_t>(count))
        .with("remaining", static_cast<std::uint64_t>(remaining()));
  }
  return Status();
}

Result<std::uint8_t> ByteReader::u8() {
  const Status available = need(1);
  if (!available.ok()) {
    return available.error();
  }
  const auto value = std::to_integer<std::uint8_t>(bytes_[offset_]);
  ++offset_;
  return value;
}

Result<std::uint16_t> ByteReader::u16() {
  std::uint16_t value = 0;
  for (unsigned index = 0; index < 2U; ++index) {
    Result<std::uint8_t> byte = u8();
    if (!byte.ok()) {
      return byte.error();
    }
    value = static_cast<std::uint16_t>(value | static_cast<std::uint16_t>(
                                                   static_cast<std::uint16_t>(byte.value())
                                                   << (8U * index)));
  }
  return value;
}

Result<std::uint32_t> ByteReader::u32() {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4U; ++index) {
    Result<std::uint8_t> byte = u8();
    if (!byte.ok()) {
      return byte.error();
    }
    value |= static_cast<std::uint32_t>(byte.value()) << (8U * index);
  }
  return value;
}

Result<std::uint64_t> ByteReader::u64() {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8U; ++index) {
    Result<std::uint8_t> byte = u8();
    if (!byte.ok()) {
      return byte.error();
    }
    value |= static_cast<std::uint64_t>(byte.value()) << (8U * index);
  }
  return value;
}

Result<std::int64_t> ByteReader::i64() {
  Result<std::uint64_t> raw = u64();
  if (!raw.ok()) {
    return raw.error();
  }
  return static_cast<std::int64_t>(raw.value());
}

Result<std::span<const std::byte>> ByteReader::block(std::size_t max_length) {
  Result<std::uint32_t> declared = u32();
  if (!declared.ok()) {
    return declared.error();
  }
  const auto length = static_cast<std::size_t>(declared.value());
  if (length > max_length || length > kMaxDeclaredLength) {
    return Error(ErrorCode::PayloadTooLarge,
                 "a declared length exceeds the ceiling for this record")
        .with("declared", static_cast<std::uint64_t>(length))
        .with("limit", static_cast<std::uint64_t>(max_length));
  }
  const Status available = need(length);
  if (!available.ok()) {
    return available.error();
  }
  const std::span<const std::byte> slice = bytes_.subspan(offset_, length);
  offset_ += length;
  return slice;
}

Result<std::string> ByteReader::text(std::size_t max_length) {
  Result<std::span<const std::byte>> raw = block(max_length);
  if (!raw.ok()) {
    return raw.error();
  }
  std::string text;
  text.reserve(raw.value().size());
  for (const std::byte item : raw.value()) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(item)));
  }
  return text;
}

Result<bool> ByteReader::optional_marker() {
  Result<std::uint8_t> marker = u8();
  if (!marker.ok()) {
    return marker.error();
  }
  if (marker.value() > 1U) {
    return Error(ErrorCode::StoreCorrupt, "an optional marker must be 0 or 1")
        .with("value", static_cast<std::uint64_t>(marker.value()));
  }
  return marker.value() == 1U;
}

Result<std::span<const std::byte>> ByteReader::optional_block(bool present,
                                                              std::size_t max_length) {
  if (!present) {
    return std::span<const std::byte>();
  }
  return block(max_length);
}

Status ByteReader::require_end() const {
  if (exhausted()) {
    return Status();
  }
  return Error(ErrorCode::TrailingBytes, "the record was followed by unconsumed bytes")
      .with("trailing", static_cast<std::uint64_t>(remaining()));
}

void store_u16_le(std::span<std::byte> out, std::uint16_t value) {
  for (unsigned index = 0; index < 2U && index < out.size(); ++index) {
    out[index] = static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
  }
}

void store_u32_le(std::span<std::byte> out, std::uint32_t value) {
  for (unsigned index = 0; index < 4U && index < out.size(); ++index) {
    out[index] = static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
  }
}

void store_u64_le(std::span<std::byte> out, std::uint64_t value) {
  for (unsigned index = 0; index < 8U && index < out.size(); ++index) {
    out[index] = static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
  }
}

std::uint16_t load_u16_le(std::span<const std::byte> in) {
  std::uint16_t value = 0;
  for (unsigned index = 0; index < 2U && index < in.size(); ++index) {
    value = static_cast<std::uint16_t>(
        value | static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(in[index]))
                    << (8U * index)));
  }
  return value;
}

std::uint32_t load_u32_le(std::span<const std::byte> in) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4U && index < in.size(); ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[index])) << (8U * index);
  }
  return value;
}

std::uint64_t load_u64_le(std::span<const std::byte> in) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8U && index < in.size(); ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(in[index])) << (8U * index);
  }
  return value;
}

}  // namespace thermal_zone_manager
