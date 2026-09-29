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

#include "thermal_zone_manager/digest.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace thermal_zone_manager {
namespace {

constexpr std::uint32_t kCrc32cPolynomial = 0x82F63B78U;

constexpr std::array<std::uint32_t, 256> make_crc_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256U; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1U) != 0U ? (value >> 1) ^ kCrc32cPolynomial : (value >> 1);
    }
    table[index] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

std::string hex_of(std::uint64_t value, unsigned digits) {
  static const char* digits_table = "0123456789abcdef";
  std::string text(digits, '0');
  for (unsigned index = 0; index < digits; ++index) {
    const unsigned shift = static_cast<unsigned>((digits - 1U - index) * 4U);
    text[index] = digits_table[(value >> shift) & 0xFU];
  }
  return text;
}

}  // namespace

std::uint64_t fnv1a64(std::span<const std::byte> data, std::uint64_t seed) noexcept {
  std::uint64_t hash = seed;
  for (const std::byte item : data) {
    hash ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(item));
    hash *= kFnvPrime;
  }
  return hash;
}

std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t seed) noexcept {
  std::uint32_t crc = ~seed;
  for (const std::byte item : data) {
    const auto index = static_cast<std::uint8_t>(
        (crc ^ static_cast<std::uint32_t>(std::to_integer<unsigned char>(item))) & 0xFFU);
    crc = kCrcTable[index] ^ (crc >> 8);
  }
  return ~crc;
}

std::string to_hex(std::uint32_t value) { return hex_of(value, 8); }

std::string to_hex(std::uint64_t value) { return hex_of(value, 16); }

}  // namespace thermal_zone_manager
