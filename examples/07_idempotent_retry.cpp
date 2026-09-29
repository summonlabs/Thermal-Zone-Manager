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

// Example 07: idempotent retry.
//
// A retry is a second delivery of the same command on a new attempt. The engine
// answers either with the original outcome (replayed) or with a refusal; it
// never applies the same command twice.
//
// The example shows what each path actually does:
//   * re-delivering a configuration command replays the original receipt - the
//     same configuration generation, revision and commit sequence - and the
//     world does not move a second time;
//   * re-delivering an identical observation, and retrying an identical
//     evaluation commit, replay their original receipts in the same way;
//   * a *new* command identifier carrying a superseded expected generation is
//     refused as stale authority, because that is a new plan against an old
//     world rather than a retry;
//   * reusing a command identifier for a different request is refused with
//     IdempotencyConflict.

#include <thermal_zone_manager/thermal_zone_manager.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tzm = thermal_zone_manager;

namespace {

constexpr std::int64_t kClockStartNs = 1700000000000000000LL;
constexpr std::int64_t kFloorMc = 15000;
constexpr std::int64_t kDerateOnsetMc = 70000;
constexpr std::int64_t kCeilingMc = 90000;
constexpr std::int64_t kCriticalMc = 100000;
constexpr std::int64_t kObservedMc = 52000;
constexpr std::int64_t kValidityNs = 60000000000LL;
constexpr std::uint64_t kDeclarationCommand = 2;
constexpr std::uint64_t kCommitCommand = 3;

tzm::ZoneConfiguration make_zone(std::int64_t resistance_ukw) {
  tzm::ZoneConfiguration zone;
  zone.id = tzm::ZoneId::first();
  zone.name = tzm::ZoneName::literal("cpu-bay");
  zone.envelope.floor_temp = tzm::MilliCelsius{kFloorMc};
  zone.envelope.derate_onset = tzm::MilliCelsius{kDerateOnsetMc};
  zone.envelope.ceiling_temp = tzm::MilliCelsius{kCeilingMc};
  zone.envelope.critical_temp = tzm::MilliCelsius{kCriticalMc};
  zone.declared_heat = tzm::MilliWatts{20000};
  zone.thermal_resistance = tzm::MicroKelvinPerWatt{resistance_ukw};
  return zone;
}

// The exact request body. Building it twice from the same arguments is what
// makes the second delivery a retry rather than a new request.
tzm::ConfigurationRequest make_declaration(tzm::CommandId command, tzm::AttemptId attempt,
                                           tzm::ControlPlaneEpoch epoch,
                                           tzm::ConfigurationGeneration expected,
                                           std::int64_t resistance_ukw) {
  tzm::ConfigurationRequest request;
  request.command = command;
  request.attempt = attempt;
  request.epoch = epoch;
  request.expected_generation = expected;
  request.zones.push_back(make_zone(resistance_ukw));
  request.policy.coupling_hops = 1;
  return request;
}

tzm::TemperatureObservation make_observation(const tzm::ThermalZoneEngine& engine,
                                             const tzm::Configuration& configuration,
                                             tzm::ControlPlaneEpoch epoch) {
  const tzm::ZoneConfiguration* declared = configuration.find(tzm::ZoneId::first());
  tzm::TemperatureObservation observation;
  if (declared == nullptr) {
    return observation;
  }
  observation.zone = declared->id;
  observation.zone_generation = declared->generation;
  observation.evidence_generation = configuration.evidence_generation();
  observation.sequence = tzm::ObservationSequence::first();
  observation.temperature = tzm::MilliCelsius{kObservedMc};
  observation.source = tzm::SourceId::literal("synthetic-harness-07");
  observation.provenance = tzm::ProvenanceKind::SyntheticHarness;
  observation.observed_at = engine.clock().now();
  observation.validity = tzm::Nanoseconds{kValidityNs};
  observation.publisher_epoch = epoch;
  observation.publisher_incarnation = engine.incarnation();
  return observation;
}

}  // namespace

int main() {
  tzm::ManualClock clock{tzm::Timestamp{kClockStartNs}};

  tzm::EngineOptions options;
  options.actor = tzm::ActorId::literal("tzm-example-07");
  options.clock = &clock;

  tzm::Result<std::unique_ptr<tzm::ThermalZoneEngine>> opened = tzm::ThermalZoneEngine::open(options);
  if (!opened.ok()) {
    std::cerr << opened.error().to_string() << "\n";
    return 1;
  }
  const std::unique_ptr<tzm::ThermalZoneEngine> engine = std::move(opened.value());

  tzm::EpochRequest epoch_request;
  epoch_request.command = tzm::CommandId::first();
  epoch_request.attempt = tzm::AttemptId::first();
  epoch_request.expected_current = tzm::ControlPlaneEpoch::zero();
  epoch_request.target = tzm::ControlPlaneEpoch::first();
  const tzm::Result<tzm::EpochReceipt> epoch = engine->advance_epoch(epoch_request);
  if (!epoch.ok()) {
    std::cerr << epoch.error().to_string() << "\n";
    return 1;
  }
  const tzm::ControlPlaneEpoch current_epoch = epoch.value().current;

  std::cout << "example 07: idempotent retry\n";
  std::cout << "  input: synthetic harness observations, not real hardware evidence\n";

  // -- first delivery --------------------------------------------------------
  const tzm::Result<tzm::ConfigurationReceipt> applied = engine->apply_configuration(
      make_declaration(tzm::CommandId::from_value(kDeclarationCommand),
                       tzm::AttemptId::from_value(2), current_epoch,
                       tzm::ConfigurationGeneration::zero(), 400000));
  if (!applied.ok()) {
    std::cerr << applied.error().to_string() << "\n";
    return 1;
  }
  std::cout << "  configuration command " << kDeclarationCommand << " applied: generation "
            << tzm::to_string(applied.value().generation) << ", revision "
            << tzm::to_string(applied.value().revision) << ", commit "
            << tzm::to_string(applied.value().commit) << ", replayed "
            << (applied.value().replayed ? "yes" : "no") << "\n";

  // -- identical redelivery of the configuration command ---------------------
  const tzm::FencingState before_retry = engine->fencing();
  const tzm::Result<tzm::ConfigurationReceipt> redelivered = engine->apply_configuration(
      make_declaration(tzm::CommandId::from_value(kDeclarationCommand),
                       tzm::AttemptId::from_value(3), current_epoch,
                       tzm::ConfigurationGeneration::zero(), 400000));
  if (!redelivered.ok()) {
    std::cerr << redelivered.error().to_string() << "\n";
    return 1;
  }
  std::cout << "  identical redelivery replayed: replayed "
            << (redelivered.value().replayed ? "yes" : "no") << ", generation "
            << tzm::to_string(redelivered.value().generation) << " (original "
            << tzm::to_string(applied.value().generation) << "), revision "
            << tzm::to_string(redelivered.value().revision) << " (original "
            << tzm::to_string(applied.value().revision) << "), commit "
            << tzm::to_string(redelivered.value().commit) << " (original "
            << tzm::to_string(applied.value().commit) << ")\n";
  if (!redelivered.value().replayed ||
      redelivered.value().generation != applied.value().generation ||
      redelivered.value().revision != applied.value().revision ||
      redelivered.value().commit != applied.value().commit) {
    std::cerr << "the configuration retry did not replay the original outcome\n";
    return 1;
  }
  const tzm::FencingState after_retry = engine->fencing();
  std::cout << "      the world did not move: revision "
            << tzm::to_string(before_retry.revision) << " -> "
            << tzm::to_string(after_retry.revision) << ", commit "
            << tzm::to_string(before_retry.commit) << " -> "
            << tzm::to_string(after_retry.commit)
            << ", so the command was not applied a second time\n";
  if (before_retry.revision != after_retry.revision ||
      before_retry.commit != after_retry.commit) {
    std::cerr << "the replayed redelivery still moved the world\n";
    return 1;
  }

  // -- a new command planned against the superseded generation ---------------
  const tzm::Result<tzm::ConfigurationReceipt> stale = engine->apply_configuration(
      make_declaration(tzm::CommandId::from_value(kDeclarationCommand + 1),
                       tzm::AttemptId::from_value(7), current_epoch,
                       tzm::ConfigurationGeneration::zero(), 400000));
  if (stale.ok()) {
    std::cerr << "a new command against a superseded generation was accepted\n";
    return 1;
  }
  std::cout << "  a new command against the superseded generation refused: "
            << tzm::to_string(stale.error().code()) << "\n";
  std::cout << "      " << stale.error().message() << "\n";
  if (stale.error().code() != tzm::ErrorCode::StaleConfigurationGeneration) {
    std::cerr << "the stale new command did not report stale authority\n";
    return 1;
  }

  // -- an observation, and its identical redelivery --------------------------
  const tzm::Configuration configuration = engine->configuration();
  const tzm::TemperatureObservation observation =
      make_observation(*engine, configuration, current_epoch);
  const tzm::Result<tzm::ObservationReceipt> ingested = engine->ingest_observation(observation);
  if (!ingested.ok()) {
    std::cerr << ingested.error().to_string() << "\n";
    return 1;
  }
  const tzm::Result<tzm::ObservationReceipt> reingested = engine->ingest_observation(observation);
  if (!reingested.ok()) {
    std::cerr << reingested.error().to_string() << "\n";
    return 1;
  }
  std::cout << "  observation sequence " << tzm::to_string(ingested.value().sequence)
            << " delivered twice: replayed " << (reingested.value().replayed ? "yes" : "no")
            << ", revision " << tzm::to_string(reingested.value().revision) << " (original "
            << tzm::to_string(ingested.value().revision) << "), commit "
            << tzm::to_string(reingested.value().commit) << " (original "
            << tzm::to_string(ingested.value().commit) << ")\n";
  if (!reingested.value().replayed || reingested.value().revision != ingested.value().revision ||
      reingested.value().commit != ingested.value().commit) {
    std::cerr << "the observation redelivery did not replay the original outcome\n";
    return 1;
  }

  // -- an evaluation commit, and its identical retry -------------------------
  tzm::EvaluationRequest request;
  request.id = tzm::EvaluationId::first();
  request.epoch = current_epoch;
  request.configuration_generation = configuration.generation();
  request.evidence_generation = configuration.evidence_generation();
  const tzm::Result<tzm::EvaluationResult> evaluated = engine->evaluate(request);
  if (!evaluated.ok()) {
    std::cerr << evaluated.error().to_string() << "\n";
    return 1;
  }

  tzm::EvaluationCommitRequest commit;
  commit.command = tzm::CommandId::from_value(kCommitCommand);
  commit.attempt = tzm::AttemptId::from_value(4);
  commit.epoch = current_epoch;
  commit.evaluation = evaluated.value().id();
  commit.configuration_generation = evaluated.value().configuration_generation();
  commit.evidence_generation = evaluated.value().evidence_generation();
  commit.basis_revision = evaluated.value().basis_revision();

  const tzm::Result<tzm::EvaluationCommitReceipt> committed = engine->commit_evaluation(commit);
  if (!committed.ok()) {
    std::cerr << committed.error().to_string() << "\n";
    return 1;
  }
  commit.attempt = tzm::AttemptId::from_value(5);  // same command, new delivery attempt
  const tzm::Result<tzm::EvaluationCommitReceipt> recommitted = engine->commit_evaluation(commit);
  if (!recommitted.ok()) {
    std::cerr << recommitted.error().to_string() << "\n";
    return 1;
  }
  std::cout << "  evaluation commit command " << kCommitCommand << " retried: replayed "
            << (recommitted.value().replayed ? "yes" : "no") << ", revision "
            << tzm::to_string(recommitted.value().revision) << " (original "
            << tzm::to_string(committed.value().revision) << "), commit "
            << tzm::to_string(recommitted.value().commit) << " (original "
            << tzm::to_string(committed.value().commit) << ")\n";
  if (!recommitted.value().replayed ||
      recommitted.value().revision != committed.value().revision ||
      recommitted.value().commit != committed.value().commit) {
    std::cerr << "the commit retry did not replay the original receipt\n";
    return 1;
  }

  // -- the same command identifier with a different body ---------------------
  const tzm::Result<tzm::ConfigurationReceipt> conflicted = engine->apply_configuration(
      make_declaration(tzm::CommandId::from_value(kDeclarationCommand),
                       tzm::AttemptId::from_value(6), current_epoch,
                       engine->configuration().generation(), 500000));
  if (conflicted.ok()) {
    std::cerr << "a command identifier was reused for a different request\n";
    return 1;
  }
  std::cout << "  command " << kDeclarationCommand << " reused with a different body refused: "
            << tzm::to_string(conflicted.error().code()) << "\n";
  std::cout << "      " << conflicted.error().message() << "\n";
  if (conflicted.error().code() != tzm::ErrorCode::IdempotencyConflict) {
    std::cerr << "the identifier reuse did not report an idempotency conflict\n";
    return 1;
  }

  std::cout << "  a command identifier is bound to one request body: a retry replays or refuses, "
               "and never applies twice\n";
  return 0;
}
