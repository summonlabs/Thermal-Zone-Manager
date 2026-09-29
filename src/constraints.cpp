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

#include "thermal_zone_manager/constraints.hpp"

#include <string>
#include <string_view>

#include "thermal_zone_manager/units.hpp"

namespace thermal_zone_manager {

std::string_view to_string(PlacementConstraintKind kind) {
  switch (kind) {
    case PlacementConstraintKind::Bounded:
      return "bounded";
    case PlacementConstraintKind::Prohibited:
      return "prohibited";
    case PlacementConstraintKind::Indeterminate:
      return "indeterminate";
  }
  return "unknown_constraint_kind";
}

std::string_view to_string(ConstraintReason reason) {
  switch (reason) {
    case ConstraintReason::EvidenceFresh:
      return "evidence_fresh";
    case ConstraintReason::EvidenceMissing:
      return "evidence_missing";
    case ConstraintReason::EvidenceStale:
      return "evidence_stale";
    case ConstraintReason::EvidenceFuture:
      return "evidence_future";
    case ConstraintReason::EvidenceRecovered:
      return "evidence_recovered";
    case ConstraintReason::EvidenceSuperseded:
      return "evidence_superseded";
    case ConstraintReason::NeighbourHeatUnknown:
      return "neighbour_heat_unknown";
    case ConstraintReason::CouplingNotContractive:
      return "coupling_not_contractive";
    case ConstraintReason::CouplingApplied:
      return "coupling_applied";
    case ConstraintReason::BelowFloor:
      return "below_floor";
    case ConstraintReason::DeratingActive:
      return "derating_active";
    case ConstraintReason::DeratingFull:
      return "derating_full";
    case ConstraintReason::AtCeiling:
      return "at_ceiling";
    case ConstraintReason::AboveCeiling:
      return "above_ceiling";
    case ConstraintReason::AtCritical:
      return "at_critical";
    case ConstraintReason::AllowanceReducedByDerating:
      return "allowance_reduced_by_derating";
    case ConstraintReason::AllowanceExhausted:
      return "allowance_exhausted";
  }
  return "unknown_constraint_reason";
}

std::string PlacementConstraint::explanation() const {
  std::string text = "zone ";
  text += to_string(zone);
  text += ": ";
  switch (kind) {
    case PlacementConstraintKind::Bounded:
      text += "may accept up to ";
      text += format_milli(max_additional_heat.value);
      text += " W of additional heat";
      break;
    case PlacementConstraintKind::Prohibited:
      text += "must not accept additional heat";
      break;
    case PlacementConstraintKind::Indeterminate:
      text += "has no proven heat allowance";
      break;
  }
  text += " (band ";
  text += band_known ? std::string(to_string(band)) : std::string("unknown");
  text += ", derating ";
  text += std::to_string(static_cast<long long>(derate_fraction.value));
  text += " ppm)";
  if (!reasons.empty()) {
    text += " [";
    for (std::size_t index = 0; index < reasons.size(); ++index) {
      if (index != 0) {
        text += ", ";
      }
      text += to_string(reasons[index]);
    }
    text += "]";
  }
  return text;
}

}  // namespace thermal_zone_manager
