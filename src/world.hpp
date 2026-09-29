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

#ifndef THERMAL_ZONE_MANAGER_SRC_WORLD_HPP
#define THERMAL_ZONE_MANAGER_SRC_WORLD_HPP

#include <cstdint>
#include <vector>

#include "thermal_zone_manager/configuration.hpp"
#include "thermal_zone_manager/derating.hpp"
#include "thermal_zone_manager/evidence.hpp"
#include "thermal_zone_manager/ids.hpp"
#include "thermal_zone_manager/store.hpp"

namespace thermal_zone_manager {
namespace detail {

// The durable state of one zone: the observation that was accepted and the
// derating state that was published.
struct PersistedZoneState {
  ZoneId zone;
  ZoneGeneration generation;
  bool has_observation = false;
  TemperatureObservation observation;
  bool recovered = false;
  DeratingState derating;

  bool operator==(const PersistedZoneState& other) const {
    if (!(zone == other.zone) || !(generation == other.generation) ||
        has_observation != other.has_observation || recovered != other.recovered ||
        !(derating == other.derating)) {
      return false;
    }
    if (!has_observation) {
      return true;
    }
    const TemperatureObservation& a = observation;
    const TemperatureObservation& b = other.observation;
    return a.zone == b.zone && a.zone_generation == b.zone_generation &&
           a.evidence_generation == b.evidence_generation && a.sequence == b.sequence &&
           a.temperature == b.temperature && a.heat == b.heat && a.source == b.source &&
           a.provenance == b.provenance && a.observed_at == b.observed_at &&
           a.validity == b.validity && a.publisher_epoch == b.publisher_epoch &&
           a.publisher_incarnation == b.publisher_incarnation;
  }
};

enum class CommandKind : std::uint8_t {
  ApplyConfiguration = 0,
  IngestObservation = 1,
  CommitEvaluation = 2,
  AdvanceEpoch = 3,
};

// One retained idempotency receipt. The fingerprint covers the semantic
// content of the request; the result value is the scalar the original outcome
// reported, so a replay can return the original answer without re-acting.
struct IdempotencyRecord {
  CommandId command;
  CommandKind kind = CommandKind::ApplyConfiguration;
  std::uint64_t fingerprint = 0;
  // The scalars the original outcome reported, so a replay returns exactly the
  // original answer rather than a recomputed one. Their meaning is fixed per
  // command kind and is documented at each use:
  //   ApplyConfiguration result_value = configuration generation,
  //                      result_aux   = evidence generation,
  //                      result_extra = (zone count << 32) | coupling edges
  //   AdvanceEpoch       result_value = the epoch that was replaced
  //   CommitEvaluation   result_value = the number of zones whose derating was
  //                                     published
  std::uint64_t result_value = 0;
  std::uint64_t result_aux = 0;
  std::uint64_t result_extra = 0;
  StoreRevision applied_revision;
  CommitSequence applied_commit;
};

// Everything the durable store holds. The configuration is the declared world;
// the zone states are the dynamic state that was accepted against it.
struct WorldState {
  FencingState fencing;
  Configuration configuration;
  std::vector<PersistedZoneState> zones;
  std::vector<IdempotencyRecord> receipts;

  const PersistedZoneState* find(ZoneId zone) const;
  PersistedZoneState* find(ZoneId zone);
};

}  // namespace detail
}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_SRC_WORLD_HPP
