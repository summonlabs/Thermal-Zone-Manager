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

#include "thermal_zone_manager/evaluation.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "serialize.hpp"
#include "thermal_zone_manager/units.hpp"

namespace thermal_zone_manager {

std::string_view to_string(HeatSource source) {
  switch (source) {
    case HeatSource::None:
      return "none";
    case HeatSource::Observed:
      return "observed";
    case HeatSource::Declared:
      return "declared";
  }
  return "unknown_heat_source";
}

std::optional<std::size_t> EvaluationResult::index_of(ZoneId zone) const noexcept {
  const auto found = std::lower_bound(
      zones_.begin(), zones_.end(), zone,
      [](const ZoneThermalState& state, ZoneId value) { return state.zone < value; });
  if (found == zones_.end() || !(found->zone == zone)) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(std::distance(zones_.begin(), found));
}

const ZoneThermalState* EvaluationResult::find(ZoneId zone) const noexcept {
  const std::optional<std::size_t> index = index_of(zone);
  if (!index.has_value()) {
    return nullptr;
  }
  return &zones_[*index];
}

const PlacementConstraint* EvaluationResult::constraint_for(ZoneId zone) const noexcept {
  const std::optional<std::size_t> index = index_of(zone);
  if (!index.has_value() || *index >= constraints_.size()) {
    return nullptr;
  }
  return &constraints_[*index];
}

void EvaluationResult::seal() {
  summary_ = EvaluationSummary{};
  summary_.zones_total = zones_.size();
  for (const ZoneThermalState& zone : zones_) {
    switch (zone.headroom.status) {
      case HeadroomStatus::Known:
        ++summary_.zones_known;
        break;
      case HeadroomStatus::Unknown:
        ++summary_.zones_unknown;
        break;
      case HeadroomStatus::Indeterminate:
        ++summary_.zones_indeterminate;
        break;
    }
    if (is_usable(zone.evidence_freshness)) {
      ++summary_.zones_with_usable_evidence;
    }
    if (zone.unknown_neighbour_count > 0) {
      ++summary_.zones_with_unknown_neighbour;
    }
    switch (zone.constraint_kind) {
      case PlacementConstraintKind::Bounded:
        ++summary_.constraints_bounded;
        break;
      case PlacementConstraintKind::Prohibited:
        ++summary_.constraints_prohibited;
        break;
      case PlacementConstraintKind::Indeterminate:
        ++summary_.constraints_indeterminate;
        break;
    }
  }
  digest_ = detail::evaluation_digest(*this);
}

namespace {

const char* yes_no(bool value) { return value ? "yes" : "no"; }

}  // namespace

std::string render(const EvaluationResult& result) {
  std::string text;
  text += "evaluation " + to_string(result.id()) + " at " +
          std::to_string(result.evaluated_at().value) + " ns\n";
  text += "  epoch " + to_string(result.epoch()) + ", configuration generation " +
          to_string(result.configuration_generation()) + ", evidence generation " +
          to_string(result.evidence_generation()) + ", policy generation " +
          to_string(result.policy_generation()) + ", store revision " +
          to_string(result.basis_revision()) + "\n";
  for (const ZoneThermalState& zone : result.zones()) {
    text += "  zone " + to_string(zone.zone) + " (" + zone.name.str() + ")\n";
    text += "    evidence      : ";
    text += std::string(to_string(zone.evidence_freshness));
    if (zone.observed_temperature_known) {
      text += "  observed " + format_milli(zone.observed_temperature.value) + " C";
    }
    text += "\n";
    text += "    heat          : ";
    if (zone.heat_known) {
      text += format_milli(zone.heat.value) + " W (" +
              std::string(to_string(zone.heat_source)) + ")";
    } else {
      text += "unknown";
    }
    text += "\n";
    text += "    coupling      : ";
    if (zone.coupled_heat_known) {
      text += format_milli(zone.coupled_heat.value) + " W arriving, rise " +
              format_milli(zone.coupled_rise.value) + " C over " +
              std::to_string(static_cast<unsigned long long>(zone.coupling_hops_used)) +
              " hop(s)";
    } else {
      text += "unresolved";
    }
    if (zone.unknown_neighbour_count > 0) {
      text += "  (" + std::to_string(static_cast<unsigned long long>(zone.unknown_neighbour_count)) +
              " neighbour(s) with unknown heat)";
    }
    if (zone.coupling_refinement_refused) {
      text += "  [refinement refused: not contractive]";
    }
    text += "\n";
    text += "    effective temp: ";
    if (zone.effective_temperature_known) {
      text += format_milli(zone.effective_temperature.value) + " C";
    } else {
      text += "unknown";
    }
    text += "  band ";
    text += zone.band_known ? std::string(to_string(zone.band)) : std::string("unknown");
    text += "\n";
    text += "    headroom      : ";
    text += std::string(to_string(zone.headroom.status));
    if (is_proven(zone.headroom.status)) {
      text += "  margin " + format_milli(zone.headroom.temperature_margin.value) + " C, " +
              format_milli(zone.headroom.power_margin.value) + " W";
    }
    text += "\n";
    text += "    derating      : level " +
            std::to_string(static_cast<unsigned long long>(zone.derating_after.published_level)) +
            "/" + std::to_string(static_cast<unsigned long long>(zone.derating_after.published_steps)) +
            ", instant " +
            std::to_string(static_cast<unsigned long long>(zone.instant_derate_level)) +
            ", fraction " + std::to_string(static_cast<long long>(zone.derate_fraction.value)) +
            " ppm, hold " +
            std::to_string(static_cast<unsigned long long>(zone.derating_after.hold_count)) +
            " (escalated " + yes_no(zone.derating_escalated) + ", released " +
            yes_no(zone.derating_released) + ", held " +
            yes_no(zone.derating_held_for_evidence) + ")\n";
    text += "    constraint    : ";
    text += std::string(to_string(zone.constraint_kind));
    if (zone.allowance_known) {
      text += "  allowance " + format_milli(zone.placement_allowance.value) + " W";
    } else {
      text += "  allowance not proven";
    }
    text += "\n";
    if (!zone.reasons.empty()) {
      text += "    reasons       :";
      for (const ConstraintReason reason : zone.reasons) {
        text += " ";
        text += to_string(reason);
      }
      text += "\n";
    }
  }
  const EvaluationSummary& summary = result.summary();
  text += "  summary: " + std::to_string(static_cast<unsigned long long>(summary.zones_total)) +
          " zone(s), " + std::to_string(static_cast<unsigned long long>(summary.zones_known)) +
          " known, " + std::to_string(static_cast<unsigned long long>(summary.zones_unknown)) +
          " unknown, " +
          std::to_string(static_cast<unsigned long long>(summary.zones_indeterminate)) +
          " indeterminate\n";
  return text;
}

}  // namespace thermal_zone_manager
