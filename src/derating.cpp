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

#include "thermal_zone_manager/derating.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

#include "thermal_zone_manager/limits.hpp"

namespace thermal_zone_manager {

std::uint32_t instant_derate_level(const TemperatureEnvelope& envelope, std::uint32_t steps,
                                   MilliCelsius temperature) {
  if (steps == 0) {
    return 0;
  }
  if (temperature <= envelope.derate_onset) {
    return 0;
  }
  if (temperature >= envelope.ceiling_temp) {
    return steps;
  }
  const std::int64_t delta = temperature.value - envelope.derate_onset.value;
  const std::optional<std::int64_t> level =
      mul_div_ceil(delta, static_cast<std::int64_t>(steps), envelope.derate_span());
  if (!level.has_value() || level.value() < 0) {
    // Unreachable for a validated envelope; a saturated maximum is the
    // conservative answer if it ever were.
    return steps;
  }
  const auto clamped = static_cast<std::uint32_t>(level.value());
  return std::min(clamped, steps);
}

PartsPerMillion derate_fraction(std::uint32_t level, std::uint32_t steps) {
  if (steps == 0 || level == 0) {
    return PartsPerMillion{0};
  }
  if (level >= steps) {
    return PartsPerMillion{kPpmScale};
  }
  const std::optional<std::int64_t> fraction =
      mul_div_ceil(static_cast<std::int64_t>(level), static_cast<std::int64_t>(kPpmScale),
                   static_cast<std::int64_t>(steps));
  if (!fraction.has_value()) {
    return PartsPerMillion{kPpmScale};
  }
  return PartsPerMillion{static_cast<std::int32_t>(fraction.value())};
}

MilliCelsius derate_level_floor(const TemperatureEnvelope& envelope, std::uint32_t steps,
                                std::uint32_t level) {
  if (level == 0 || steps == 0) {
    return envelope.derate_onset;
  }
  const std::uint32_t bounded = std::min(level, steps);
  const std::int64_t offset = static_cast<std::int64_t>(bounded) - 1;
  const std::optional<std::int64_t> climb =
      mul_div_floor(offset, envelope.derate_span(), static_cast<std::int64_t>(steps));
  if (!climb.has_value()) {
    return envelope.derate_onset;
  }
  return MilliCelsius{envelope.derate_onset.value + climb.value()};
}

MilliCelsius derate_level_onset(const TemperatureEnvelope& envelope, std::uint32_t steps,
                                std::uint32_t level) {
  if (level == 0 || steps == 0) {
    return envelope.derate_onset;
  }
  const MilliCelsius floor_value = derate_level_floor(envelope, steps, level);
  return MilliCelsius{floor_value.value + 1};
}

DeratingState clamp_derating(const DeratingState& current, std::uint32_t steps) {
  DeratingState result;
  result.published_steps = steps;
  result.published_level = std::min(current.published_level, steps);
  result.hold_count = current.published_steps == steps ? current.hold_count : 0;
  if (result.hold_count > kMaxReleaseHoldObservations) {
    result.hold_count = kMaxReleaseHoldObservations;
  }
  return result;
}

DeratingOutcome advance_derating(const DeratingState& current,
                                 const TemperatureEnvelope& envelope, std::uint32_t steps,
                                 MilliCelsius recovery_margin,
                                 std::uint32_t release_hold_observations,
                                 std::optional<MilliCelsius> usable_temperature) {
  DeratingOutcome outcome;
  DeratingState state = clamp_derating(current, steps);
  outcome.clamped_to_ladder = state.published_level != current.published_level ||
                              state.published_steps != current.published_steps;

  if (!usable_temperature.has_value()) {
    // No usable temperature: hold the published level and clear the recovery
    // progress. A stale reading can never release a step.
    state.hold_count = 0;
    outcome.state = state;
    outcome.instant_level = state.published_level;
    outcome.fraction = derate_fraction(state.published_level, steps);
    outcome.held_for_evidence = true;
    return outcome;
  }

  const std::uint32_t instant = instant_derate_level(envelope, steps, *usable_temperature);
  outcome.instant_level = instant;

  if (instant > state.published_level) {
    state.published_level = instant;
    state.hold_count = 0;
    outcome.escalated = true;
  } else if (instant < state.published_level) {
    const MilliCelsius threshold = derate_level_floor(envelope, steps, state.published_level);
    const bool below = usable_temperature->value <= threshold.value - recovery_margin.value;
    if (below) {
      if (state.hold_count < release_hold_observations) {
        ++state.hold_count;
      }
      if (state.hold_count >= release_hold_observations) {
        state.published_level -= 1U;
        state.hold_count = 0;
        outcome.released = true;
      }
    } else {
      state.hold_count = 0;
    }
  } else {
    state.hold_count = 0;
  }

  state.published_steps = steps;
  outcome.state = state;
  outcome.fraction = derate_fraction(state.published_level, steps);
  return outcome;
}

}  // namespace thermal_zone_manager
