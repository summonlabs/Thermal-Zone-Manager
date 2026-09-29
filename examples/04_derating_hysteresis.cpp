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

// Example 04: derating hysteresis.
//
// A four-step ladder over the ramp [70.000 C, 90.000 C] with a 1.000 C release
// margin and a release hold of three observations. The manual clock and the
// harness observations make the sequence exact and repeatable.
//
// Escalation is immediate; release takes one step at a time and only after
// release_hold_observations consecutive usable observations at or below the
// release threshold. A reading that is not usable can never release a step.

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
constexpr std::int64_t kResistanceUkw = 400000;
constexpr std::int64_t kRecoveryMarginMc = 1000;
constexpr std::uint32_t kDerateSteps = 4;
constexpr std::uint32_t kReleaseHold = 3;
constexpr std::int64_t kValidityNs = 60000000000LL;

struct Counters {
  std::uint64_t command = 1;
  std::uint64_t attempt = 1;
  std::uint64_t evaluation = 1;
  std::uint64_t sequence = 1;
};

tzm::ZoneConfiguration make_zone() {
  tzm::ZoneConfiguration zone;
  zone.id = tzm::ZoneId::first();
  zone.name = tzm::ZoneName::literal("cpu-bay");
  zone.envelope.floor_temp = tzm::MilliCelsius{kFloorMc};
  zone.envelope.derate_onset = tzm::MilliCelsius{kDerateOnsetMc};
  zone.envelope.ceiling_temp = tzm::MilliCelsius{kCeilingMc};
  zone.envelope.critical_temp = tzm::MilliCelsius{kCriticalMc};
  zone.declared_heat = tzm::MilliWatts{20000};
  zone.thermal_resistance = tzm::MicroKelvinPerWatt{kResistanceUkw};
  zone.derate_steps = kDerateSteps;
  zone.recovery_margin = tzm::MilliCelsius{kRecoveryMarginMc};
  return zone;
}

// Hands one fresh synthetic observation to the engine.
bool ingest(tzm::ThermalZoneEngine& engine, const tzm::Configuration& configuration,
            tzm::ControlPlaneEpoch epoch, Counters& counters, std::int64_t temperature_mc,
            tzm::Timestamp observed_at) {
  const tzm::ZoneConfiguration* declared = configuration.find(tzm::ZoneId::first());
  if (declared == nullptr) {
    std::cerr << "the declared zone is missing from the configuration\n";
    return false;
  }
  tzm::TemperatureObservation observation;
  observation.zone = declared->id;
  observation.zone_generation = declared->generation;
  observation.evidence_generation = configuration.evidence_generation();
  observation.sequence = tzm::ObservationSequence::from_value(counters.sequence++);
  observation.temperature = tzm::MilliCelsius{temperature_mc};
  observation.source = tzm::SourceId::literal("synthetic-harness-04");
  observation.provenance = tzm::ProvenanceKind::SyntheticHarness;
  observation.observed_at = observed_at;
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

// Evaluates every declared zone and publishes the derating outcome. The
// published level only moves here: evaluate() itself is pure.
tzm::Result<tzm::EvaluationResult> evaluate_and_publish(tzm::ThermalZoneEngine& engine,
                                                        const tzm::Configuration& configuration,
                                                        tzm::ControlPlaneEpoch epoch,
                                                        Counters& counters) {
  tzm::EvaluationRequest request;
  request.id = tzm::EvaluationId::from_value(counters.evaluation++);
  request.epoch = epoch;
  request.configuration_generation = configuration.generation();
  request.evidence_generation = configuration.evidence_generation();

  const tzm::Result<tzm::EvaluationResult> evaluated = engine.evaluate(request);
  if (!evaluated.ok()) {
    return evaluated.error();
  }

  tzm::EvaluationCommitRequest commit;
  commit.command = tzm::CommandId::from_value(counters.command++);
  commit.attempt = tzm::AttemptId::from_value(counters.attempt++);
  commit.epoch = epoch;
  commit.evaluation = evaluated.value().id();
  commit.configuration_generation = evaluated.value().configuration_generation();
  commit.evidence_generation = evaluated.value().evidence_generation();
  commit.basis_revision = evaluated.value().basis_revision();

  const tzm::Result<tzm::EvaluationCommitReceipt> committed = engine.commit_evaluation(commit);
  if (!committed.ok()) {
    return committed.error();
  }
  return evaluated.value();
}

void print_step(unsigned step, std::int64_t temperature_mc, const tzm::ZoneThermalState& state) {
  std::cout << "  step " << step << ": observed " << tzm::format_milli(temperature_mc)
            << " C -> instant level " << state.instant_derate_level << "/"
            << state.derating_after.published_steps << ", published "
            << state.derating_after.published_level << "/"
            << state.derating_after.published_steps << ", hold "
            << state.derating_after.hold_count << ", band " << tzm::to_string(state.band)
            << ", escalated " << (state.derating_escalated ? "yes" : "no") << ", released "
            << (state.derating_released ? "yes" : "no") << ", held "
            << (state.derating_held_for_evidence ? "yes" : "no") << "\n";
}

}  // namespace

int main() {
  tzm::ManualClock clock{tzm::Timestamp{kClockStartNs}};

  tzm::EngineOptions options;
  options.actor = tzm::ActorId::literal("tzm-example-04");
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

  Counters counters;
  counters.command = 2;
  counters.attempt = 2;

  tzm::ConfigurationRequest declaration;
  declaration.command = tzm::CommandId::from_value(counters.command++);
  declaration.attempt = tzm::AttemptId::from_value(counters.attempt++);
  declaration.epoch = epoch.value().current;
  declaration.expected_generation = tzm::ConfigurationGeneration::zero();
  declaration.zones.push_back(make_zone());
  declaration.policy.coupling_hops = 1;
  declaration.policy.release_hold_observations = kReleaseHold;

  const tzm::Result<tzm::ConfigurationReceipt> applied =
      engine->apply_configuration(declaration);
  if (!applied.ok()) {
    std::cerr << applied.error().to_string() << "\n";
    return 1;
  }
  const tzm::Configuration configuration = engine->configuration();

  std::cout << "example 04: derating hysteresis\n";
  std::cout << "  input: synthetic harness observations, not real hardware evidence\n";
  std::cout << "  ladder: " << kDerateSteps << " step(s) over ["
            << tzm::format_milli(kDerateOnsetMc) << " C, " << tzm::format_milli(kCeilingMc)
            << " C], release margin " << tzm::format_milli(kRecoveryMarginMc)
            << " C, release hold " << kReleaseHold << " observation(s)\n";

  const std::int64_t temperatures[3] = {72000, 82000, 92000};
  unsigned step = 1;
  for (const std::int64_t temperature : temperatures) {
    if (!ingest(*engine, configuration, epoch.value().current, counters, temperature, clock.now())) {
      return 1;
    }
    const tzm::Result<tzm::EvaluationResult> evaluated =
        evaluate_and_publish(*engine, configuration, epoch.value().current, counters);
    if (!evaluated.ok()) {
      std::cerr << evaluated.error().to_string() << "\n";
      return 1;
    }
    print_step(step, temperature, evaluated.value().zones().front());
    ++step;
  }

  // A stale reading holds the published level and clears the recovery progress:
  // it can never release a step.
  clock.advance(tzm::Nanoseconds{120000000000LL});  // 120 s, past the 60 s validity
  const tzm::Result<tzm::EvaluationResult> stale =
      evaluate_and_publish(*engine, configuration, epoch.value().current, counters);
  if (!stale.ok()) {
    std::cerr << stale.error().to_string() << "\n";
    return 1;
  }
  const tzm::ZoneThermalState& held = stale.value().zones().front();
  std::cout << "  clock moved 120 s past the validity window with no new observation:\n";
  std::cout << "      evidence " << tzm::to_string(held.evidence_freshness) << ", held_for_evidence "
            << (held.derating_held_for_evidence ? "yes" : "no") << ", published level still "
            << held.derating_after.published_level << "/" << held.derating_after.published_steps
            << "\n";
  if (!held.derating_held_for_evidence ||
      held.derating_after.published_level != kDerateSteps) {
    std::cerr << "a stale observation changed the published derating level\n";
    return 1;
  }

  // Recovery: three consecutive usable observations at or below the release
  // threshold release exactly one step each time.
  const std::int64_t recovery_temperature = 78000;
  for (int index = 0; index < 7; ++index) {
    if (!ingest(*engine, configuration, epoch.value().current, counters, recovery_temperature,
                clock.now())) {
      return 1;
    }
    const tzm::Result<tzm::EvaluationResult> evaluated =
        evaluate_and_publish(*engine, configuration, epoch.value().current, counters);
    if (!evaluated.ok()) {
      std::cerr << evaluated.error().to_string() << "\n";
      return 1;
    }
    print_step(step, recovery_temperature, evaluated.value().zones().front());
    ++step;
  }

  // Escalation is immediate again.
  if (!ingest(*engine, configuration, epoch.value().current, counters, 88000, clock.now())) {
    return 1;
  }
  const tzm::Result<tzm::EvaluationResult> re_escalated =
      evaluate_and_publish(*engine, configuration, epoch.value().current, counters);
  if (!re_escalated.ok()) {
    std::cerr << re_escalated.error().to_string() << "\n";
    return 1;
  }
  print_step(step, 88000, re_escalated.value().zones().front());
  if (!re_escalated.value().zones().front().derating_escalated) {
    std::cerr << "a return above the ladder did not escalate immediately\n";
    return 1;
  }

  std::cout << "  escalation is immediate; release takes " << kReleaseHold
            << " consecutive usable observations below the release threshold, one ladder step "
               "at a time\n";
  return 0;
}
