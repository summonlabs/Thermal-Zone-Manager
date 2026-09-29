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

#include <cstdint>
#include <limits>
#include <string>

#include "test_harness.hpp"
#include "thermal_zone_manager/thermal_zone_manager.hpp"

using namespace thermal_zone_manager;

namespace {

std::span<const std::byte> as_bytes(const std::string& text) {
  return std::as_bytes(std::span(text.data(), text.size()));
}

}  // namespace

TZM_TEST(units, add_and_subtract_detect_overflow) {
  TZM_CHECK(add_checked(1, 2).value() == 3);
  TZM_CHECK(!add_checked(std::numeric_limits<std::int64_t>::max(), 1).has_value());
  TZM_CHECK(!add_checked(std::numeric_limits<std::int64_t>::min(), -1).has_value());
  TZM_CHECK(add_checked(std::numeric_limits<std::int64_t>::max(), 0).value() ==
            std::numeric_limits<std::int64_t>::max());
  TZM_CHECK(sub_checked(0, std::numeric_limits<std::int64_t>::max()).value() ==
            -std::numeric_limits<std::int64_t>::max());
  TZM_CHECK(!sub_checked(std::numeric_limits<std::int64_t>::min(), 1).has_value());
  TZM_CHECK(!sub_checked(std::numeric_limits<std::int64_t>::max(), -1).has_value());
}

TZM_TEST(units, multiply_detects_overflow) {
  TZM_CHECK(mul_checked(6, 7).value() == 42);
  TZM_CHECK(mul_checked(-6, 7).value() == -42);
  TZM_CHECK(mul_checked(-6, -7).value() == 42);
  TZM_CHECK(mul_checked(0, std::numeric_limits<std::int64_t>::min()).value() == 0);
  TZM_CHECK(!mul_checked(std::numeric_limits<std::int64_t>::max(), 2).has_value());
  TZM_CHECK(!mul_checked(std::numeric_limits<std::int64_t>::min(), 2).has_value());
  TZM_CHECK(!mul_checked(std::numeric_limits<std::int64_t>::min(), -1).has_value());
  TZM_CHECK(mul_checked(std::numeric_limits<std::int64_t>::min(), 1).value() ==
            std::numeric_limits<std::int64_t>::min());
}

TZM_TEST(units, division_rounds_in_the_documented_direction) {
  TZM_CHECK(div_floor(7, 2).value() == 3);
  TZM_CHECK(div_ceil(7, 2).value() == 4);
  TZM_CHECK(div_floor(-7, 2).value() == -4);
  TZM_CHECK(div_ceil(-7, 2).value() == -3);
  TZM_CHECK(div_floor(-8, 2).value() == -4);
  TZM_CHECK(div_ceil(-8, 2).value() == -4);
  TZM_CHECK(!div_floor(1, 0).has_value());
  TZM_CHECK(!div_ceil(1, 0).has_value());
  TZM_CHECK(!div_floor(1, -1).has_value());
}

TZM_TEST(units, mul_div_is_exact_beyond_sixty_four_bits) {
  // The product overflows a signed 64-bit value but the quotient does not, so
  // the exact answer must still be produced.
  const std::int64_t big = 4000000000000000000LL;
  TZM_CHECK(mul_div_floor(big, 4, 4).value() == big);
  TZM_CHECK(mul_div_ceil(big, 4, 4).value() == big);
  TZM_CHECK(mul_div_floor(big, 3, 3).value() == big);
  TZM_CHECK(mul_div_floor(std::numeric_limits<std::int64_t>::max(), 2, 2).value() ==
            std::numeric_limits<std::int64_t>::max());
  TZM_CHECK(!mul_div_floor(big, 4, 1).has_value());
  TZM_CHECK(!mul_div_floor(1, 1, 0).has_value());
}

TZM_TEST(units, mul_div_conservative_rounding) {
  // Coupled heat rounds up so headroom is never overstated.
  TZM_CHECK(mul_div_ceil(10, 1, 3).value() == 4);
  TZM_CHECK(mul_div_floor(10, 1, 3).value() == 3);
  TZM_CHECK(mul_div_ceil(-10, 1, 3).value() == -3);
  TZM_CHECK(mul_div_floor(-10, 1, 3).value() == -4);
  // Exact division is unaffected by the rounding direction.
  TZM_CHECK(mul_div_ceil(10, 2, 4).value() == 5);
  TZM_CHECK(mul_div_floor(10, 2, 4).value() == 5);
}

TZM_TEST(units, plausibility_bounds_are_inclusive) {
  TZM_CHECK(is_plausible(MilliCelsius{kAbsoluteZeroMilliCelsius}));
  TZM_CHECK(is_plausible(MilliCelsius{kMaxPlausibleMilliCelsius}));
  TZM_CHECK(!is_plausible(MilliCelsius{kAbsoluteZeroMilliCelsius - 1}));
  TZM_CHECK(!is_plausible(MilliCelsius{kMaxPlausibleMilliCelsius + 1}));
}

TZM_TEST(units, formatting_has_three_fractional_digits) {
  TZM_CHECK_EQ(format_milli(0), std::string("0.000"));
  TZM_CHECK_EQ(format_milli(5), std::string("0.005"));
  TZM_CHECK_EQ(format_milli(-1250), std::string("-1.250"));
  TZM_CHECK_EQ(format_milli(1000), std::string("1.000"));
  TZM_CHECK_EQ(format_milli(std::numeric_limits<std::int64_t>::min()),
               std::string("-9223372036854775.808"));
}

TZM_TEST(digest, crc32c_matches_the_standard_check_value) {
  const std::string check = "123456789";
  TZM_CHECK_EQ(crc32c(as_bytes(check)), 0xE3069283U);
  TZM_CHECK_EQ(crc32c(std::span<const std::byte>()), 0U);
}

TZM_TEST(digest, fnv1a_is_stable_and_order_sensitive) {
  TZM_CHECK_EQ(fnv1a64(as_bytes("")), kFnvOffsetBasis);
  const std::uint64_t first = fnv1a64(as_bytes("abc"));
  const std::uint64_t second = fnv1a64(as_bytes("acb"));
  TZM_CHECK_NE(first, second);
  TZM_CHECK_EQ(fnv1a64(as_bytes("abc")), first);
  TZM_CHECK_EQ(to_hex(static_cast<std::uint32_t>(0)), std::string("00000000"));
  TZM_CHECK_EQ(to_hex(static_cast<std::uint64_t>(0xABCDEF)), std::string("0000000000abcdef"));
}

TZM_TEST(version, reports_the_release_identity) {
  TZM_CHECK_EQ(version_string(), std::string("1.0.0"));
  static_assert(kVersionMajor == 1 && kVersionMinor == 0 && kVersionPatch == 0,
                "the release identity is 1.0.0");
  static_assert(kStoreFormatVersion >= 1, "the store format version starts at one");
  const std::string information = build_information();
  TZM_CHECK(information.find("thermal_zone_manager 1.0.0") != std::string::npos);
  TZM_CHECK(information.find("store format version") != std::string::npos);
}

TZM_TEST(limits, bounds_are_inclusive_at_the_edge) {
  TZM_CHECK(is_within_zone_limit(kMaxZones));
  TZM_CHECK(!is_within_zone_limit(kMaxZones + 1));
  TZM_CHECK(is_within_edge_limit(kMaxCouplingEdges));
  TZM_CHECK(!is_within_edge_limit(kMaxCouplingEdges + 1));
  TZM_CHECK(is_within_zone_limit(0));
}
