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

// Package consumer: an independent, out-of-tree use of the installed package.
//
// This translation unit only includes <thermal_zone_manager/...> and links
// ThermalZoneManager::thermal_zone_manager, so it compiles and links exactly
// what an installed package provides. It runs a real lifecycle end to end:
// open an ephemeral engine, establish a control-plane epoch, declare two
// coupled zones, ingest one fresh observation, evaluate, check the emitted
// headroom and constraint, then publish the evaluation.

#include <thermal_zone_manager/thermal_zone_manager.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>

namespace tzm = thermal_zone_manager;

namespace {

constexpr std::int64_t kClockStartNs = 1700000000000000000LL;
constexpr std::int64_t kFloorMc = 15000;
constexpr std::int64_t kDerateOnsetMc = 70000;
constexpr std::int64_t kCeilingMc = 90000;
constexpr std::int64_t kCriticalMc = 100000;
constexpr std::int64_t kResistanceUkw = 400000;
constexpr std::int64_t kSinkTemperatureMc = 55000;
constexpr std::int64_t kSourceTemperatureMc = 40000;
constexpr std::int64_t kSourceHeatMw = 50000;
constexpr std::int64_t kValidityNs = 60000000000LL;
constexpr std::int32_t kCouplingPpm = 400000;

int refuse(const tzm::Error& error) {
  std::cerr << error.to_string() << "\n";
  return 1;
}

tzm::ZoneConfiguration make_zone(std::uint64_t id, const char* name, std::int64_t heat_mw) {
  tzm::ZoneConfiguration zone;
  zone.id = tzm::ZoneId::from_value(id);
  zone.name = tzm::ZoneName::literal(name);
  zone.envelope.floor_temp = tzm::MilliCelsius{kFloorMc};
  zone.envelope.derate_onset = tzm::MilliCelsius{kDerateOnsetMc};
  zone.envelope.ceiling_temp = tzm::MilliCelsius{kCeilingMc};
  zone.envelope.critical_temp = tzm::MilliCelsius{kCriticalMc};
  zone.declared_heat = tzm::MilliWatts{heat_mw};
  zone.thermal_resistance = tzm::MicroKelvinPerWatt{kResistanceUkw};
  return zone;
}

}  // namespace

int main() {
  tzm::ManualClock clock{tzm::Timestamp{kClockStartNs}};

  tzm::EngineOptions options;
  options.actor = tzm::ActorId::literal("tzm-package-consumer");
  options.clock = &clock;

  tzm::Result<std::unique_ptr<tzm::ThermalZoneEngine>> opened = tzm::ThermalZoneEngine::open(options);
  if (!opened.ok()) {
    return refuse(opened.error());
  }
  const std::unique_ptr<tzm::ThermalZoneEngine> engine = std::move(opened.value());

  tzm::EpochRequest epoch_request;
  epoch_request.command = tzm::CommandId::first();
  epoch_request.attempt = tzm::AttemptId::first();
  epoch_request.expected_current = tzm::ControlPlaneEpoch::zero();
  epoch_request.target = tzm::ControlPlaneEpoch::first();
  const tzm::Result<tzm::EpochReceipt> epoch = engine->advance_epoch(epoch_request);
  if (!epoch.ok()) {
    return refuse(epoch.error());
  }

  tzm::ConfigurationRequest declaration;
  declaration.command = tzm::CommandId::from_value(2);
  declaration.attempt = tzm::AttemptId::from_value(2);
  declaration.epoch = epoch.value().current;
  declaration.expected_generation = tzm::ConfigurationGeneration::zero();
  declaration.zones.push_back(make_zone(1, "consumer-sink", 20000));
  declaration.zones.push_back(make_zone(2, "consumer-source", kSourceHeatMw));
  declaration.policy.coupling_hops = 1;

  tzm::CouplingEdge edge;
  edge.source = tzm::ZoneId::from_value(2);
  edge.sink = tzm::ZoneId::from_value(1);
  edge.coefficient = tzm::PartsPerMillion{kCouplingPpm};
  declaration.coupling.push_back(edge);

  const tzm::Result<tzm::ConfigurationReceipt> applied =
      engine->apply_configuration(declaration);
  if (!applied.ok()) {
    return refuse(applied.error());
  }

  const tzm::Configuration configuration = engine->configuration();
  for (std::uint64_t id = 1; id <= 2; ++id) {
    const tzm::ZoneId zone_id = tzm::ZoneId::from_value(id);
    const tzm::ZoneConfiguration* declared = configuration.find(zone_id);
    if (declared == nullptr) {
      std::cerr << "the declared zone is missing from the configuration\n";
      return 1;
    }
    tzm::TemperatureObservation observation;
    observation.zone = zone_id;
    observation.zone_generation = declared->generation;
    observation.evidence_generation = configuration.evidence_generation();
    observation.sequence = tzm::ObservationSequence::first();
    observation.temperature =
        tzm::MilliCelsius{id == 1 ? kSinkTemperatureMc : kSourceTemperatureMc};
    observation.source = tzm::SourceId::literal("package-consumer-harness");
    observation.provenance = tzm::ProvenanceKind::SyntheticHarness;
    observation.observed_at = clock.now();
    observation.validity = tzm::Nanoseconds{kValidityNs};
    observation.publisher_epoch = epoch.value().current;
    observation.publisher_incarnation = engine->incarnation();
    const tzm::Result<tzm::ObservationReceipt> ingested = engine->ingest_observation(observation);
    if (!ingested.ok()) {
      return refuse(ingested.error());
    }
  }

  tzm::EvaluationRequest request;
  request.id = tzm::EvaluationId::first();
  request.epoch = epoch.value().current;
  request.configuration_generation = configuration.generation();
  request.evidence_generation = configuration.evidence_generation();

  const tzm::Result<tzm::EvaluationResult> evaluated = engine->evaluate(request);
  if (!evaluated.ok()) {
    return refuse(evaluated.error());
  }

  const tzm::ZoneThermalState* sink = evaluated.value().find(tzm::ZoneId::from_value(1));
  const tzm::PlacementConstraint* constraint =
      evaluated.value().constraint_for(tzm::ZoneId::from_value(1));
  if (sink == nullptr || constraint == nullptr) {
    std::cerr << "the evaluation did not cover the declared sink zone\n";
    return 1;
  }
  if (sink->headroom.status != tzm::HeadroomStatus::Known) {
    std::cerr << "the sink headroom is " << tzm::to_string(sink->headroom.status)
              << ", expected known\n";
    return 1;
  }
  if (constraint->kind != tzm::PlacementConstraintKind::Bounded &&
      constraint->kind != tzm::PlacementConstraintKind::Prohibited) {
    std::cerr << "the sink constraint is " << tzm::to_string(constraint->kind)
              << ", expected bounded or prohibited\n";
    return 1;
  }

  tzm::EvaluationCommitRequest commit;
  commit.command = tzm::CommandId::from_value(3);
  commit.attempt = tzm::AttemptId::from_value(3);
  commit.epoch = epoch.value().current;
  commit.evaluation = evaluated.value().id();
  commit.configuration_generation = evaluated.value().configuration_generation();
  commit.evidence_generation = evaluated.value().evidence_generation();
  commit.basis_revision = evaluated.value().basis_revision();
  const tzm::Result<tzm::EvaluationCommitReceipt> committed = engine->commit_evaluation(commit);
  if (!committed.ok()) {
    return refuse(committed.error());
  }

  std::cout << "tzm package consumer: ok (version " << tzm::version_string() << ")\n";
  return 0;
}
