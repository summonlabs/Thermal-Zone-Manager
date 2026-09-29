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

#ifndef THERMAL_ZONE_MANAGER_EVALUATION_HPP
#define THERMAL_ZONE_MANAGER_EVALUATION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "thermal_zone_manager/constraints.hpp"
#include "thermal_zone_manager/derating.hpp"
#include "thermal_zone_manager/evidence.hpp"
#include "thermal_zone_manager/export.hpp"
#include "thermal_zone_manager/headroom.hpp"
#include "thermal_zone_manager/ids.hpp"
#include "thermal_zone_manager/units.hpp"

namespace thermal_zone_manager {

class ThermalZoneEngine;

// Where the heat figure used for coupling came from.
enum class HeatSource : std::uint8_t {
  None = 0,      // no figure at all: the contribution to neighbours is unknown
  Observed = 1,  // a concurrent heat observation, used in preference to the declaration
  Declared = 2,  // the zone's declared heat load
};

TZM_API std::string_view to_string(HeatSource source);

// One zone's complete, generation-stamped thermal state for one evaluation.
struct ZoneThermalState {
  ZoneId zone;
  ZoneName name;
  ZoneGeneration zone_generation;

  // -- observed temperature evidence ---------------------------------------
  bool observation_present = false;
  EvidenceFreshness evidence_freshness = EvidenceFreshness::Missing;
  bool observed_temperature_known = false;
  MilliCelsius observed_temperature{};
  Timestamp observed_at{};
  SourceId source;
  ProvenanceKind provenance = ProvenanceKind::ExternalSensor;

  // -- heat used for coupling ----------------------------------------------
  bool heat_known = false;
  HeatSource heat_source = HeatSource::None;
  MilliWatts heat{};

  // -- coupling -------------------------------------------------------------
  // True when every neighbour contribution was resolvable. False when at least
  // one neighbour with a non-zero coefficient has an unknown heat load.
  bool coupled_heat_known = false;
  MilliWatts coupled_heat{};
  MilliCelsius coupled_rise{};
  std::uint32_t coupling_hops_used = 0;
  std::size_t unknown_neighbour_count = 0;
  // True when a multi-hop refinement was requested but refused because the
  // graph is not contractive. The first-order answer was used instead.
  bool coupling_refinement_refused = false;

  // -- effective temperature and band --------------------------------------
  bool effective_temperature_known = false;
  MilliCelsius effective_temperature{};
  bool band_known = false;
  ThermalBand band = ThermalBand::Nominal;

  // -- headroom -------------------------------------------------------------
  ThermalHeadroom headroom;

  // -- derating -------------------------------------------------------------
  std::uint32_t instant_derate_level = 0;
  DeratingState derating_before;
  DeratingState derating_after;
  PartsPerMillion derate_fraction{};
  bool derating_escalated = false;
  bool derating_released = false;
  bool derating_held_for_evidence = false;

  // -- emitted allowance ----------------------------------------------------
  bool allowance_known = false;
  MilliWatts placement_allowance{};

  PlacementConstraintKind constraint_kind = PlacementConstraintKind::Indeterminate;
  std::vector<ConstraintReason> reasons;
};

// Aggregate counts over one evaluation.
struct EvaluationSummary {
  std::size_t zones_total = 0;
  std::size_t zones_known = 0;
  std::size_t zones_unknown = 0;
  std::size_t zones_indeterminate = 0;
  std::size_t constraints_bounded = 0;
  std::size_t constraints_prohibited = 0;
  std::size_t constraints_indeterminate = 0;
  std::size_t zones_with_usable_evidence = 0;
  std::size_t zones_with_unknown_neighbour = 0;
};

// The immutable answer to one evaluation request.
//
// Zones and constraints are aligned and both are in ascending ZoneId order, so
// the result is a total order independent of any container iteration order.
// Evaluating the same state at the same instant twice produces byte-identical
// results, which digest() exposes as a 64-bit value.
class EvaluationResult {
 public:
  EvaluationId id() const noexcept { return id_; }
  Timestamp evaluated_at() const noexcept { return evaluated_at_; }
  ControlPlaneEpoch epoch() const noexcept { return epoch_; }
  ConfigurationGeneration configuration_generation() const noexcept {
    return configuration_generation_;
  }
  EvidenceGeneration evidence_generation() const noexcept { return evidence_generation_; }
  PolicyGeneration policy_generation() const noexcept { return policy_generation_; }
  StoreRevision basis_revision() const noexcept { return basis_revision_; }

  const std::vector<ZoneThermalState>& zones() const noexcept { return zones_; }
  const std::vector<PlacementConstraint>& constraints() const noexcept { return constraints_; }
  const EvaluationSummary& summary() const noexcept { return summary_; }

  // Null when the zone is not part of this evaluation. Never a substitute.
  const ZoneThermalState* find(ZoneId zone) const noexcept;
  const PlacementConstraint* constraint_for(ZoneId zone) const noexcept;

  std::size_t size() const noexcept { return zones_.size(); }

  // A deterministic 64-bit digest over the canonical encoding of the whole
  // result, excluding nothing.
  std::uint64_t digest() const noexcept { return digest_; }

 private:
  friend class ThermalZoneEngine;

  EvaluationId id_;
  Timestamp evaluated_at_{};
  ControlPlaneEpoch epoch_;
  ConfigurationGeneration configuration_generation_;
  EvidenceGeneration evidence_generation_;
  PolicyGeneration policy_generation_;
  StoreRevision basis_revision_;
  std::vector<ZoneThermalState> zones_;
  std::vector<PlacementConstraint> constraints_;
  EvaluationSummary summary_;
  std::uint64_t digest_ = 0;

  void seal();
  // Indices are ascending; binary search over the aligned vectors.
  std::optional<std::size_t> index_of(ZoneId zone) const noexcept;
};

// One evaluation request. The evaluation instant is taken from the engine's
// clock, never from the caller, so a caller cannot manufacture freshness.
struct EvaluationRequest {
  EvaluationId id;
  ControlPlaneEpoch epoch;
  // The declarations this request was planned against. Both are checked
  // against the live state before anything is evaluated.
  ConfigurationGeneration configuration_generation;
  EvidenceGeneration evidence_generation;
  // Optional: when present, the store revision the caller believes is current.
  std::optional<StoreRevision> basis_revision;
  // Optional subset in ascending order; empty means every declared zone.
  std::vector<ZoneId> zones;
};

// A multi-line human-readable rendering of one evaluation result.
TZM_API std::string render(const EvaluationResult& result);

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_EVALUATION_HPP
