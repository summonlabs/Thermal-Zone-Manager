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

#ifndef THERMAL_ZONE_MANAGER_UNITS_HPP
#define THERMAL_ZONE_MANAGER_UNITS_HPP

#include <cstdint>
#include <optional>
#include <string>

#include "thermal_zone_manager/export.hpp"

// Exact integer physical units. Every authoritative thermal decision in this
// runtime is made with the checked integer arithmetic declared here. Floating
// point is never used for an authority-bearing computation.
//
//   MilliCelsius        millidegrees Celsius      1 mC   = 0.001 C
//   MilliWatts          milliwatts                1 mW   = 0.001 W
//   MicroKelvinPerWatt  microkelvin per watt      1 uK/W = 1e-6 K/W
//   PartsPerMillion     dimensionless ratio       1 ppm  = 1e-6
//   Nanoseconds         nanoseconds
//   Timestamp           nanoseconds since the Unix epoch (UTC)
//
// Dimensional identity used by the headroom maths:
//
//   rise[mC] = heat[mW] * resistance[uK/W] / 1000000000
//   heat[mW] = rise[mC] * 1000000 / resistance[uK/W]
//
// Derivation: P[W] = heat[mW] / 1000, R[K/W] = resistance[uK/W] / 1000000, and
// rise[K] = P[W] * R[K/W], so rise[mC] = rise[K] * 1000.
//
// Both directions are exact rational operations and are evaluated with 128-bit
// intermediates, so no authoritative result depends on floating point.

namespace thermal_zone_manager {

// Physical plausibility bounds. Values outside these ranges are refused rather
// than clamped: an implausible temperature is a bad request, not a cold zone.
inline constexpr std::int64_t kAbsoluteZeroMilliCelsius = -273150;
inline constexpr std::int64_t kMaxPlausibleMilliCelsius = 1000000;   // 1000 C
inline constexpr std::int64_t kMinPlausibleMilliCelsius = kAbsoluteZeroMilliCelsius;
inline constexpr std::int64_t kMaxPlausibleMilliWatts = 1000000000000000LL;  // 1e12 mW = 1 GW
inline constexpr std::int64_t kMaxPlausibleResistance = 1000000000LL;        // 1e9 uK/W
inline constexpr std::int32_t kPpmScale = 1000000;                            // 1e6

// Millidegrees Celsius.
struct MilliCelsius {
  std::int64_t value = 0;

  friend constexpr bool operator==(MilliCelsius a, MilliCelsius b) { return a.value == b.value; }
  friend constexpr bool operator!=(MilliCelsius a, MilliCelsius b) { return a.value != b.value; }
  friend constexpr bool operator<(MilliCelsius a, MilliCelsius b) { return a.value < b.value; }
  friend constexpr bool operator<=(MilliCelsius a, MilliCelsius b) { return a.value <= b.value; }
  friend constexpr bool operator>(MilliCelsius a, MilliCelsius b) { return a.value > b.value; }
  friend constexpr bool operator>=(MilliCelsius a, MilliCelsius b) { return a.value >= b.value; }
};

// Milliwatts of dissipated heat.
struct MilliWatts {
  std::int64_t value = 0;

  friend constexpr bool operator==(MilliWatts a, MilliWatts b) { return a.value == b.value; }
  friend constexpr bool operator!=(MilliWatts a, MilliWatts b) { return a.value != b.value; }
  friend constexpr bool operator<(MilliWatts a, MilliWatts b) { return a.value < b.value; }
  friend constexpr bool operator<=(MilliWatts a, MilliWatts b) { return a.value <= b.value; }
  friend constexpr bool operator>(MilliWatts a, MilliWatts b) { return a.value > b.value; }
  friend constexpr bool operator>=(MilliWatts a, MilliWatts b) { return a.value >= b.value; }
};

// Thermal resistance from the zone to its declared reference temperature,
// expressed in microkelvin per watt. Must be strictly positive; a zero or
// negative resistance is meaningless and is refused.
struct MicroKelvinPerWatt {
  std::int64_t value = 0;

  friend constexpr bool operator==(MicroKelvinPerWatt a, MicroKelvinPerWatt b) {
    return a.value == b.value;
  }
  friend constexpr bool operator!=(MicroKelvinPerWatt a, MicroKelvinPerWatt b) {
    return a.value != b.value;
  }
  friend constexpr bool operator<(MicroKelvinPerWatt a, MicroKelvinPerWatt b) {
    return a.value < b.value;
  }
  friend constexpr bool operator<=(MicroKelvinPerWatt a, MicroKelvinPerWatt b) {
    return a.value <= b.value;
  }
};

// A bounded dimensionless ratio in parts per million. The range is [0, 1e6].
struct PartsPerMillion {
  std::int32_t value = 0;

  friend constexpr bool operator==(PartsPerMillion a, PartsPerMillion b) {
    return a.value == b.value;
  }
  friend constexpr bool operator!=(PartsPerMillion a, PartsPerMillion b) {
    return a.value != b.value;
  }
  friend constexpr bool operator<(PartsPerMillion a, PartsPerMillion b) {
    return a.value < b.value;
  }
  friend constexpr bool operator<=(PartsPerMillion a, PartsPerMillion b) {
    return a.value <= b.value;
  }
};

// A duration in nanoseconds. Never negative.
struct Nanoseconds {
  std::int64_t value = 0;

  friend constexpr bool operator==(Nanoseconds a, Nanoseconds b) { return a.value == b.value; }
  friend constexpr bool operator!=(Nanoseconds a, Nanoseconds b) { return a.value != b.value; }
  friend constexpr bool operator<(Nanoseconds a, Nanoseconds b) { return a.value < b.value; }
  friend constexpr bool operator<=(Nanoseconds a, Nanoseconds b) { return a.value <= b.value; }
};

// Nanoseconds since the Unix epoch, UTC. A Timestamp is an instant, not a
// duration, and the two are deliberately distinct types.
struct Timestamp {
  std::int64_t value = 0;

  friend constexpr bool operator==(Timestamp a, Timestamp b) { return a.value == b.value; }
  friend constexpr bool operator!=(Timestamp a, Timestamp b) { return a.value != b.value; }
  friend constexpr bool operator<(Timestamp a, Timestamp b) { return a.value < b.value; }
  friend constexpr bool operator<=(Timestamp a, Timestamp b) { return a.value <= b.value; }
  friend constexpr bool operator>(Timestamp a, Timestamp b) { return a.value > b.value; }
  friend constexpr bool operator>=(Timestamp a, Timestamp b) { return a.value >= b.value; }
};

// ---------------------------------------------------------------------------
// Checked exact arithmetic
// ---------------------------------------------------------------------------

// Returns a + b when the exact sum is representable, otherwise nullopt.
TZM_API std::optional<std::int64_t> add_checked(std::int64_t a, std::int64_t b);

// Returns a - b when the exact difference is representable, otherwise nullopt.
TZM_API std::optional<std::int64_t> sub_checked(std::int64_t a, std::int64_t b);

// Returns a * b when the exact product is representable, otherwise nullopt.
TZM_API std::optional<std::int64_t> mul_checked(std::int64_t a, std::int64_t b);

// Floor(a / d) with d > 0, rounding towards negative infinity. Returns nullopt
// when d <= 0.
TZM_API std::optional<std::int64_t> div_floor(std::int64_t a, std::int64_t d);

// Ceiling(a / d) with d > 0, rounding towards positive infinity. Returns
// nullopt when d <= 0.
TZM_API std::optional<std::int64_t> div_ceil(std::int64_t a, std::int64_t d);

// Floor(a * b / d) evaluated with a 128-bit intermediate product, so the
// result is exact whenever it is representable even when a * b is not.
// Returns nullopt when d <= 0 or the quotient is not representable.
TZM_API std::optional<std::int64_t> mul_div_floor(std::int64_t a, std::int64_t b,
                                                  std::int64_t d);

// Ceiling(a * b / d) evaluated with a 128-bit intermediate product.
// Returns nullopt when d <= 0 or the quotient is not representable.
TZM_API std::optional<std::int64_t> mul_div_ceil(std::int64_t a, std::int64_t b, std::int64_t d);

// True when the value is inside the physically plausible range for a
// temperature. Plausibility is a request-validity property, not a safety one.
TZM_API bool is_plausible(MilliCelsius value);

// Formats a milli-value with exactly three fractional digits, e.g. "-1250"
// renders as "-1.250". Used for human-readable output only.
TZM_API std::string format_milli(std::int64_t value);

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_UNITS_HPP
