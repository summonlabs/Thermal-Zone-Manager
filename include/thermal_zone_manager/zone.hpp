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

#ifndef THERMAL_ZONE_MANAGER_ZONE_HPP
#define THERMAL_ZONE_MANAGER_ZONE_HPP

#include <cstdint>
#include <optional>

#include "thermal_zone_manager/envelope.hpp"
#include "thermal_zone_manager/errors.hpp"
#include "thermal_zone_manager/export.hpp"
#include "thermal_zone_manager/ids.hpp"
#include "thermal_zone_manager/limits.hpp"
#include "thermal_zone_manager/units.hpp"

namespace thermal_zone_manager {

// The declared configuration of one thermal zone. Everything here is supplied
// by an operator or an upstream planner; nothing here is derived from an
// observation.
struct ZoneConfiguration {
  ZoneId id;
  ZoneName name;
  ZoneGeneration generation;
  TemperatureEnvelope envelope;

  // Heat that the zone contributes when no concurrent heat observation exists.
  // Absent means "not declared", which is different from a declared zero: an
  // undeclared heat load makes the coupled contribution to a neighbour unknown
  // rather than zero.
  std::optional<MilliWatts> declared_heat;

  // Thermal resistance from the zone to its declared reference temperature.
  // Must be strictly positive.
  MicroKelvinPerWatt thermal_resistance;

  // Resolution of the derating ladder. The ramp [derate_onset, ceiling] is
  // divided into this many equal steps; a temperature is derated by the number
  // of steps it has climbed, so a larger value produces a finer grained
  // derating response.
  std::uint32_t derate_steps = 8;

  // Hysteresis release margin. A derating step may only be released once the
  // temperature has fallen this far below the step's own threshold, which is
  // what prevents the published level from flapping across a boundary.
  MilliCelsius recovery_margin{1000};  // 1.000 C, that is 1000 mC

  Status validate() const;
};

// Deterministic ordering of zone configurations: ascending ZoneId. The total
// order is used everywhere a zone collection becomes observable.
TZM_API bool zone_less(const ZoneConfiguration& a, const ZoneConfiguration& b);

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_ZONE_HPP
