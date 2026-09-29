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

#include "thermal_zone_manager/configuration.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "serialize.hpp"
#include "thermal_zone_manager/limits.hpp"
#include "validation.hpp"

namespace thermal_zone_manager {

// The empty world: nothing has been declared yet. Its generation, evidence
// generation and policy generation are all zero, which is exactly what a
// caller of create() may never supply. The durable decoder relies on that
// distinction to tell "no world declared" apart from "a declared world with no
// zones", the latter being invalid.
Configuration::Configuration() { compute_digest(); }

Result<Configuration> Configuration::create(ZoneList zones, CouplingGraph coupling,
                                            ThermalPolicy policy,
                                            ConfigurationGeneration generation,
                                            EvidenceGeneration evidence_generation) {
  // -- phase 2: structural validity and resource bounds ---------------------
  if (zones.empty()) {
    return Error(ErrorCode::EmptyConfiguration,
                 "a configuration must declare at least one zone");
  }
  if (!is_within_zone_limit(zones.size())) {
    return Error(ErrorCode::TooManyZones, "the configuration exceeds its zone ceiling")
        .with("zones", static_cast<std::uint64_t>(zones.size()))
        .with("limit", static_cast<std::uint64_t>(kMaxZones));
  }
  if (generation.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "a configuration generation may not be zero");
  }
  if (evidence_generation.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "an evidence generation may not be zero");
  }
  for (const ZoneConfiguration& zone : zones) {
    const Status structural = detail::validate_zone_structural(zone);
    if (!structural.ok()) {
      return structural.error();
    }
  }

  // -- phase 5: numeric domain ---------------------------------------------
  for (const ZoneConfiguration& zone : zones) {
    const Status numeric = detail::validate_zone_numeric(zone);
    if (!numeric.ok()) {
      return numeric.error();
    }
  }
  const Status policy_status = policy.validate();
  if (!policy_status.ok()) {
    return policy_status.error();
  }
  const CouplingGraph::EdgeList edges = coupling.edges();
  const Status coupling_numeric = detail::validate_coupling_numeric(edges);
  if (!coupling_numeric.ok()) {
    return coupling_numeric.error();
  }

  // -- phase 6: identity ----------------------------------------------------
  for (const ZoneConfiguration& zone : zones) {
    const Status identity = detail::validate_zone_identity(zone);
    if (!identity.ok()) {
      return identity.error();
    }
  }
  std::sort(zones.begin(), zones.end(), zone_less);
  for (std::size_t index = 1; index < zones.size(); ++index) {
    if (zones[index - 1].id == zones[index].id) {
      return Error(ErrorCode::DuplicateZoneId, "a zone handle is declared more than once")
          .with("zone", zones[index].id.raw());
    }
    if (zones[index - 1].name == zones[index].name) {
      return Error(ErrorCode::DuplicateZoneName, "a zone name is declared more than once")
          .with("name", zones[index].name.str());
    }
  }

  // -- phase 7: coupling graph ---------------------------------------------
  std::vector<ZoneId> ids;
  ids.reserve(zones.size());
  for (const ZoneConfiguration& zone : zones) {
    ids.push_back(zone.id);
  }
  Result<CouplingGraph> verified = CouplingGraph::create(edges, ids);
  if (!verified.ok()) {
    return verified.error();
  }

  Configuration result;
  result.zones_ = std::move(zones);
  result.coupling_ = std::move(verified.value());
  result.policy_ = policy;
  result.generation_ = generation;
  result.evidence_generation_ = evidence_generation;
  result.compute_digest();
  return result;
}

const ZoneConfiguration* Configuration::find(ZoneId id) const noexcept {
  const auto found = std::lower_bound(
      zones_.begin(), zones_.end(), id,
      [](const ZoneConfiguration& zone, ZoneId value) { return zone.id < value; });
  if (found == zones_.end() || !(found->id == id)) {
    return nullptr;
  }
  return &(*found);
}

std::vector<ZoneId> Configuration::zone_ids() const {
  std::vector<ZoneId> ids;
  ids.reserve(zones_.size());
  for (const ZoneConfiguration& zone : zones_) {
    ids.push_back(zone.id);
  }
  return ids;
}

void Configuration::compute_digest() { digest_ = detail::configuration_digest(*this); }

bool Configuration::operator==(const Configuration& other) const {
  if (generation_ != other.generation_ || evidence_generation_ != other.evidence_generation_) {
    return false;
  }
  if (!(coupling_ == other.coupling_)) {
    return false;
  }
  if (policy_.generation != other.policy_.generation ||
      policy_.coupling_hops != other.policy_.coupling_hops ||
      policy_.release_hold_observations != other.policy_.release_hold_observations ||
      policy_.future_tolerance != other.policy_.future_tolerance) {
    return false;
  }
  if (zones_.size() != other.zones_.size()) {
    return false;
  }
  for (std::size_t index = 0; index < zones_.size(); ++index) {
    const ZoneConfiguration& a = zones_[index];
    const ZoneConfiguration& b = other.zones_[index];
    if (!(a.id == b.id) || !(a.name == b.name) || !(a.generation == b.generation) ||
        !(a.envelope == b.envelope) || a.declared_heat != b.declared_heat ||
        !(a.thermal_resistance == b.thermal_resistance) || a.derate_steps != b.derate_steps ||
        !(a.recovery_margin == b.recovery_margin)) {
      return false;
    }
  }
  return true;
}

}  // namespace thermal_zone_manager
