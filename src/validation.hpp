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

#ifndef THERMAL_ZONE_MANAGER_SRC_VALIDATION_HPP
#define THERMAL_ZONE_MANAGER_SRC_VALIDATION_HPP

#include <vector>

#include "thermal_zone_manager/coupling.hpp"
#include "thermal_zone_manager/errors.hpp"
#include "thermal_zone_manager/policy.hpp"
#include "thermal_zone_manager/zone.hpp"

// Validation is split into the phases of the public precedence table so that a
// composite request is checked in the documented order no matter which part of
// it is malformed. A caller that violates rules in two phases always sees the
// code of the earlier phase.

namespace thermal_zone_manager {
namespace detail {

// Phase 2: structural validity.
Status validate_zone_structural(const ZoneConfiguration& zone);

// Phase 5: numeric domain of one zone.
Status validate_zone_numeric(const ZoneConfiguration& zone);

// Phase 6: identity of one zone, ignoring duplicates.
Status validate_zone_identity(const ZoneConfiguration& zone);

// Phase 5: numeric domain of a coupling edge, coefficient range only.
Status validate_coupling_numeric(const std::vector<CouplingEdge>& edges);

}  // namespace detail
}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_SRC_VALIDATION_HPP
