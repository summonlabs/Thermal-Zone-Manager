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

#include "thermal_zone_manager/zone.hpp"

#include <cstdint>

#include "validation.hpp"

namespace thermal_zone_manager {

namespace detail {

Status validate_zone_structural(const ZoneConfiguration& zone) {
  if (zone.name.empty()) {
    return Error(ErrorCode::InvalidArgument, "a zone must carry a name")
        .with("zone", zone.id.raw());
  }
  if (zone.generation.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "a zone must carry a non-zero generation")
        .with("zone", zone.id.raw());
  }
  if (zone.declared_heat.has_value()) {
    if (zone.declared_heat->value < 0 || zone.declared_heat->value > kMaxPlausibleMilliWatts) {
      return Error(ErrorCode::InvalidArgument, "a declared heat load is outside its range")
          .with("zone", zone.id.raw())
          .with("milliwatts", zone.declared_heat->value)
          .with("max", kMaxPlausibleMilliWatts);
    }
  }
  return Status();
}

Status validate_zone_numeric(const ZoneConfiguration& zone) {
  const Status envelope = zone.envelope.validate();
  if (!envelope.ok()) {
    Error enriched = envelope.error();
    enriched.with("zone", zone.id.raw());
    return enriched;
  }
  if (zone.thermal_resistance.value <= 0 ||
      zone.thermal_resistance.value > kMaxPlausibleResistance) {
    return Error(ErrorCode::InvalidThermalResistance,
                 "a thermal resistance must be positive and inside its range")
        .with("zone", zone.id.raw())
        .with("micro_kelvin_per_watt", zone.thermal_resistance.value)
        .with("max", kMaxPlausibleResistance);
  }
  if (zone.derate_steps < kMinDerateSteps || zone.derate_steps > kMaxDerateSteps) {
    return Error(ErrorCode::InvalidDerateLadder,
                 "the derating ladder resolution is outside its supported range")
        .with("zone", zone.id.raw())
        .with("derate_steps", static_cast<std::uint64_t>(zone.derate_steps))
        .with("min", static_cast<std::uint64_t>(kMinDerateSteps))
        .with("max", static_cast<std::uint64_t>(kMaxDerateSteps));
  }
  const std::int64_t span = zone.envelope.derate_span();
  if (zone.recovery_margin.value <= 0 || zone.recovery_margin.value >= span) {
    return Error(ErrorCode::InvalidDerateLadder,
                 "the hysteresis recovery margin must be positive and smaller than the "
                 "derating span")
        .with("zone", zone.id.raw())
        .with("recovery_margin", zone.recovery_margin.value)
        .with("derate_span", span);
  }
  return Status();
}

Status validate_zone_identity(const ZoneConfiguration& zone) {
  if (zone.id.is_zero()) {
    return Error(ErrorCode::UnknownZone, "zone handle zero is reserved and never valid");
  }
  return Status();
}

}  // namespace detail

Status ZoneConfiguration::validate() const {
  Status status = detail::validate_zone_structural(*this);
  if (!status.ok()) {
    return status;
  }
  status = detail::validate_zone_numeric(*this);
  if (!status.ok()) {
    return status;
  }
  return detail::validate_zone_identity(*this);
}

bool zone_less(const ZoneConfiguration& a, const ZoneConfiguration& b) { return a.id < b.id; }

}  // namespace thermal_zone_manager
