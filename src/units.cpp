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

#include "thermal_zone_manager/units.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace thermal_zone_manager {
namespace {

// Unsigned magnitude of a signed 64-bit value. Two's complement is guaranteed
// by C++20, so the negation trick is exact even for the most negative value.
constexpr std::uint64_t magnitude(std::int64_t value) noexcept {
  return value < 0 ? (~static_cast<std::uint64_t>(value) + 1ULL)
                   : static_cast<std::uint64_t>(value);
}

constexpr std::uint64_t kInt64MaxMagnitude = 9223372036854775807ULL;
constexpr std::uint64_t kInt64MinMagnitude = 9223372036854775808ULL;

struct U128 {
  std::uint64_t hi;
  std::uint64_t lo;
};

// Exact 64x64 -> 128 bit product built from 32-bit limbs. No compiler
// intrinsic is required, so the arithmetic is identical on every toolchain.
constexpr U128 umul128(std::uint64_t a, std::uint64_t b) noexcept {
  const std::uint64_t a_lo = a & 0xFFFFFFFFULL;
  const std::uint64_t a_hi = a >> 32;
  const std::uint64_t b_lo = b & 0xFFFFFFFFULL;
  const std::uint64_t b_hi = b >> 32;
  const std::uint64_t p0 = a_lo * b_lo;
  const std::uint64_t p1 = a_lo * b_hi;
  const std::uint64_t p2 = a_hi * b_lo;
  const std::uint64_t p3 = a_hi * b_hi;
  const std::uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
  U128 result{};
  result.lo = (p0 & 0xFFFFFFFFULL) | (mid << 32);
  result.hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
  return result;
}

// Exact 128/64 division by restoring shift-subtract. Returns false when the
// quotient does not fit in 64 bits or the divisor is zero.
bool udiv128(U128 numerator, std::uint64_t divisor, std::uint64_t& quotient,
             std::uint64_t& remainder) noexcept {
  if (divisor == 0) {
    return false;
  }
  if (numerator.hi >= divisor) {
    return false;
  }
  std::uint64_t q = 0;
  std::uint64_t r = numerator.hi;
  for (int bit = 63; bit >= 0; --bit) {
    const std::uint64_t carry = r >> 63;
    r = (r << 1) | ((numerator.lo >> bit) & 1ULL);
    q <<= 1;
    if (carry != 0 || r >= divisor) {
      r -= divisor;
      q |= 1ULL;
    }
  }
  quotient = q;
  remainder = r;
  return true;
}

std::optional<std::int64_t> from_signed_magnitude(bool negative, std::uint64_t value) noexcept {
  if (negative) {
    if (value > kInt64MinMagnitude) {
      return std::nullopt;
    }
    if (value == kInt64MinMagnitude) {
      return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(value);
  }
  if (value > kInt64MaxMagnitude) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(value);
}

}  // namespace

std::optional<std::int64_t> add_checked(std::int64_t a, std::int64_t b) {
  if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
    return std::nullopt;
  }
  if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
    return std::nullopt;
  }
  return a + b;
}

std::optional<std::int64_t> sub_checked(std::int64_t a, std::int64_t b) {
  if (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b) {
    return std::nullopt;
  }
  if (b < 0 && a > std::numeric_limits<std::int64_t>::max() + b) {
    return std::nullopt;
  }
  return a - b;
}

std::optional<std::int64_t> mul_checked(std::int64_t a, std::int64_t b) {
  const bool negative = (a < 0) != (b < 0);
  const U128 product = umul128(magnitude(a), magnitude(b));
  if (product.hi != 0) {
    return std::nullopt;
  }
  return from_signed_magnitude(negative, product.lo);
}

std::optional<std::int64_t> div_floor(std::int64_t a, std::int64_t d) {
  if (d <= 0) {
    return std::nullopt;
  }
  std::int64_t quotient = a / d;
  const std::int64_t remainder = a % d;
  if (remainder != 0 && a < 0) {
    --quotient;
  }
  return quotient;
}

std::optional<std::int64_t> div_ceil(std::int64_t a, std::int64_t d) {
  if (d <= 0) {
    return std::nullopt;
  }
  std::int64_t quotient = a / d;
  const std::int64_t remainder = a % d;
  if (remainder != 0 && a > 0) {
    ++quotient;
  }
  return quotient;
}

std::optional<std::int64_t> mul_div_floor(std::int64_t a, std::int64_t b, std::int64_t d) {
  if (d <= 0) {
    return std::nullopt;
  }
  const bool negative = (a < 0) != (b < 0);
  const U128 product = umul128(magnitude(a), magnitude(b));
  std::uint64_t quotient = 0;
  std::uint64_t remainder = 0;
  if (!udiv128(product, static_cast<std::uint64_t>(d), quotient, remainder)) {
    return std::nullopt;
  }
  if (negative && remainder != 0) {
    if (quotient == std::numeric_limits<std::uint64_t>::max()) {
      return std::nullopt;
    }
    ++quotient;
  }
  return from_signed_magnitude(negative && quotient != 0, quotient);
}

std::optional<std::int64_t> mul_div_ceil(std::int64_t a, std::int64_t b, std::int64_t d) {
  if (d <= 0) {
    return std::nullopt;
  }
  const bool negative = (a < 0) != (b < 0);
  const U128 product = umul128(magnitude(a), magnitude(b));
  std::uint64_t quotient = 0;
  std::uint64_t remainder = 0;
  if (!udiv128(product, static_cast<std::uint64_t>(d), quotient, remainder)) {
    return std::nullopt;
  }
  if (!negative && remainder != 0) {
    if (quotient == std::numeric_limits<std::uint64_t>::max()) {
      return std::nullopt;
    }
    ++quotient;
  }
  return from_signed_magnitude(negative && quotient != 0, quotient);
}

bool is_plausible(MilliCelsius value) {
  return value.value >= kMinPlausibleMilliCelsius && value.value <= kMaxPlausibleMilliCelsius;
}

std::string format_milli(std::int64_t value) {
  const bool negative = value < 0;
  const std::uint64_t magnitude_value = magnitude(value);
  const std::uint64_t whole = magnitude_value / 1000ULL;
  const std::uint64_t fraction = magnitude_value % 1000ULL;
  std::string text;
  if (negative) {
    text.push_back('-');
  }
  text += std::to_string(whole);
  text.push_back('.');
  std::array<char, 4> digits{};
  digits[0] = static_cast<char>('0' + static_cast<int>((fraction / 100ULL) % 10ULL));
  digits[1] = static_cast<char>('0' + static_cast<int>((fraction / 10ULL) % 10ULL));
  digits[2] = static_cast<char>('0' + static_cast<int>(fraction % 10ULL));
  digits[3] = '\0';
  text.append(digits.data(), 3);
  return text;
}

}  // namespace thermal_zone_manager
