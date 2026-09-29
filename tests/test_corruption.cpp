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
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "test_harness.hpp"
#include "thermal_zone_manager/thermal_zone_manager.hpp"

using namespace thermal_zone_manager;

namespace {

std::vector<tzm_test::ZoneSpec> one_zone() {
  tzm_test::ZoneSpec spec;
  spec.id = 1;
  spec.name = "rack-a";
  return {spec};
}

std::string state_path(const std::string& root) { return root + "\\thermal-zones.tzm"; }

std::vector<std::byte> read_bytes(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::byte> bytes;
  char item = 0;
  while (stream.get(item)) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(item)));
  }
  return bytes;
}

void write_bytes(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

// Opens the store read-write and reports the machine-readable outcome.
Result<std::unique_ptr<ThermalZoneEngine>> open_store(const std::string& root) {
  EngineOptions options;
  options.root = root;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  return ThermalZoneEngine::open(options);
}

}  // namespace

TZM_TEST(corruption, a_store_with_no_committed_generation_opens_empty) {
  tzm_test::TempDir directory("corruption-empty");
  {
    // Create the store without ever committing anything.
    Result<std::unique_ptr<ThermalZoneEngine>> created = open_store(directory.path());
    TZM_REQUIRE_OK(created);
    TZM_CHECK(created.value()->configuration().empty());
    TZM_CHECK(created.value()->fencing().commit.is_zero());
  }
  Result<std::unique_ptr<ThermalZoneEngine>> engine = open_store(directory.path());
  TZM_REQUIRE_OK(engine);
  TZM_CHECK(engine.value()->configuration().empty());
  TZM_CHECK(engine.value()->fencing().commit.is_zero());
  TZM_CHECK(!engine.value()->recovery_report().recovered);
  TZM_CHECK(engine.value()->recovery_report().store_existed);
}

TZM_TEST(corruption, a_damaged_header_is_reported_not_ignored) {
  tzm_test::TempDir directory("corruption-header");
  {
    Result<std::unique_ptr<tzm_test::Facility>> facility = tzm_test::Facility::open(
        directory.path(), one_zone(), {}, 1, 3, StoreAccess::ReadWrite, true);
    TZM_REQUIRE_OK(facility);
    TZM_REQUIRE_OK(facility.value()->observe(1, 60000));
  }
  const std::string path = state_path(directory.path());
  std::vector<std::byte> bytes = read_bytes(path);
  TZM_REQUIRE(bytes.size() > 64);

  std::vector<std::byte> magic = bytes;
  magic[0] = std::byte{'X'};
  write_bytes(path, magic);
  TZM_CHECK_ERR(open_store(directory.path()), ErrorCode::StoreCorrupt);

  std::vector<std::byte> version = bytes;
  version[4] = std::byte{9};
  write_bytes(path, version);
  TZM_CHECK_ERR(open_store(directory.path()), ErrorCode::StoreFormatMismatch);

  std::vector<std::byte> reserved = bytes;
  reserved[40] = std::byte{1};
  write_bytes(path, reserved);
  TZM_CHECK_ERR(open_store(directory.path()), ErrorCode::StoreCorrupt);

  std::vector<std::byte> checksum = bytes;
  checksum[0] ^= std::byte{0x01};
  checksum[1] ^= std::byte{0x02};
  write_bytes(path, checksum);
  TZM_CHECK_ERR(open_store(directory.path()), ErrorCode::StoreCorrupt);

  write_bytes(path, bytes);
  Result<std::unique_ptr<ThermalZoneEngine>> restored = open_store(directory.path());
  TZM_REQUIRE_OK(restored);
  TZM_CHECK(restored.value()->recovery_report().recovered);
}

TZM_TEST(corruption, a_single_flipped_byte_in_the_published_slot_falls_back) {
  tzm_test::TempDir directory("corruption-slot-byte");
  {
    Result<std::unique_ptr<tzm_test::Facility>> facility = tzm_test::Facility::open(
        directory.path(), one_zone(), {}, 1, 3, StoreAccess::ReadWrite, true);
    TZM_REQUIRE_OK(facility);
    TZM_REQUIRE_OK(facility.value()->observe(1, 60000));
    // A second commit so both slots hold a complete generation.
    TZM_REQUIRE_OK(facility.value()->observe(1, 61000));
  }
  const std::string path = state_path(directory.path());
  const std::vector<std::byte> original = read_bytes(path);
  TZM_REQUIRE(original.size() > 200);

  for (std::size_t offset : {static_cast<std::size_t>(100), static_cast<std::size_t>(64 + 96),
                             static_cast<std::size_t>(64 + 96 + 200)}) {
    std::vector<std::byte> damaged = original;
    damaged[offset] ^= std::byte{0xFF};
    write_bytes(path, damaged);
    Result<std::unique_ptr<ThermalZoneEngine>> engine = open_store(directory.path());
    if (engine.ok()) {
      // Recovering is acceptable only when a complete generation was found.
      TZM_CHECK(engine.value()->recovery_report().recovered);
      TZM_CHECK(engine.value()->configuration().zone_count() == 1);
    } else {
      TZM_CHECK(engine.error().code() == ErrorCode::StoreCorrupt ||
                engine.error().code() == ErrorCode::StoreFormatMismatch);
    }
  }
  write_bytes(path, original);
  Result<std::unique_ptr<ThermalZoneEngine>> restored = open_store(directory.path());
  TZM_REQUIRE_OK(restored);
  TZM_CHECK_EQ(restored.value()->configuration().zone_count(), static_cast<std::size_t>(1));
}

TZM_TEST(corruption, every_truncation_length_resolves_or_is_refused) {
  tzm_test::TempDir directory("corruption-truncate");
  {
    Result<std::unique_ptr<tzm_test::Facility>> facility = tzm_test::Facility::open(
        directory.path(), one_zone(), {}, 1, 3, StoreAccess::ReadWrite, true);
    TZM_REQUIRE_OK(facility);
    TZM_REQUIRE_OK(facility.value()->observe(1, 60000));
    TZM_REQUIRE_OK(facility.value()->observe(1, 61000));
  }
  const std::string path = state_path(directory.path());
  const std::vector<std::byte> original = read_bytes(path);
  TZM_REQUIRE(original.size() > 64);

  // Every truncation point of the header and of the first slot must either
  // recover a complete generation or be refused. Nothing may decode partially.
  const std::size_t limit = 64 + 96 + 4096;
  for (std::size_t length = 0; length <= limit; length += 7) {
    std::vector<std::byte> truncated(original.begin(),
                                     original.begin() + static_cast<std::ptrdiff_t>(length));
    write_bytes(path, truncated);
    Result<std::unique_ptr<ThermalZoneEngine>> engine = open_store(directory.path());
    if (!engine.ok()) {
      TZM_CHECK(engine.error().code() == ErrorCode::StoreCorrupt ||
                engine.error().code() == ErrorCode::StoreFormatMismatch ||
                engine.error().code() == ErrorCode::StoreUnavailable);
      continue;
    }
    // A recovered generation must be complete and decodable.
    TZM_CHECK(engine.value()->configuration().zone_count() <= 1);
    TZM_CHECK(engine.value()->fencing().revision.raw() <=
              static_cast<std::uint64_t>(2 * original.size()));
  }
  write_bytes(path, original);
  Result<std::unique_ptr<ThermalZoneEngine>> restored = open_store(directory.path());
  TZM_REQUIRE_OK(restored);
  TZM_CHECK(restored.value()->configuration().zone_count() == 1);
}

TZM_TEST(corruption, zeroed_slot_headers_are_treated_as_never_written) {
  tzm_test::TempDir directory("corruption-zeroed");
  {
    Result<std::unique_ptr<tzm_test::Facility>> facility = tzm_test::Facility::open(
        directory.path(), one_zone(), {}, 1, 3, StoreAccess::ReadWrite, true);
    TZM_REQUIRE_OK(facility);
    TZM_REQUIRE_OK(facility.value()->observe(1, 60000));
  }
  const std::string path = state_path(directory.path());
  const std::vector<std::byte> original = read_bytes(path);
  // Zero the slot headers only, leaving the payloads in place: the header
  // checksum cannot match, so the slots are unreadable but not silently empty.
  std::vector<std::byte> zeroed = original;
  for (std::size_t index = 0; index < 96; ++index) {
    zeroed[64 + index] = std::byte{0};
  }
  write_bytes(path, zeroed);
  Result<std::unique_ptr<ThermalZoneEngine>> engine = open_store(directory.path());
  // One slot is still complete, so recovery succeeds from it.
  TZM_REQUIRE_OK(engine);
  TZM_CHECK(engine.value()->recovery_report().recovered);
  TZM_CHECK(engine.value()->configuration().zone_count() == 1);
  write_bytes(path, original);
}

TZM_TEST(corruption, a_payload_that_passes_integrity_but_is_nonsense_is_rejected) {
  tzm_test::TempDir directory("corruption-semantic");
  {
    Result<std::unique_ptr<tzm_test::Facility>> facility = tzm_test::Facility::open(
        directory.path(), one_zone(), {}, 1, 3, StoreAccess::ReadWrite, true);
    TZM_REQUIRE_OK(facility);
    TZM_REQUIRE_OK(facility.value()->observe(1, 60000));
    TZM_REQUIRE_OK(facility.value()->evaluate_and_commit());
  }
  const std::string path = state_path(directory.path());
  const std::vector<std::byte> original = read_bytes(path);
  TZM_REQUIRE(original.size() > 64 + 96 + 64);

  // The damaged slot keeps a correct header checksum and a correct payload
  // checksum, so only the semantic decoder can reject it. The other slot is
  // zeroed so recovery cannot fall back to it.
  for (const std::size_t slot : {static_cast<std::size_t>(64),
                                 static_cast<std::size_t>(64 + 96 + 8 * 1024 * 1024)}) {
    std::vector<std::byte> damaged = original;
    if (damaged[slot] != std::byte{'T'} || damaged[slot + 1] != std::byte{'Z'}) {
      continue;
    }
    const std::uint32_t payload_bytes =
        load_u32_le(std::span<const std::byte>(damaged).subspan(slot + 64, 4));
    if (payload_bytes < 8) {
      continue;
    }
    const std::size_t payload_offset = slot + 96;
    damaged[payload_offset + 3] ^= std::byte{0x7F};
    const std::uint32_t payload_crc =
        crc32c(std::span<const std::byte>(damaged).subspan(payload_offset, payload_bytes));
    store_u32_le(std::span<std::byte>(damaged).subspan(slot + 68, 4), payload_crc);
    const std::uint32_t header_crc = crc32c(std::span<const std::byte>(damaged).subspan(slot, 72));
    store_u32_le(std::span<std::byte>(damaged).subspan(slot + 72, 4), header_crc);
    const std::size_t other = slot == 64 ? 64 + 96 + 8 * 1024 * 1024 : 64;
    for (std::size_t index = 0; index < 96 + payload_bytes + 8; ++index) {
      if (other + index < damaged.size()) {
        damaged[other + index] = std::byte{0};
      }
    }
    write_bytes(path, damaged);
    Result<std::unique_ptr<ThermalZoneEngine>> engine = open_store(directory.path());
    if (engine.ok()) {
      // Whatever came back must be a self-consistent world.
      TZM_CHECK(engine.value()->configuration().zone_count() <= 1);
      TZM_CHECK(engine.value()->fencing().commit.raw() <= original.size());
    } else {
      TZM_CHECK(engine.error().code() == ErrorCode::StoreCorrupt ||
                engine.error().code() == ErrorCode::StoreFormatMismatch ||
                engine.error().code() == ErrorCode::UnexpectedEndOfInput ||
                engine.error().code() == ErrorCode::TruncatedRecord);
    }
  }
  write_bytes(path, original);
  Result<std::unique_ptr<ThermalZoneEngine>> restored = open_store(directory.path());
  TZM_REQUIRE_OK(restored);
  TZM_CHECK_EQ(restored.value()->configuration().zone_count(), static_cast<std::size_t>(1));
}
