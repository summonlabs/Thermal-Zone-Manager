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

#ifndef THERMAL_ZONE_MANAGER_ENGINE_HPP
#define THERMAL_ZONE_MANAGER_ENGINE_HPP

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "thermal_zone_manager/clock.hpp"
#include "thermal_zone_manager/configuration.hpp"
#include "thermal_zone_manager/constraints.hpp"
#include "thermal_zone_manager/derating.hpp"
#include "thermal_zone_manager/errors.hpp"
#include "thermal_zone_manager/evaluation.hpp"
#include "thermal_zone_manager/evidence.hpp"
#include "thermal_zone_manager/export.hpp"
#include "thermal_zone_manager/ids.hpp"
#include "thermal_zone_manager/store.hpp"
#include "thermal_zone_manager/units.hpp"

namespace thermal_zone_manager {

struct EngineOptions {
  // Store directory. Empty selects an ephemeral in-memory engine, which has no
  // durable state and no cross-process exclusion; that mode is for examples,
  // benchmarks and analysis, never for a production authority path.
  std::string root;
  StoreAccess access = StoreAccess::ReadWrite;
  bool create_if_missing = true;
  ActorId actor;
  // Zero asks the engine to mint one.
  ControllerIncarnation incarnation;
  // Not owned. Null selects the system clock.
  const Clock* clock = nullptr;
  Nanoseconds lock_wait{0};
  bool verify_after_write = true;
  // Payload capacity of each durable slot. It is recorded when the store is
  // created and read back from the file afterwards.
  std::size_t slot_capacity = kDefaultStoreSlotPayload;
};

// A mutation request. Every mutation carries the command that identifies it,
// the attempt that delivered it, and the control-plane epoch it was planned
// under. The attempt is part of the delivery, not of the request identity, so
// a lost-response retry of the same command replays the original outcome.
struct ConfigurationRequest {
  CommandId command;
  AttemptId attempt;
  ControlPlaneEpoch epoch;
  // The configuration generation this request was planned against. A mismatch
  // is refused; the engine never re-bases a request onto a newer world.
  ConfigurationGeneration expected_generation;
  // Declared zones. The generation of each zone, and the policy generation,
  // are assigned by the engine: a caller-supplied value is replaced, never
  // trusted.
  std::vector<ZoneConfiguration> zones;
  std::vector<CouplingEdge> coupling;
  ThermalPolicy policy;
};

struct EpochRequest {
  CommandId command;
  AttemptId attempt;
  ControlPlaneEpoch expected_current;
  ControlPlaneEpoch target;
};

struct EvaluationCommitRequest {
  CommandId command;
  AttemptId attempt;
  ControlPlaneEpoch epoch;
  // The evaluation whose derating outcome is to be published. The engine
  // refuses an identifier it did not produce, so no caller can fabricate a
  // derating level.
  EvaluationId evaluation;
  ConfigurationGeneration configuration_generation;
  EvidenceGeneration evidence_generation;
  StoreRevision basis_revision;
};

struct ConfigurationReceipt {
  ConfigurationGeneration generation;
  EvidenceGeneration evidence_generation;
  StoreRevision revision;
  CommitSequence commit;
  std::size_t zone_count = 0;
  std::size_t coupling_edge_count = 0;
  std::uint64_t fingerprint = 0;
  // True when an idempotent retry replayed the original outcome instead of
  // re-applying the request.
  bool replayed = false;
  // False for an ephemeral engine.
  bool durable = false;
};

struct ObservationReceipt {
  ZoneId zone;
  ObservationSequence sequence;
  StoreRevision revision;
  CommitSequence commit;
  std::uint64_t fingerprint = 0;
  bool replayed = false;
  bool durable = false;
};

struct EvaluationCommitReceipt {
  EvaluationId evaluation;
  StoreRevision revision;
  CommitSequence commit;
  std::size_t zones_updated = 0;
  std::uint64_t fingerprint = 0;
  bool replayed = false;
  bool durable = false;
};

struct EpochReceipt {
  ControlPlaneEpoch previous;
  ControlPlaneEpoch current;
  StoreRevision revision;
  CommitSequence commit;
  std::uint64_t fingerprint = 0;
  bool replayed = false;
  bool durable = false;
};

// The Thermal Zone Manager runtime.
//
// Concurrency: every public member is safe to call from any thread. Mutation
// takes the durable writer lock first and then the state mutex; reads take only
// the state mutex in shared mode, or only the recent-evaluation mutex. The two
// mutexes are never held at the same time, and no callback of any kind is
// invoked while a lock is held, so there is no re-entry path into this object.
class ThermalZoneEngine {
 public:
  // Opens (and, when asked, creates) the store, recovers the durable
  // generation, and marks every recovered observation as not-fresh.
  static Result<std::unique_ptr<ThermalZoneEngine>> open(const EngineOptions& options);

  ~ThermalZoneEngine();
  ThermalZoneEngine(const ThermalZoneEngine&) = delete;
  ThermalZoneEngine& operator=(const ThermalZoneEngine&) = delete;
  ThermalZoneEngine(ThermalZoneEngine&&) = delete;
  ThermalZoneEngine& operator=(ThermalZoneEngine&&) = delete;

  // -- inspection ----------------------------------------------------------

  bool durable() const noexcept;
  bool holds_writer() const noexcept;
  bool closed() const noexcept;
  const Clock& clock() const noexcept;
  ControllerIncarnation incarnation() const;

  // A copy of the declared world, taken under the shared lock.
  Configuration configuration() const;
  FencingState fencing() const;
  RecoveryReport recovery_report() const;
  std::string describe() const;

  // The evidence currently held for a zone, and the freshness verdict of the
  // most recent evaluation. Unknown zone yields Missing.
  ZoneEvidence evidence_for(ZoneId zone) const;
  DeratingState derating_for(ZoneId zone) const;

  // -- mutation ------------------------------------------------------------

  // Replaces the declared world. Assigns ConfigurationGeneration and
  // EvidenceGeneration, and per-zone generations: an unchanged zone keeps its
  // generation, a changed or new zone gets a new one.
  Result<ConfigurationReceipt> apply_configuration(const ConfigurationRequest& request);

  // Accepts one observation. The zone, zone generation, evidence generation
  // and control-plane epoch are all checked; a stale or foreign observation is
  // refused rather than downgraded.
  //
  // Idempotency for observations is keyed by (zone, sequence) and enforced
  // against durable state, not by a command receipt: re-delivering an identical
  // observation replays the original outcome, and re-using a sequence with a
  // different payload is refused as conflicting.
  Result<ObservationReceipt> ingest_observation(const TemperatureObservation& observation);

  // Advances the control-plane epoch. Every later request must carry the new
  // epoch; requests carrying the old one are refused.
  Result<EpochReceipt> advance_epoch(const EpochRequest& request);

  // -- evaluation ----------------------------------------------------------

  // Computes the thermal state of every requested zone. Pure: it mutates
  // nothing durable and mutates nothing observable. The returned result
  // carries the derating state each zone would move to, which only becomes
  // authoritative when commit_evaluation() publishes it.
  Result<EvaluationResult> evaluate(const EvaluationRequest& request);

  // Durably publishes the derating outcome of a previous evaluation, fenced by
  // the evaluation identifier, both generations and the store revision. A
  // stale evaluation is refused; a retry of the same command replays the
  // original receipt.
  Result<EvaluationCommitReceipt> commit_evaluation(const EvaluationCommitRequest& request);

  // Releases the writer lock. Repeated close is safe.
  Status close();

 private:
  ThermalZoneEngine();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Human-readable renderings. Human output only; never the machine contract.
TZM_API std::string render(const Configuration& configuration);
TZM_API std::string render(const FencingState& fencing);
TZM_API std::string render(const RecoveryReport& report);
TZM_API std::string render(const ConfigurationReceipt& receipt);
TZM_API std::string render(const ObservationReceipt& receipt);
TZM_API std::string render(const EvaluationCommitReceipt& receipt);

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_ENGINE_HPP
