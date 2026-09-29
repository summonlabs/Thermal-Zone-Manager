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

#ifndef THERMAL_ZONE_MANAGER_HEADROOM_HPP
#define THERMAL_ZONE_MANAGER_HEADROOM_HPP

#include <cstdint>
#include <string_view>

#include "thermal_zone_manager/export.hpp"
#include "thermal_zone_manager/units.hpp"

namespace thermal_zone_manager {

// The three answers a headroom question can have.
//
// Known         every input was usable and the exact margin was computed.
// Unknown       a critical input was missing, stale, superseded or from the
//               future. There is no margin: not zero, not the last value.
// Indeterminate the inputs were usable but the model cannot resolve them, for
//               example a neighbour whose heat load is undeclared while a
//               non-zero coupling coefficient points at this zone.
//
// Unknown and Indeterminate both mean "no proven capacity". Neither may be
// consumed as zero, and neither may be consumed as safe.
enum class HeadroomStatus : std::uint8_t {
  Known = 0,
  Unknown = 1,
  Indeterminate = 2,
};

TZM_API std::string_view to_string(HeadroomStatus status);

// True only for Known. The single predicate a placement consumer should use
// before treating a margin as evidence of capacity.
TZM_API bool is_proven(HeadroomStatus status);

// The computed margin of a zone.
//
//   temperature_margin = ceiling_temp - effective_temperature
//   power_margin       = floor(temperature_margin * 1000 / thermal_resistance)
//
// A negative temperature margin means the zone is already above its declared
// ceiling; the power margin is then also negative and the placement allowance
// it feeds is clamped to a known zero. Margins are never clamped to zero,
// because "already over the envelope by 3 C" is information a caller needs.
struct ThermalHeadroom {
  HeadroomStatus status = HeadroomStatus::Unknown;
  MilliCelsius temperature_margin{};
  MilliWatts power_margin{};

  bool over_envelope() const noexcept { return status == HeadroomStatus::Known &&
                                               temperature_margin.value < 0; }
};

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_HEADROOM_HPP
