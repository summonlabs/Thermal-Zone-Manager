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

// Example 03: unknown is not zero.
//
// Three ways of not knowing, side by side:
//   (a) no observation was ever delivered for the zone;
//   (b) an observation exists but its validity window has expired;
//   (c) a coupled neighbour has a non-zero coefficient and an undeclared heat
//       load, so the arriving heat cannot be resolved.
//
// In all three the headroom is not zero and the emitted constraint carries no
// proven allowance. Neither answer may be consumed as capacity, and neither may
// be consumed as safe.

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
constexpr std::int64_t kObservedMc = 50000;
constexpr std::int32_t kCouplingPpm = 400000;

struct World {
  std::unique_ptr<tzm::ThermalZoneEngine> engine;
  tzm::ControlPlaneEpoch epoch;
  tzm::Configuration configuration;
};

tzm::ZoneConfiguration make_zone(std::uint64_t id, const std::string& name, bool declare_heat) {
  tzm::ZoneConfiguration zone;
  zone.id = tzm::ZoneId::from_value(id);
  zone.name = tzm::ZoneName::literal(name);
  zone.envelope.floor_temp = tzm::MilliCelsius{kFloorMc};
  zone.envelope.derate_onset = tzm::MilliCelsius{kDerateOnsetMc};
  zone.envelope.ceiling_temp = tzm::MilliCelsius{kCeilingMc};
  zone.envelope.critical_temp = tzm::MilliCelsius{kCriticalMc};
  if (declare_heat) {
    zone.declared_heat = tzm::MilliWatts{20000};
  }
  zone.thermal_resistance = tzm::MicroKelvinPerWatt{kResistanceUkw};
  return zone;
}

bool open_world(tzm::ManualClock& clock, const char* actor,
                const std::vector<tzm::ZoneConfiguration>& zones,
                const std::vector<tzm::CouplingEdge>& coupling, World& world) {
  tzm::EngineOptions options;
  options.actor = tzm::ActorId::literal(actor);
  options.clock = &clock;

  tzm::Result<std::unique_ptr<tzm::ThermalZoneEngine>> opened = tzm::ThermalZoneEngine::open(options);
  if (!opened.ok()) {
    std::cerr << opened.error().to_string() << "\n";
    return false;
  }
  world.engine = std::move(opened.value());

  tzm::EpochRequest epoch_request;
  epoch_request.command = tzm::CommandId::first();
  epoch_request.attempt = tzm::AttemptId::first();
  epoch_request.expected_current = tzm::ControlPlaneEpoch::zero();
  epoch_request.target = tzm::ControlPlaneEpoch::first();
  const tzm::Result<tzm::EpochReceipt> epoch = world.engine->advance_epoch(epoch_request);
  if (!epoch.ok()) {
    std::cerr << epoch.error().to_string() << "\n";
    return false;
  }
  world.epoch = epoch.value().current;

  tzm::ConfigurationRequest declaration;
  declaration.command = tzm::CommandId::from_value(2);
  declaration.attempt = tzm::AttemptId::from_value(2);
  declaration.epoch = world.epoch;
  declaration.expected_generation = tzm::ConfigurationGeneration::zero();
  declaration.zones = zones;
  declaration.coupling = coupling;
  declaration.policy.coupling_hops = 1;
  const tzm::Result<tzm::ConfigurationReceipt> applied =
      world.engine->apply_configuration(declaration);
  if (!applied.ok()) {
    std::cerr << applied.error().to_string() << "\n";
    return false;
  }
  world.configuration = world.engine->configuration();
  return true;
}

bool ingest_fresh(tzm::ThermalZoneEngine& engine, const tzm::Configuration& configuration,
                  tzm::ControlPlaneEpoch epoch, std::uint64_t zone_value, std::int64_t temperature_mc,
                  tzm::Timestamp observed_at, tzm::Nanoseconds validity) {
  const tzm::ZoneId zone_id = tzm::ZoneId::from_value(zone_value);
  const tzm::ZoneConfiguration* declared = configuration.find(zone_id);
  if (declared == nullptr) {
    std::cerr << "the declared zone is missing from the configuration\n";
    return false;
  }
  tzm::TemperatureObservation observation;
  observation.zone = zone_id;
  observation.zone_generation = declared->generation;
  observation.evidence_generation = configuration.evidence_generation();
  observation.sequence = tzm::ObservationSequence::first();
  observation.temperature = tzm::MilliCelsius{temperature_mc};
  observation.source = tzm::SourceId::literal("synthetic-harness-03");
  observation.provenance = tzm::ProvenanceKind::SyntheticHarness;
  observation.observed_at = observed_at;
  observation.validity = validity;
  observation.publisher_epoch = epoch;
  observation.publisher_incarnation = engine.incarnation();
  const tzm::Result<tzm::ObservationReceipt> ingested = engine.ingest_observation(observation);
  if (!ingested.ok()) {
    std::cerr << ingested.error().to_string() << "\n";
    return false;
  }
  return true;
}

tzm::Result<tzm::EvaluationResult> evaluate_all(tzm::ThermalZoneEngine& engine,
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

// One row of the comparison, printed the same way for all three cases.
void print_row(const char* label, const tzm::ZoneThermalState& state) {
  std::cout << "  " << label << "\n";
  std::cout << "      evidence freshness : " << tzm::to_string(state.evidence_freshness) << "\n";
  std::cout << "      effective temp known: " << (state.effective_temperature_known ? "yes" : "no")
            << "\n";
  std::cout << "      headroom status    : " << tzm::to_string(state.headroom.status) << " (proven "
            << (tzm::is_proven(state.headroom.status) ? "yes" : "no") << ")\n";
  std::cout << "      constraint kind    : " << tzm::to_string(state.constraint_kind) << "\n";
  std::cout << "      allowance known    : " << (state.allowance_known ? "yes" : "no")
            << " (the milliwatt field reads " << tzm::format_milli(state.placement_allowance.value)
            << " W but it is not a capacity)\n";
  if (state.unknown_neighbour_count > 0) {
    std::cout << "      unknown neighbours : "
              << static_cast<unsigned long long>(state.unknown_neighbour_count) << "\n";
  }
}

}  // namespace

int main() {
  std::cout << "example 03: unknown is not zero\n";
  std::cout << "  input: synthetic harness observations, not real hardware evidence\n";

  // (a) nothing was ever observed for the zone.
  {
    tzm::ManualClock clock{tzm::Timestamp{kClockStartNs}};
    World world;
    if (!open_world(clock, "tzm-example-03a", {make_zone(1, "cpu-bay", true)}, {}, world)) {
      return 1;
    }
    const tzm::Result<tzm::EvaluationResult> evaluated =
        evaluate_all(*world.engine, world.configuration, world.epoch, 1);
    if (!evaluated.ok()) {
      std::cerr << evaluated.error().to_string() << "\n";
      return 1;
    }
    print_row("(a) no observation was ever delivered", evaluated.value().zones().front());
  }

  // (b) an observation exists, but the manual clock moved past its validity.
  {
    tzm::ManualClock clock{tzm::Timestamp{kClockStartNs}};
    World world;
    if (!open_world(clock, "tzm-example-03b", {make_zone(1, "cpu-bay", true)}, {}, world)) {
      return 1;
    }
    if (!ingest_fresh(*world.engine, world.configuration, world.epoch, 1, kObservedMc, clock.now(),
                      tzm::Nanoseconds{1000000000LL})) {
      return 1;
    }
    clock.advance(tzm::Nanoseconds{2000000000LL});  // 2 s, past the 1 s validity window
    const tzm::Result<tzm::EvaluationResult> evaluated =
        evaluate_all(*world.engine, world.configuration, world.epoch, 2);
    if (!evaluated.ok()) {
      std::cerr << evaluated.error().to_string() << "\n";
      return 1;
    }
    print_row("(b) the held observation expired 1 s after it was taken",
              evaluated.value().zones().front());
  }

  // (c) a coupled neighbour with a non-zero coefficient and an undeclared heat
  //     load: the arriving heat cannot be resolved.
  {
    tzm::ManualClock clock{tzm::Timestamp{kClockStartNs}};
    World world;
    tzm::CouplingEdge edge;
    edge.source = tzm::ZoneId::from_value(2);
    edge.sink = tzm::ZoneId::from_value(1);
    edge.coefficient = tzm::PartsPerMillion{kCouplingPpm};
    if (!open_world(clock, "tzm-example-03c",
                    {make_zone(1, "cpu-bay", true), make_zone(2, "gpu-bay", false)}, {edge},
                    world)) {
      return 1;
    }
    if (!ingest_fresh(*world.engine, world.configuration, world.epoch, 1, kObservedMc, clock.now(),
                      tzm::Nanoseconds{60000000000LL})) {
      return 1;
    }
    const tzm::Result<tzm::EvaluationResult> evaluated =
        evaluate_all(*world.engine, world.configuration, world.epoch, 3);
    if (!evaluated.ok()) {
      std::cerr << evaluated.error().to_string() << "\n";
      return 1;
    }
    const tzm::ZoneThermalState* sink = evaluated.value().find(tzm::ZoneId::from_value(1));
    const tzm::ZoneThermalState* neighbour = evaluated.value().find(tzm::ZoneId::from_value(2));
    if (sink == nullptr || neighbour == nullptr) {
      std::cerr << "the evaluation did not cover every declared zone\n";
      return 1;
    }
    print_row("(c) zone 1 is fresh, but zone 2 heat is undeclared", *sink);
    std::cout << "      zone 2 (the neighbour) reports "
              << tzm::to_string(neighbour->evidence_freshness) << " evidence and heat_known "
              << (neighbour->heat_known ? "yes" : "no") << "\n";
    if (sink->headroom.status != tzm::HeadroomStatus::Indeterminate ||
        sink->constraint_kind != tzm::PlacementConstraintKind::Indeterminate ||
        sink->allowance_known) {
      std::cerr << "an unresolvable neighbour contribution was reported as a proven allowance\n";
      return 1;
    }
  }

  std::cout << "  neither answer is zero: 'unknown' is not 0 mW of remaining capacity and\n";
  std::cout << "  'indeterminate' is not 0 mW either; allowance_known is false for both, and\n";
  std::cout << "  neither may be consumed as capacity or treated as safe.\n";
  return 0;
}
