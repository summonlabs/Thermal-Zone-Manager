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

#include "thermal_zone_manager/envelope.hpp"

#include <string_view>

namespace thermal_zone_manager {

std::string_view to_string(ThermalBand band) {
  switch (band) {
    case ThermalBand::BelowFloor:
      return "below_floor";
    case ThermalBand::Nominal:
      return "nominal";
    case ThermalBand::Derating:
      return "derating";
    case ThermalBand::AtLimit:
      return "at_limit";
    case ThermalBand::Critical:
      return "critical";
  }
  return "unknown_band";
}

Status TemperatureEnvelope::validate() const {
  // Plausibility is checked before ordering so that an absurd bound is
  // reported as an implausible temperature rather than as a broken ordering.
  const MilliCelsius bounds[4] = {floor_temp, derate_onset, ceiling_temp, critical_temp};
  const char* names[4] = {"floor_temp", "derate_onset", "ceiling_temp", "critical_temp"};
  for (std::size_t index = 0; index < 4; ++index) {
    if (!is_plausible(bounds[index])) {
      return Error(ErrorCode::InvalidTemperature, "an envelope bound is not physically plausible")
          .with("field", names[index])
          .with("millicelsius", bounds[index].value);
    }
  }
  if (floor_temp >= derate_onset) {
    return Error(ErrorCode::InvalidEnvelope,
                 "the floor must be strictly below the derating onset")
        .with("floor_temp", floor_temp.value)
        .with("derate_onset", derate_onset.value);
  }
  if (derate_onset >= ceiling_temp) {
    return Error(ErrorCode::InvalidEnvelope,
                 "the derating onset must be strictly below the ceiling")
        .with("derate_onset", derate_onset.value)
        .with("ceiling_temp", ceiling_temp.value);
  }
  if (ceiling_temp > critical_temp) {
    return Error(ErrorCode::ContradictoryLimits,
                 "the critical threshold may not be below the ceiling")
        .with("ceiling_temp", ceiling_temp.value)
        .with("critical_temp", critical_temp.value);
  }
  return Status();
}

ThermalBand classify(const TemperatureEnvelope& envelope, MilliCelsius temperature) {
  if (temperature < envelope.floor_temp) {
    return ThermalBand::BelowFloor;
  }
  if (temperature < envelope.derate_onset) {
    return ThermalBand::Nominal;
  }
  if (temperature < envelope.ceiling_temp) {
    return ThermalBand::Derating;
  }
  if (temperature < envelope.critical_temp) {
    return ThermalBand::AtLimit;
  }
  return ThermalBand::Critical;
}

bool is_within_envelope(const TemperatureEnvelope& envelope, MilliCelsius temperature) {
  return temperature >= envelope.floor_temp && temperature <= envelope.ceiling_temp;
}

}  // namespace thermal_zone_manager
