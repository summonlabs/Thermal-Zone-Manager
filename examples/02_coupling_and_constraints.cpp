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

// Example 02: coupling, and what a placement constraint is not.
//
// Three zones. Zone 2 dissipates heat that is transported into zone 1, so zone 1
// runs hotter than its own sensor says and its allowance shrinks. Zone 3 is
// declared and observed exactly like zone 1 but has no hot neighbour, which is
// what makes the cost of the coupling visible.
//
// The constraints printed here are emitted advice. This runtime never selects a
// location, never reserves capacity and never actuates anything.

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
constexpr std::int64_t kQuietHeatMw = 20000;
constexpr std::int64_t kHotHeatMw = 50000;
constexpr std::int64_t kQuietTempMc = 55000;
constexpr std::int64_t kHotTempMc = 40000;
constexpr std::int32_t kCouplingPpm = 400000;  // 0.400 of zone 2 heat reaches zone 1
constexpr std::int64_t kValidityNs = 60000000000LL;

std::string format_celsius(std::int64_t value) { return tzm::format_milli(value) + " C"; }
std::string format_watts(std::int64_t value) { return tzm::format_milli(value) + " W"; }

std::string reason_list(const std::vector<tzm::ConstraintReason>& reasons) {
  std::string text;
  for (std::size_t index = 0; index < reasons.size(); ++index) {
    if (index != 0) {
      text += ", ";
    }
    text += tzm::to_string(reasons[index]);
  }
  if (text.empty()) {
    text = "none";
  }
  return text;
}

}  // namespace

int main() {
  tzm::ManualClock clock{tzm::Timestamp{kClockStartNs}};

  tzm::EngineOptions options;
  options.actor = tzm::ActorId::literal("tzm-example-02");
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

  const auto make_zone = [](std::uint64_t id, const std::string& name, std::int64_t heat_mw) {
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
  };

  tzm::ConfigurationRequest declaration;
  declaration.command = tzm::CommandId::from_value(2);
  declaration.attempt = tzm::AttemptId::from_value(2);
  declaration.epoch = epoch.value().current;
  declaration.expected_generation = tzm::ConfigurationGeneration::zero();
  declaration.zones.push_back(make_zone(1, "cpu-bay", kQuietHeatMw));
  declaration.zones.push_back(make_zone(2, "gpu-bay", kHotHeatMw));
  declaration.zones.push_back(make_zone(3, "nic-bay", kQuietHeatMw));
  declaration.policy.coupling_hops = 1;

  tzm::CouplingEdge edge;
  edge.source = tzm::ZoneId::from_value(2);
  edge.sink = tzm::ZoneId::from_value(1);
  edge.coefficient = tzm::PartsPerMillion{kCouplingPpm};
  declaration.coupling.push_back(edge);

  const tzm::Result<tzm::ConfigurationReceipt> applied =
      engine->apply_configuration(declaration);
  if (!applied.ok()) {
    std::cerr << applied.error().to_string() << "\n";
    return 1;
  }

  const tzm::Configuration configuration = engine->configuration();
  const tzm::Timestamp now = clock.now();
  for (std::uint64_t id = 1; id <= 3; ++id) {
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
    observation.temperature = tzm::MilliCelsius{id == 2 ? kHotTempMc : kQuietTempMc};
    observation.source = tzm::SourceId::literal("synthetic-harness-02");
    observation.provenance = tzm::ProvenanceKind::SyntheticHarness;
    observation.observed_at = now;
    observation.validity = tzm::Nanoseconds{kValidityNs};
    observation.publisher_epoch = epoch.value().current;
    observation.publisher_incarnation = engine->incarnation();
    const tzm::Result<tzm::ObservationReceipt> ingested = engine->ingest_observation(observation);
    if (!ingested.ok()) {
      std::cerr << ingested.error().to_string() << "\n";
      return 1;
    }
  }

  tzm::EvaluationRequest request;
  request.id = tzm::EvaluationId::first();
  request.epoch = epoch.value().current;
  request.configuration_generation = configuration.generation();
  request.evidence_generation = configuration.evidence_generation();

  const tzm::Result<tzm::EvaluationResult> evaluated = engine->evaluate(request);
  if (!evaluated.ok()) {
    std::cerr << evaluated.error().to_string() << "\n";
    return 1;
  }

  const tzm::ZoneThermalState* sink = evaluated.value().find(tzm::ZoneId::from_value(1));
  const tzm::ZoneThermalState* source = evaluated.value().find(tzm::ZoneId::from_value(2));
  const tzm::ZoneThermalState* quiet = evaluated.value().find(tzm::ZoneId::from_value(3));
  const tzm::PlacementConstraint* sink_constraint =
      evaluated.value().constraint_for(tzm::ZoneId::from_value(1));
  if (sink == nullptr || source == nullptr || quiet == nullptr || sink_constraint == nullptr) {
    std::cerr << "the evaluation did not cover every declared zone\n";
    return 1;
  }

  const std::int64_t cost_mw =
      quiet->headroom.power_margin.value - sink->headroom.power_margin.value;

  std::cout << "example 02: coupling and constraints\n";
  std::cout << "  input          : synthetic harness observations, not real hardware evidence\n";
  std::cout << "  coupling       : zone 2 -> zone 1 at " << kCouplingPpm
            << " ppm, " << evaluated.value().zones().front().coupling_hops_used << " hop(s) used\n";
  std::cout << "  zone 2 heat    : " << format_watts(source->heat.value) << " ("
            << tzm::to_string(source->heat_source) << "), observed "
            << format_celsius(source->observed_temperature.value) << "\n";
  std::cout << "  zone 1 arriving: " << format_watts(sink->coupled_heat.value) << ", rise "
            << format_celsius(sink->coupled_rise.value) << "\n";
  std::cout << "  zone 1 effective temperature: "
            << format_celsius(sink->effective_temperature.value) << " (observed "
            << format_celsius(sink->observed_temperature.value) << ")\n";
  std::cout << "  zone 1 band    : " << tzm::to_string(sink->band) << ", headroom "
            << tzm::to_string(sink->headroom.status) << "\n";
  std::cout << "  zone 1 allowance: " << format_watts(sink_constraint->max_additional_heat.value)
            << ", zone 3 allowance: " << format_watts(quiet->placement_allowance.value) << "\n";
  std::cout << "  neighbour cost : zone 1 loses exactly " << format_watts(cost_mw)
            << " of allowance to zone 2 heat\n";
  std::cout << "  zone 1 reasons : " << reason_list(sink->reasons) << "\n";
  std::cout << "  explanation    : " << sink_constraint->explanation() << "\n";
  std::cout << "  advice only    : this is advisory output for the placement planner; nothing "
               "was placed, reserved or actuated\n";

  if (sink->headroom.status != tzm::HeadroomStatus::Known ||
      sink->coupled_heat.value <= 0 || cost_mw <= 0 ||
      sink_constraint->kind != tzm::PlacementConstraintKind::Bounded) {
    std::cerr << "the coupled evaluation did not reduce the sink allowance as expected\n";
    return 1;
  }
  return 0;
}
