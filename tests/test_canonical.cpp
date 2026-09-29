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


#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "test_harness.hpp"
#include "thermal_zone_manager/thermal_zone_manager.hpp"

using namespace thermal_zone_manager;

namespace {

std::span<const std::byte> as_bytes(const std::string& text) {
  return std::as_bytes(std::span(text.data(), text.size()));
}

}  // namespace

TZM_TEST(canonical, round_trips_every_primitive) {
  ByteWriter writer;
  writer.put_u8(0xABU);
  writer.put_u16(0x1234U);
  writer.put_u32(0x89ABCDEFU);
  writer.put_u64(0x0123456789ABCDEFULL);
  writer.put_i64(-42);
  writer.put_block(as_bytes("block"));
  writer.put_text("text");
  writer.put_optional_text(false, "");
  writer.put_optional_text(true, "present");
  writer.put_optional_i64(false, 0);
  writer.put_optional_i64(true, -7);
  TZM_CHECK(!writer.overflowed());

  ByteReader reader(writer.bytes());
  TZM_CHECK_EQ(reader.u8().value(), static_cast<std::uint8_t>(0xABU));
  TZM_CHECK_EQ(reader.u16().value(), static_cast<std::uint16_t>(0x1234U));
  TZM_CHECK_EQ(reader.u32().value(), 0x89ABCDEFU);
  TZM_CHECK_EQ(reader.u64().value(), 0x0123456789ABCDEFULL);
  TZM_CHECK_EQ(reader.i64().value(), static_cast<std::int64_t>(-42));
  const auto block = reader.block(64);
  TZM_REQUIRE_OK(block);
  TZM_CHECK_EQ(block.value().size(), static_cast<std::size_t>(5));
  TZM_CHECK_EQ(reader.text(64).value(), std::string("text"));
  TZM_CHECK_EQ(reader.optional_marker().value(), false);
  TZM_CHECK_EQ(reader.optional_marker().value(), true);
  TZM_CHECK_EQ(reader.text(64).value(), std::string("present"));
  TZM_CHECK_EQ(reader.optional_marker().value(), false);
  TZM_CHECK_EQ(reader.optional_marker().value(), true);
  TZM_CHECK_EQ(reader.i64().value(), static_cast<std::int64_t>(-7));
  TZM_CHECK(reader.exhausted());
  TZM_CHECK_OK(reader.require_end());
}

TZM_TEST(canonical, encoding_is_little_endian_and_position_independent) {
  ByteWriter writer;
  writer.put_u32(0x01020304U);
  TZM_REQUIRE_EQ(writer.bytes().size(), static_cast<std::size_t>(4));
  TZM_CHECK_EQ(std::to_integer<unsigned>(writer.bytes()[0]), 0x04U);
  TZM_CHECK_EQ(std::to_integer<unsigned>(writer.bytes()[3]), 0x01U);
}

TZM_TEST(canonical, absent_and_present_but_empty_are_distinct) {
  ByteWriter absent;
  absent.put_optional_text(false, "");
  ByteWriter present;
  present.put_optional_text(true, "");
  TZM_CHECK_NE(absent.size(), present.size());
  ByteReader reader_absent(absent.bytes());
  ByteReader reader_present(present.bytes());
  TZM_CHECK_EQ(reader_absent.optional_marker().value(), false);
  TZM_CHECK_EQ(reader_present.optional_marker().value(), true);
  TZM_CHECK_EQ(reader_present.text(8).value(), std::string(""));
}

TZM_TEST(canonical, reader_rejects_trailing_bytes_and_truncation) {
  ByteWriter writer;
  writer.put_u32(7);
  writer.put_u8(1);
  ByteReader reader(writer.bytes());
  TZM_REQUIRE_OK(reader.u32());
  TZM_CHECK_ERR(reader.require_end(), ErrorCode::TrailingBytes);

  ByteReader truncated(std::span<const std::byte>(writer.bytes()).first(2));
  TZM_CHECK_ERR(truncated.u32(), ErrorCode::UnexpectedEndOfInput);

  ByteWriter marker;
  marker.put_u8(2);
  ByteReader bad_marker(marker.bytes());
  TZM_CHECK_ERR(bad_marker.optional_marker(), ErrorCode::StoreCorrupt);
}

TZM_TEST(canonical, declared_lengths_are_bounded_before_allocation) {
  ByteWriter writer;
  writer.put_u32(0xFFFFFFFFU);
  ByteReader reader(writer.bytes());
  TZM_CHECK_ERR(reader.block(1024), ErrorCode::PayloadTooLarge);

  ByteWriter small;
  small.put_text("abcd");
  ByteReader small_reader(small.bytes());
  TZM_CHECK_ERR(small_reader.text(2), ErrorCode::PayloadTooLarge);
}

TZM_TEST(canonical, writer_refuses_to_grow_past_its_ceiling) {
  ByteWriter writer;
  const std::size_t chunk = 1024 * 1024;
  std::vector<std::byte> payload(chunk, std::byte{0x5A});
  while (!writer.overflowed()) {
    writer.put_block(payload);
  }
  TZM_CHECK(writer.overflowed());
  TZM_CHECK(writer.size() <= ByteWriter::max_bytes());
}

TZM_TEST(canonical, fixed_width_helpers_round_trip) {
  std::vector<std::byte> buffer(8, std::byte{0});
  store_u32_le(buffer, 0xA1B2C3D4U);
  TZM_CHECK_EQ(load_u32_le(buffer), 0xA1B2C3D4U);
  store_u64_le(buffer, 0x1122334455667788ULL);
  TZM_CHECK_EQ(load_u64_le(buffer), 0x1122334455667788ULL);
  TZM_CHECK_EQ(load_u32_le(std::span<const std::byte>(buffer).first(2)), 0x7788U);
}
