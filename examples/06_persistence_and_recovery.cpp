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

// Example 06: persistence and recovery.
//
// Phase A opens a durable store, declares one zone, ingests one fresh synthetic
// observation, evaluates it and publishes the evaluation, then closes.
//
// Phase B reopens the same directory. Everything that came back from disk is
// marked recovered, which is deliberately not fresh: the headroom is Unknown
// until a strictly newer observation replaces the recovered value. Nothing is
// deleted, and the example is safe to run twice against the same directory.

#include <thermal_zone_manager/thermal_zone_manager.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tzm = thermal_zone_manager;

namespace {

constexpr std::int64_t kFloorMc = 15000;
constexpr std::int64_t kDerateOnsetMc = 70000;
constexpr std::int64_t kCeilingMc = 90000;
constexpr std::int64_t kCriticalMc = 100000;
constexpr std::int64_t kResistanceUkw = 400000;
constexpr std::int64_t kDeclaredHeatMw = 20000;
constexpr std::int64_t kObservedMc = 53000;
constexpr std::int64_t kValidityNs = 60000000000LL;
constexpr const char* kDefaultRoot = "tzm-example-06-store";

tzm::ZoneConfiguration make_zone() {
  tzm::ZoneConfiguration zone;
  zone.id = tzm::ZoneId::first();
  zone.name = tzm::ZoneName::literal("cpu-bay");
  zone.envelope.floor_temp = tzm::MilliCelsius{kFloorMc};
  zone.envelope.derate_onset = tzm::MilliCelsius{kDerateOnsetMc};
  zone.envelope.ceiling_temp = tzm::MilliCelsius{kCeilingMc};
  zone.envelope.critical_temp = tzm::MilliCelsius{kCriticalMc};
  zone.declared_heat = tzm::MilliWatts{kDeclaredHeatMw};
  zone.thermal_resistance = tzm::MicroKelvinPerWatt{kResistanceUkw};
  return zone;
}

bool open_engine(const std::string& root, const char* actor,
                 std::unique_ptr<tzm::ThermalZoneEngine>& engine_out) {
  tzm::EngineOptions options;
  options.root = root;
  options.access = tzm::StoreAccess::ReadWrite;
  options.create_if_missing = true;
  options.actor = tzm::ActorId::literal(actor);
  tzm::Result<std::unique_ptr<tzm::ThermalZoneEngine>> opened = tzm::ThermalZoneEngine::open(options);
  if (!opened.ok()) {
    std::cerr << opened.error().to_string() << "\n";
    return false;
  }
  engine_out = std::move(opened.value());
  return true;
}

// Establishes a first epoch only when the recovered store still carries epoch
// zero, so a second run over the same store keeps the epoch it recovered.
bool ensure_epoch(tzm::ThermalZoneEngine& engine, tzm::ControlPlaneEpoch& epoch_out) {
  const tzm::FencingState fencing = engine.fencing();
  if (!fencing.epoch.is_zero()) {
    epoch_out = fencing.epoch;
    return true;
  }
  tzm::EpochRequest request;
  request.command = tzm::CommandId::first();
  request.attempt = tzm::AttemptId::first();
  request.expected_current = tzm::ControlPlaneEpoch::zero();
  request.target = tzm::ControlPlaneEpoch::first();
  const tzm::Result<tzm::EpochReceipt> receipt = engine.advance_epoch(request);
  if (!receipt.ok()) {
    std::cerr << receipt.error().to_string() << "\n";
    return false;
  }
  epoch_out = receipt.value().current;
  return true;
}

tzm::ObservationSequence next_sequence(const tzm::ThermalZoneEngine& engine) {
  const tzm::ZoneEvidence held = engine.evidence_for(tzm::ZoneId::first());
  if (held.present) {
    return held.observation.sequence.next();
  }
  return tzm::ObservationSequence::first();
}

bool ingest(tzm::ThermalZoneEngine& engine, const tzm::Configuration& configuration,
            tzm::ControlPlaneEpoch epoch, std::int64_t temperature_mc) {
  const tzm::ZoneConfiguration* declared = configuration.find(tzm::ZoneId::first());
  if (declared == nullptr) {
    std::cerr << "the declared zone is missing from the configuration\n";
    return false;
  }
  tzm::TemperatureObservation observation;
  observation.zone = declared->id;
  observation.zone_generation = declared->generation;
  observation.evidence_generation = configuration.evidence_generation();
  observation.sequence = next_sequence(engine);
  observation.temperature = tzm::MilliCelsius{temperature_mc};
  observation.source = tzm::SourceId::literal("synthetic-harness-06");
  observation.provenance = tzm::ProvenanceKind::SyntheticHarness;
  observation.observed_at = engine.clock().now();
  observation.validity = tzm::Nanoseconds{kValidityNs};
  observation.publisher_epoch = epoch;
  observation.publisher_incarnation = engine.incarnation();
  const tzm::Result<tzm::ObservationReceipt> ingested = engine.ingest_observation(observation);
  if (!ingested.ok()) {
    std::cerr << ingested.error().to_string() << "\n";
    return false;
  }
  return true;
}

tzm::Result<tzm::EvaluationResult> evaluate_zone(tzm::ThermalZoneEngine& engine,
                                                 const tzm::Configuration& configuration,
                                                 tzm::ControlPlaneEpoch epoch,
                                                 std::uint64_t evaluation_value) {
  tzm::EvaluationRequest request;
  request.id = tzm::EvaluationId::from_value(evaluation_value);
  request.epoch = epoch;
  request.configuration_generation = configuration.generation();
  request.evidence_generation = configuration.evidence_generation();
  return engine.evaluate(request);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? std::string(argv[1]) : std::string(kDefaultRoot);

  std::cout << "example 06: persistence and recovery\n";
  std::cout << "  store root: " << root << " (files are left in place, nothing is deleted)\n";
  std::cout << "  input: synthetic harness observations, not real hardware evidence\n";

  // -- phase A: declare, observe, evaluate, publish, close -------------------
  tzm::ControlPlaneEpoch epoch;
  {
    std::unique_ptr<tzm::ThermalZoneEngine> engine;
    if (!open_engine(root, "tzm-example-06-a", engine)) {
      return 1;
    }
    if (!ensure_epoch(*engine, epoch)) {
      return 1;
    }

    // The configuration generation is read from the live engine, so a second
    // run replaces the declaration instead of being refused as stale.
    tzm::ConfigurationRequest declaration;
    declaration.command = tzm::CommandId::from_value(engine->fencing().revision.raw() + 1);
    declaration.attempt = tzm::AttemptId::first();
    declaration.epoch = epoch;
    declaration.expected_generation = engine->configuration().generation();
    declaration.zones.push_back(make_zone());
    declaration.policy.coupling_hops = 1;
    const tzm::Result<tzm::ConfigurationReceipt> applied =
        engine->apply_configuration(declaration);
    if (!applied.ok()) {
      std::cerr << applied.error().to_string() << "\n";
      return 1;
    }

    const tzm::Configuration configuration = engine->configuration();
    if (!ingest(*engine, configuration, epoch, kObservedMc)) {
      return 1;
    }
    const tzm::Result<tzm::EvaluationResult> evaluated = evaluate_zone(*engine, configuration, epoch, 1);
    if (!evaluated.ok()) {
      std::cerr << evaluated.error().to_string() << "\n";
      return 1;
    }
    const tzm::ZoneThermalState& state = evaluated.value().zones().front();
    std::cout << "  phase A: fresh observation, evidence " << tzm::to_string(state.evidence_freshness)
              << ", headroom " << tzm::to_string(state.headroom.status) << " (margin "
              << tzm::format_milli(state.headroom.temperature_margin.value) << " C)\n";

    tzm::EvaluationCommitRequest commit;
    commit.command = tzm::CommandId::from_value(engine->fencing().revision.raw() + 1);
    commit.attempt = tzm::AttemptId::from_value(2);
    commit.epoch = epoch;
    commit.evaluation = evaluated.value().id();
    commit.configuration_generation = evaluated.value().configuration_generation();
    commit.evidence_generation = evaluated.value().evidence_generation();
    commit.basis_revision = evaluated.value().basis_revision();
    const tzm::Result<tzm::EvaluationCommitReceipt> committed = engine->commit_evaluation(commit);
    if (!committed.ok()) {
      std::cerr << committed.error().to_string() << "\n";
      return 1;
    }
    std::cout << "  phase A: published " << committed.value().zones_updated
              << " zone derating outcome(s) at revision "
              << tzm::to_string(committed.value().revision) << "\n";

    const tzm::Status closed = engine->close();
    if (!closed.ok()) {
      std::cerr << closed.error().to_string() << "\n";
      return 1;
    }
    std::cout << "  phase A: closed the store\n";
  }

  // -- phase B: reopen, inspect, and prove recovery is not freshness ---------
  {
    std::unique_ptr<tzm::ThermalZoneEngine> engine;
    if (!open_engine(root, "tzm-example-06-b", engine)) {
      return 1;
    }
    std::cout << tzm::render(engine->recovery_report());

    const tzm::ZoneEvidence held = engine->evidence_for(tzm::ZoneId::first());
    if (!held.present) {
      std::cerr << "the recovered store holds no observation for the zone\n";
      return 1;
    }
    std::cout << "  phase B: recovered observation sequence "
              << tzm::to_string(held.observation.sequence) << ", freshness "
              << tzm::to_string(held.freshness) << "\n";

    const tzm::Configuration configuration = engine->configuration();
    const tzm::Result<tzm::EvaluationResult> recovered = evaluate_zone(*engine, configuration, epoch, 2);
    if (!recovered.ok()) {
      std::cerr << recovered.error().to_string() << "\n";
      return 1;
    }
    const tzm::ZoneThermalState& recovered_state = recovered.value().zones().front();
    std::cout << "  phase B: headroom " << tzm::to_string(recovered_state.headroom.status)
              << ", proven " << (tzm::is_proven(recovered_state.headroom.status) ? "yes" : "no")
              << " - a recovered value is never fresh physical evidence\n";
    if (recovered_state.headroom.status != tzm::HeadroomStatus::Unknown ||
        tzm::is_usable(recovered_state.evidence_freshness)) {
      std::cerr << "recovered evidence was treated as usable\n";
      return 1;
    }

    // A strictly newer observation from a live source clears the recovered
    // marker, and only then does the zone have a proven margin again.
    if (!ingest(*engine, configuration, epoch, kObservedMc)) {
      return 1;
    }
    const tzm::Result<tzm::EvaluationResult> refreshed = evaluate_zone(*engine, configuration, epoch, 3);
    if (!refreshed.ok()) {
      std::cerr << refreshed.error().to_string() << "\n";
      return 1;
    }
    const tzm::ZoneThermalState& refreshed_state = refreshed.value().zones().front();
    std::cout << "  phase B: strictly newer observation sequence "
              << tzm::to_string(engine->evidence_for(tzm::ZoneId::first()).observation.sequence)
              << " -> evidence " << tzm::to_string(refreshed_state.evidence_freshness)
              << ", headroom " << tzm::to_string(refreshed_state.headroom.status) << "\n";
    if (refreshed_state.headroom.status != tzm::HeadroomStatus::Known) {
      std::cerr << "a strictly newer live observation did not restore a proven headroom\n";
      return 1;
    }

    const tzm::Status closed = engine->close();
    if (!closed.ok()) {
      std::cerr << closed.error().to_string() << "\n";
      return 1;
    }
  }

  std::cout << "  the durable store was reopened, read back and continued; nothing was deleted\n";
  return 0;
}
