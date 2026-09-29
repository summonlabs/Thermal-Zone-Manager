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

#ifndef THERMAL_ZONE_MANAGER_POLICY_HPP
#define THERMAL_ZONE_MANAGER_POLICY_HPP

#include <cstdint>

#include "thermal_zone_manager/errors.hpp"
#include "thermal_zone_manager/export.hpp"
#include "thermal_zone_manager/ids.hpp"
#include "thermal_zone_manager/units.hpp"

namespace thermal_zone_manager {

// Declared evaluation policy. Policy is configuration: it is versioned by its
// own generation so a decision can be tied to the policy that produced it.
struct ThermalPolicy {
  PolicyGeneration generation;

  // Number of coupling passes evaluated. The value is fixed before evaluation
  // begins, so propagation terminates after exactly this many passes.
  //
  // One pass is the closed-form first-order answer and is always available.
  // More than one pass refines the series and additionally requires a
  // contractive graph; a non-contractive graph with hops > 1 is refused with
  // CouplingNotContractive rather than evaluated approximately.
  std::uint32_t coupling_hops = 1;

  // Consecutive usable observations at or below the release threshold that are
  // required before the published derating level is allowed to fall by one
  // step.
  std::uint32_t release_hold_observations = 3;

  // Tolerance applied when an observation claims a timestamp later than the
  // evaluation instant. Within the tolerance the observation is still usable;
  // beyond it the observation is Future and is not usable.
  Nanoseconds future_tolerance{0};

  Status validate() const;
};

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_POLICY_HPP
