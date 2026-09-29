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

#ifndef THERMAL_ZONE_MANAGER_CONSTRAINTS_HPP
#define THERMAL_ZONE_MANAGER_CONSTRAINTS_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "thermal_zone_manager/envelope.hpp"
#include "thermal_zone_manager/export.hpp"
#include "thermal_zone_manager/ids.hpp"
#include "thermal_zone_manager/units.hpp"

// Placement constraints are an OUTPUT of this runtime. A constraint states how
// much additional heat a zone can accept, or that no proven amount exists. It
// never selects a location, never reserves capacity, never ranks candidates and
// never actuates anything. Placement authority belongs to the facility
// placement planner; this record is bounded evidence for it.

namespace thermal_zone_manager {

enum class PlacementConstraintKind : std::uint8_t {
  // A proven, bounded allowance. max_additional_heat is meaningful.
  Bounded = 0,
  // A proven zero: the zone must not receive more heat. This is a known zero.
  Prohibited = 1,
  // No proven allowance exists. max_additional_heat is not a value: it is not
  // zero, and it must not be read as capacity. A consumer that requires
  // proven capacity must treat this exactly like Prohibited, and a consumer
  // that reports state must not report it as an allowance.
  Indeterminate = 2,
};

TZM_API std::string_view to_string(PlacementConstraintKind kind);

// Why a constraint reads the way it does. Reasons are unique and always
// emitted in ascending enumerator order, so the reason list is a total order.
enum class ConstraintReason : std::uint8_t {
  EvidenceFresh = 0,
  EvidenceMissing = 1,
  EvidenceStale = 2,
  EvidenceFuture = 3,
  EvidenceRecovered = 4,
  EvidenceSuperseded = 5,
  NeighbourHeatUnknown = 6,
  CouplingNotContractive = 7,
  CouplingApplied = 8,
  BelowFloor = 9,
  DeratingActive = 10,
  DeratingFull = 11,
  AtCeiling = 12,
  AboveCeiling = 13,
  AtCritical = 14,
  AllowanceReducedByDerating = 15,
  AllowanceExhausted = 16,
};

TZM_API std::string_view to_string(ConstraintReason reason);

// One zone's emitted placement constraint.
struct PlacementConstraint {
  ZoneId zone;
  ZoneGeneration zone_generation;
  ConfigurationGeneration configuration_generation;
  EvidenceGeneration evidence_generation;
  PolicyGeneration policy_generation;
  EvaluationId evaluation;
  // The store revision the evaluation was computed against. A consumer that
  // acts on this record can tell whether it is still current.
  StoreRevision basis_revision;

  PlacementConstraintKind kind = PlacementConstraintKind::Indeterminate;
  // True only when max_additional_heat carries a proven value. False for
  // Indeterminate.
  bool allowance_known = false;
  MilliWatts max_additional_heat{};

  ThermalBand band = ThermalBand::Nominal;
  bool band_known = false;
  PartsPerMillion derate_fraction{};

  // Ascending, unique.
  std::vector<ConstraintReason> reasons;

  // A single deterministic sentence describing the constraint. Human-readable
  // only; the machine contract is kind, allowance_known and the codes.
  std::string explanation() const;
};

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_CONSTRAINTS_HPP
