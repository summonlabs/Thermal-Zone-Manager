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

#ifndef TZM_TEST_BIGINT_HPP
#define TZM_TEST_BIGINT_HPP

#include <cstdint>
#include <vector>

// A deliberately independent exact-integer helper for the reference model. It
// uses decimal limbs and long division, so it shares no arithmetic code and no
// representation with the library's binary 128-bit helpers. Its purpose is to
// make the reference model an independent check of the exact rational
// operations, not a second copy of them.

namespace tzm_test {

class BigUInt {
 public:
  BigUInt() : limbs_(1, 0) {}

  static BigUInt from_u64(std::uint64_t value) {
    BigUInt result;
    result.limbs_.clear();
    while (value != 0) {
      result.limbs_.push_back(static_cast<std::uint32_t>(value % kBase));
      value /= kBase;
    }
    if (result.limbs_.empty()) {
      result.limbs_.push_back(0);
    }
    return result;
  }

  bool is_zero() const { return limbs_.size() == 1 && limbs_[0] == 0; }

  void multiply_u64(std::uint64_t factor) {
    std::uint64_t carry = 0;
    for (std::uint32_t& limb : limbs_) {
      const std::uint64_t product = static_cast<std::uint64_t>(limb) * factor + carry;
      limb = static_cast<std::uint32_t>(product % kBase);
      carry = product / kBase;
    }
    while (carry != 0) {
      limbs_.push_back(static_cast<std::uint32_t>(carry % kBase));
      carry /= kBase;
    }
  }

  // Long division in base 10^9. Returns the quotient; the remainder is written
  // to remainder when it is not null.
  BigUInt divide_u64(std::uint64_t divisor, std::uint64_t* remainder) const {
    BigUInt quotient;
    quotient.limbs_.assign(limbs_.size(), 0);
    std::uint64_t carry = 0;
    for (std::size_t index = limbs_.size(); index-- > 0;) {
      const std::uint64_t current = carry * kBase + limbs_[index];
      quotient.limbs_[index] = static_cast<std::uint32_t>(current / divisor);
      carry = current % divisor;
    }
    while (quotient.limbs_.size() > 1 && quotient.limbs_.back() == 0) {
      quotient.limbs_.pop_back();
    }
    if (remainder != nullptr) {
      *remainder = carry;
    }
    return quotient;
  }

  // Saturating conversion; the reference model only ever converts values it
  // already knows are representable.
  std::uint64_t to_u64() const {
    std::uint64_t value = 0;
    for (std::size_t index = limbs_.size(); index-- > 0;) {
      value = value * kBase + limbs_[index];
    }
    return value;
  }

 private:
  static constexpr std::uint64_t kBase = 1000000000ULL;
  std::vector<std::uint32_t> limbs_;
};

// floor(a * b / d) and ceil(a * b / d) for non-negative a, b and positive d.
inline std::uint64_t ref_mul_div_floor(std::uint64_t a, std::uint64_t b, std::uint64_t d) {
  BigUInt product = BigUInt::from_u64(a);
  product.multiply_u64(b);
  return product.divide_u64(d, nullptr).to_u64();
}

inline std::uint64_t ref_mul_div_ceil(std::uint64_t a, std::uint64_t b, std::uint64_t d) {
  BigUInt product = BigUInt::from_u64(a);
  product.multiply_u64(b);
  std::uint64_t remainder = 0;
  const std::uint64_t quotient = product.divide_u64(d, &remainder).to_u64();
  return remainder == 0 ? quotient : quotient + 1;
}

inline std::uint64_t ref_mul_div_remainder(std::uint64_t a, std::uint64_t b, std::uint64_t d) {
  BigUInt product = BigUInt::from_u64(a);
  product.multiply_u64(b);
  std::uint64_t remainder = 0;
  product.divide_u64(d, &remainder);
  return remainder;
}

// floor(a * b / d) for a signed a, a non-negative b and a positive d, rounding
// towards negative infinity exactly as the documented semantics require.
inline std::int64_t ref_floor_mul_div(std::int64_t a, std::uint64_t b, std::uint64_t d) {
  const bool negative = a < 0;
  const std::uint64_t magnitude =
      negative ? static_cast<std::uint64_t>(-(a + 1)) + 1U : static_cast<std::uint64_t>(a);
  const std::uint64_t quotient = ref_mul_div_floor(magnitude, b, d);
  const std::uint64_t remainder = ref_mul_div_remainder(magnitude, b, d);
  const std::uint64_t rounded = (negative && remainder != 0) ? quotient + 1U : quotient;
  return negative ? -static_cast<std::int64_t>(rounded) : static_cast<std::int64_t>(rounded);
}

// ceil(a * b / d) with the same exactness.
inline std::int64_t ref_ceil_mul_div(std::int64_t a, std::uint64_t b, std::uint64_t d) {
  const bool negative = a < 0;
  const std::uint64_t magnitude =
      negative ? static_cast<std::uint64_t>(-(a + 1)) + 1U : static_cast<std::uint64_t>(a);
  const std::uint64_t quotient = ref_mul_div_floor(magnitude, b, d);
  const std::uint64_t remainder = ref_mul_div_remainder(magnitude, b, d);
  const std::uint64_t rounded = (!negative && remainder != 0) ? quotient + 1U : quotient;
  return negative ? -static_cast<std::int64_t>(rounded) : static_cast<std::int64_t>(rounded);
}

}  // namespace tzm_test

#endif  // TZM_TEST_BIGINT_HPP
