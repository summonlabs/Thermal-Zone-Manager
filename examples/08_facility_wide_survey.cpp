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

// Example 08: a facility-wide survey in one evaluation pass.
//
// Twelve zones in a deterministic coupling ring with a second (skip) edge per
// zone and one cross link. One zone has never reported and is left with an
// undeclared heat load, so the zones it feeds cannot resolve their arriving
// heat: the survey shows all three headroom answers in one table.
//
// The observations are synthetic harness data. No real hardware is exercised.

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
constexpr std::int64_t kDeclaredHeatMw = 20000;
constexpr std::int64_t kValidityNs = 60000000000LL;
constexpr std::uint64_t kZoneCount = 12;
constexpr std::uint64_t kSilentZone = 12;   // never reported, heat not declared
constexpr std::uint64_t kCrossSink = 5;     // fed by the silent zone
constexpr std::int32_t kRingPpm = 80000;
constexpr std::int32_t kSkipPpm = 40000;
constexpr std::int32_t kCrossPpm = 50000;

std::string pad(const std::string& text, std::size_t width) {
  if (text.size() >= width) {
    return text;
  }
  return text + std::string(width - text.size(), ' ');
}

std::string allowance_text(const tzm::ZoneThermalState& state) {
  if (!state.allowance_known) {
    return "not proven";
  }
  return tzm::format_milli(state.placement_allowance.value) + " W";
}

}  // namespace

int main() {
  tzm::ManualClock clock{tzm::Timestamp{kClockStartNs}};

  tzm::EngineOptions options;
  options.actor = tzm::ActorId::literal("tzm-example-08");
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

  tzm::ConfigurationRequest declaration;
  declaration.command = tzm::CommandId::from_value(2);
  declaration.attempt = tzm::AttemptId::from_value(2);
  declaration.epoch = epoch.value().current;
  declaration.expected_generation = tzm::ConfigurationGeneration::zero();
  declaration.policy.coupling_hops = 1;

  for (std::uint64_t id = 1; id <= kZoneCount; ++id) {
    tzm::ZoneConfiguration zone;
    zone.id = tzm::ZoneId::from_value(id);
    zone.name = tzm::ZoneName::literal("zone-" + std::to_string(id));
    zone.envelope.floor_temp = tzm::MilliCelsius{kFloorMc};
    zone.envelope.derate_onset = tzm::MilliCelsius{kDerateOnsetMc};
    zone.envelope.ceiling_temp = tzm::MilliCelsius{kCeilingMc};
    zone.envelope.critical_temp = tzm::MilliCelsius{kCriticalMc};
    if (id != kSilentZone) {
      zone.declared_heat = tzm::MilliWatts{kDeclaredHeatMw};
    }
    zone.thermal_resistance = tzm::MicroKelvinPerWatt{kResistanceUkw};
    declaration.zones.push_back(zone);

    tzm::CouplingEdge ring;
    ring.source = tzm::ZoneId::from_value(id);
    ring.sink = tzm::ZoneId::from_value((id % kZoneCount) + 1);
    ring.coefficient = tzm::PartsPerMillion{kRingPpm};
    declaration.coupling.push_back(ring);

    tzm::CouplingEdge skip;
    skip.source = tzm::ZoneId::from_value(id);
    skip.sink = tzm::ZoneId::from_value(((id + 1) % kZoneCount) + 1);
    skip.coefficient = tzm::PartsPerMillion{kSkipPpm};
    declaration.coupling.push_back(skip);
  }

  tzm::CouplingEdge cross;
  cross.source = tzm::ZoneId::from_value(kSilentZone);
  cross.sink = tzm::ZoneId::from_value(kCrossSink);
  cross.coefficient = tzm::PartsPerMillion{kCrossPpm};
  declaration.coupling.push_back(cross);

  const tzm::Result<tzm::ConfigurationReceipt> applied =
      engine->apply_configuration(declaration);
  if (!applied.ok()) {
    std::cerr << applied.error().to_string() << "\n";
    return 1;
  }

  const tzm::Configuration configuration = engine->configuration();
  const tzm::Timestamp now = clock.now();
  for (std::uint64_t id = 1; id <= kZoneCount; ++id) {
    if (id == kSilentZone) {
      continue;  // this zone never reported
    }
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
    observation.temperature = tzm::MilliCelsius{45000 + 2500 * static_cast<std::int64_t>(id)};
    observation.source = tzm::SourceId::literal("synthetic-harness-08");
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
  const tzm::EvaluationResult& result = evaluated.value();

  std::cout << "example 08: facility-wide survey\n";
  std::cout << "  input: synthetic harness observations, not real hardware evidence\n";
  std::cout << "  facility: " << static_cast<unsigned long long>(kZoneCount) << " zone(s), "
            << static_cast<unsigned long long>(applied.value().coupling_edge_count)
            << " coupling edge(s), zone " << kSilentZone
            << " silent and undeclared\n";
  std::cout << "\n";
  std::cout << "  " << pad("zone", 6) << pad("name", 10) << pad("evidence", 12) << pad("band", 12)
            << pad("headroom", 15) << pad("constraint", 15) << "allowance\n";

  for (const tzm::ZoneThermalState& state : result.zones()) {
    std::cout << "  " << pad(tzm::to_string(state.zone), 6) << pad(state.name.str(), 10)
              << pad(std::string(tzm::to_string(state.evidence_freshness)), 12)
              << pad(state.band_known ? std::string(tzm::to_string(state.band))
                                      : std::string("unknown"), 12)
              << pad(std::string(tzm::to_string(state.headroom.status)), 15)
              << pad(std::string(tzm::to_string(state.constraint_kind)), 15)
              << allowance_text(state) << "\n";
  }

  const tzm::EvaluationSummary& summary = result.summary();
  std::cout << "\n";
  std::cout << "  summary: zones " << static_cast<unsigned long long>(summary.zones_total)
            << " (known " << static_cast<unsigned long long>(summary.zones_known) << ", unknown "
            << static_cast<unsigned long long>(summary.zones_unknown) << ", indeterminate "
            << static_cast<unsigned long long>(summary.zones_indeterminate) << ")\n";
  std::cout << "           constraints (bounded "
            << static_cast<unsigned long long>(summary.constraints_bounded) << ", prohibited "
            << static_cast<unsigned long long>(summary.constraints_prohibited)
            << ", indeterminate "
            << static_cast<unsigned long long>(summary.constraints_indeterminate) << ")\n";
  std::cout << "           usable evidence "
            << static_cast<unsigned long long>(summary.zones_with_usable_evidence)
            << ", zones with an unknown neighbour "
            << static_cast<unsigned long long>(summary.zones_with_unknown_neighbour) << "\n";
  std::cout << "  result digest: " << tzm::to_hex(result.digest()) << "\n";

  constexpr std::size_t kExpectedUnknown = 1;  // exactly the silent zone
  if (summary.zones_total != kZoneCount || summary.zones_unknown != kExpectedUnknown ||
      summary.zones_known == 0 || summary.zones_indeterminate == 0 ||
      summary.constraints_indeterminate !=
          summary.zones_unknown + summary.zones_indeterminate) {
    std::cerr << "the facility survey did not report the expected mixture\n";
    return 1;
  }
  return 0;
}
