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

#ifndef THERMAL_ZONE_MANAGER_CONFIGURATION_HPP
#define THERMAL_ZONE_MANAGER_CONFIGURATION_HPP

#include <cstdint>
#include <vector>

#include "thermal_zone_manager/coupling.hpp"
#include "thermal_zone_manager/errors.hpp"
#include "thermal_zone_manager/export.hpp"
#include "thermal_zone_manager/ids.hpp"
#include "thermal_zone_manager/policy.hpp"
#include "thermal_zone_manager/zone.hpp"

namespace thermal_zone_manager {

// The declared, generation-stamped world: zones, coupling and policy.
//
// A Configuration is immutable once built. Replacing the declared world
// produces a new Configuration with a bumped ConfigurationGeneration and a
// bumped EvidenceGeneration; observations stamped with the previous evidence
// generation are not re-interpreted under the new one, they are reported as
// superseded.
class Configuration {
 public:
  using ZoneList = std::vector<ZoneConfiguration>;

  // The empty world at generation zero, with an empty policy generation.
  Configuration();

  // Validates every zone, the coupling graph and the policy, then sorts the
  // zones into ascending ZoneId order. Duplicate handles or duplicate names
  // are refused.
  static Result<Configuration> create(ZoneList zones, CouplingGraph coupling, ThermalPolicy policy,
                                      ConfigurationGeneration generation,
                                      EvidenceGeneration evidence_generation);

  ConfigurationGeneration generation() const noexcept { return generation_; }
  EvidenceGeneration evidence_generation() const noexcept { return evidence_generation_; }
  const ThermalPolicy& policy() const noexcept { return policy_; }
  const CouplingGraph& coupling() const noexcept { return coupling_; }

  // Ascending ZoneId order.
  const ZoneList& zones() const noexcept { return zones_; }
  std::size_t zone_count() const noexcept { return zones_.size(); }
  bool empty() const noexcept { return zones_.empty(); }

  // The zone with this handle, or nullptr. Never returns a substituted zone.
  const ZoneConfiguration* find(ZoneId id) const noexcept;

  // Ascending zone handles.
  std::vector<ZoneId> zone_ids() const;

  // A deterministic 64-bit digest over the canonical encoding of the declared
  // world, generations included. Two configurations that differ in any
  // declared value or generation have different digests.
  std::uint64_t digest() const noexcept { return digest_; }

  bool operator==(const Configuration& other) const;
  bool operator!=(const Configuration& other) const { return !(*this == other); }

  void compute_digest();

 private:
  ZoneList zones_;
  CouplingGraph coupling_;
  ThermalPolicy policy_;
  ConfigurationGeneration generation_;
  EvidenceGeneration evidence_generation_;
  std::uint64_t digest_ = 0;
};

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_CONFIGURATION_HPP
