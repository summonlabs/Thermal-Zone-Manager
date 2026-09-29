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

#ifndef THERMAL_ZONE_MANAGER_DERATING_HPP
#define THERMAL_ZONE_MANAGER_DERATING_HPP

#include <cstdint>
#include <optional>

#include "thermal_zone_manager/envelope.hpp"
#include "thermal_zone_manager/export.hpp"
#include "thermal_zone_manager/ids.hpp"
#include "thermal_zone_manager/units.hpp"

namespace thermal_zone_manager {

// Derating is a quantised ladder over the ramp [derate_onset, ceiling].
//
//   span  = ceiling_temp - derate_onset          (strictly positive)
//   level(T) = 0                                  for T <= derate_onset
//            = ceil((T - derate_onset) * steps / span)   otherwise, clamped
//            = steps                             for T >= ceiling
//
// The level is monotone non-decreasing in temperature, and the fraction is
// monotone non-decreasing in the level:
//
//   fraction(level) = ceil(level * 1000000 / steps)   parts per million
//
// Both are computed with exact integer arithmetic. The ceiling is used for the
// division, so a zone at or above its declared ceiling is always fully derated.

// The instantaneous level for a temperature. Never exceeds steps.
TZM_API std::uint32_t instant_derate_level(const TemperatureEnvelope& envelope,
                                           std::uint32_t steps, MilliCelsius temperature);

// The derating fraction of a level, in parts per million. Zero for level 0 and
// 1000000 for level == steps.
TZM_API PartsPerMillion derate_fraction(std::uint32_t level, std::uint32_t steps);

// The highest temperature that still maps to a level at or below level - 1.
// Recovery from level L is only considered at or below
// derate_release_threshold(L) - recovery_margin.
TZM_API MilliCelsius derate_level_floor(const TemperatureEnvelope& envelope, std::uint32_t steps,
                                        std::uint32_t level);

// The temperature at or above which a temperature maps to level L, for
// L >= 1. This is derate_level_floor(L) + 1.
TZM_API MilliCelsius derate_level_onset(const TemperatureEnvelope& envelope, std::uint32_t steps,
                                        std::uint32_t level);

// Durable, per-zone derating state.
//
// published_level only ever rises immediately and falls one step at a time,
// and only after release_hold_observations consecutive usable observations at
// or below the release threshold. That is what makes the published level
// non-flapping: the number of level changes over a sequence of N observations
// is at most the number of escalations plus N / release_hold_observations.
struct DeratingState {
  std::uint32_t published_level = 0;
  std::uint32_t published_steps = 0;
  std::uint32_t hold_count = 0;

  bool operator==(const DeratingState& other) const {
    return published_level == other.published_level && published_steps == other.published_steps &&
           hold_count == other.hold_count;
  }
};

// The observable outcome of one derating update.
struct DeratingOutcome {
  DeratingState state;
  std::uint32_t instant_level = 0;
  PartsPerMillion fraction{};
  bool escalated = false;
  bool released = false;
  // True when the update was skipped because no usable temperature was
  // available. The published level is unchanged and hold_count is reset, so a
  // stale reading can never release a step.
  bool held_for_evidence = false;
  // True when the published level had to be clamped down because the ladder
  // resolution changed underneath it.
  bool clamped_to_ladder = false;
};

// Advances the derating state by one observation.
//
// usable_temperature is the effective (coupling-adjusted) temperature when the
// evidence was usable, and nullopt otherwise. Passing nullopt never changes
// published_level.
TZM_API DeratingOutcome advance_derating(const DeratingState& current,
                                         const TemperatureEnvelope& envelope, std::uint32_t steps,
                                         MilliCelsius recovery_margin,
                                         std::uint32_t release_hold_observations,
                                         std::optional<MilliCelsius> usable_temperature);

// Re-clamps a state to a ladder resolution without consuming an observation.
TZM_API DeratingState clamp_derating(const DeratingState& current, std::uint32_t steps);

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_DERATING_HPP
