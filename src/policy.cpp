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

#include "thermal_zone_manager/policy.hpp"

#include "thermal_zone_manager/limits.hpp"

namespace thermal_zone_manager {

Status ThermalPolicy::validate() const {
  if (coupling_hops < kMinCouplingHops || coupling_hops > kMaxCouplingHops) {
    return Error(ErrorCode::InvalidPolicy, "the coupling hop count is outside its supported range")
        .with("coupling_hops", static_cast<std::uint64_t>(coupling_hops))
        .with("min", static_cast<std::uint64_t>(kMinCouplingHops))
        .with("max", static_cast<std::uint64_t>(kMaxCouplingHops));
  }
  if (release_hold_observations < kMinReleaseHoldObservations ||
      release_hold_observations > kMaxReleaseHoldObservations) {
    return Error(ErrorCode::InvalidPolicy,
                 "the hysteresis release hold is outside its supported range")
        .with("release_hold_observations",
              static_cast<std::uint64_t>(release_hold_observations))
        .with("min", static_cast<std::uint64_t>(kMinReleaseHoldObservations))
        .with("max", static_cast<std::uint64_t>(kMaxReleaseHoldObservations));
  }
  if (future_tolerance.value < 0 || future_tolerance.value > 60000000000LL) {
    return Error(ErrorCode::InvalidPolicy,
                 "the future timestamp tolerance is outside its supported range")
        .with("future_tolerance_ns", future_tolerance.value);
  }
  return Status();
}

}  // namespace thermal_zone_manager
