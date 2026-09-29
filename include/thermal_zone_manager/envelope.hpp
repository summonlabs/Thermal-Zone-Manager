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

#ifndef THERMAL_ZONE_MANAGER_ENVELOPE_HPP
#define THERMAL_ZONE_MANAGER_ENVELOPE_HPP

#include <cstdint>
#include <string_view>

#include "thermal_zone_manager/errors.hpp"
#include "thermal_zone_manager/export.hpp"
#include "thermal_zone_manager/units.hpp"

namespace thermal_zone_manager {

// Classification of a temperature against a declared envelope. The enumerators
// are ordered by increasing temperature, so classify() is monotone
// non-decreasing in temperature: a warmer zone never reports a cooler band.
enum class ThermalBand : std::uint8_t {
  BelowFloor = 0,  // colder than the declared lower limit
  Nominal = 1,     // inside the envelope, below the derating onset
  Derating = 2,    // between the derating onset and the ceiling
  AtLimit = 3,     // at or above the ceiling, below the critical threshold
  Critical = 4,    // at or above the critical threshold
};

TZM_API std::string_view to_string(ThermalBand band);

// A declared temperature envelope. This is configuration: it is supplied by an
// operator or an upstream planner and is never derived from observations.
//
// The required ordering is
//
//   floor_temp < derate_onset < ceiling_temp <= critical_temp
//
// and every bound must be physically plausible. The ordering is strict between
// the floor, the derating onset and the ceiling so that the derating ramp is
// never empty and the ladder has a positive span to divide.
struct TemperatureEnvelope {
  MilliCelsius floor_temp{};
  MilliCelsius derate_onset{};
  MilliCelsius ceiling_temp{};
  MilliCelsius critical_temp{};

  // Returns InvalidEnvelope or ContradictoryLimits for a refused ordering and
  // InvalidTemperature for an implausible bound.
  Status validate() const;

  // The width of the derating ramp in millidegrees. Positive whenever
  // validate() succeeds.
  std::int64_t derate_span() const { return ceiling_temp.value - derate_onset.value; }

  bool operator==(const TemperatureEnvelope& other) const {
    return floor_temp == other.floor_temp && derate_onset == other.derate_onset &&
           ceiling_temp == other.ceiling_temp && critical_temp == other.critical_temp;
  }
};

// The band a temperature falls into. Total and deterministic: every
// temperature maps to exactly one band.
TZM_API ThermalBand classify(const TemperatureEnvelope& envelope, MilliCelsius temperature);

// True when the temperature is inside the closed interval
// [floor_temp, ceiling_temp].
TZM_API bool is_within_envelope(const TemperatureEnvelope& envelope, MilliCelsius temperature);

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_ENVELOPE_HPP
