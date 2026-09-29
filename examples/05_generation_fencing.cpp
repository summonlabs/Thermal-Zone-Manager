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

// Example 05: generation and epoch fencing.
//
// Every observation is stamped with the zone generation, the evidence
// generation and the control-plane epoch it was taken under. All three are
// checked when the observation is handed in, and a mismatch is a refusal with a
// machine-readable code rather than a silent downgrade.
//
// Three refusals are shown, each followed by one correctly stamped observation
// that is accepted, so the fence - not the payload - is what refused.

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

tzm::ZoneConfiguration make_zone(std::int64_t resistance_ukw) {
  tzm::ZoneConfiguration zone;
  zone.id = tzm::ZoneId::first();
  zone.name = tzm::ZoneName::literal("cpu-bay");
  zone.envelope.floor_temp = tzm::MilliCelsius{kFloorMc};
  zone.envelope.derate_onset = tzm::MilliCelsius{kDerateOnsetMc};
  zone.envelope.ceiling_temp = tzm::MilliCelsius{kCeilingMc};
  zone.envelope.critical_temp = tzm::MilliCelsius{kCriticalMc};
  zone.declared_heat = tzm::MilliWatts{2000};
  zone.thermal_resistance = tzm::MicroKelvinPerWatt{resistance_ukw};
  return zone;
}

// A complete, otherwise valid observation whose stamp the caller chooses.
tzm::TemperatureObservation stamped(const tzm::ThermalZoneEngine& engine, tzm::ZoneId zone,
                                    tzm::ZoneGeneration zone_generation,
                                    tzm::EvidenceGeneration evidence_generation,
                                    tzm::ControlPlaneEpoch epoch, std::uint64_t sequence_value,
                                    tzm::Timestamp observed_at) {
  tzm::TemperatureObservation observation;
  observation.zone = zone;
  observation.zone_generation = zone_generation;
  observation.evidence_generation = evidence_generation;
  observation.sequence = tzm::ObservationSequence::from_value(sequence_value);
  observation.temperature = tzm::MilliCelsius{50000};
  observation.source = tzm::SourceId::literal("synthetic-harness-05");
  observation.provenance = tzm::ProvenanceKind::SyntheticHarness;
  observation.observed_at = observed_at;
  observation.validity = tzm::Nanoseconds{60000000000LL};
  observation.publisher_epoch = epoch;
  observation.publisher_incarnation = engine.incarnation();
  return observation;
}

// Prints the refusal with its exact code name and returns true when the code is
// the one the fence is expected to report.
bool expect_refusal(const char* label, tzm::ErrorCode expected,
                    const tzm::Result<tzm::ObservationReceipt>& receipt) {
  if (receipt.ok()) {
    std::cout << "  " << label << ": accepted (unexpected)\n";
    return false;
  }
  const tzm::Error& error = receipt.error();
  std::cout << "  " << label << " refused: " << tzm::to_string(error.code());
  std::cout << (error.code() == expected ? " (expected)" : " (UNEXPECTED)") << "\n";
  std::cout << "      message: " << error.message() << "\n";
  return error.code() == expected;
}

}  // namespace

int main() {
  tzm::ManualClock clock{tzm::Timestamp{kClockStartNs}};

  tzm::EngineOptions options;
  options.actor = tzm::ActorId::literal("tzm-example-05");
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
  const tzm::ControlPlaneEpoch first_epoch = epoch.value().current;

  // The first declaration.
  tzm::ConfigurationRequest first_declaration;
  first_declaration.command = tzm::CommandId::from_value(2);
  first_declaration.attempt = tzm::AttemptId::from_value(2);
  first_declaration.epoch = first_epoch;
  first_declaration.expected_generation = tzm::ConfigurationGeneration::zero();
  first_declaration.zones.push_back(make_zone(400000));
  first_declaration.policy.coupling_hops = 1;
  const tzm::Result<tzm::ConfigurationReceipt> first_applied =
      engine->apply_configuration(first_declaration);
  if (!first_applied.ok()) {
    std::cerr << first_applied.error().to_string() << "\n";
    return 1;
  }
  const tzm::Configuration first_configuration = engine->configuration();
  const tzm::ZoneConfiguration* first_zone = first_configuration.find(tzm::ZoneId::first());
  if (first_zone == nullptr) {
    std::cerr << "the declared zone is missing from the configuration\n";
    return 1;
  }
  const tzm::ZoneGeneration first_zone_generation = first_zone->generation;
  const tzm::EvidenceGeneration first_evidence_generation =
      first_configuration.evidence_generation();

  // The declaration changes, so the zone generation and the evidence generation
  // both move on.
  tzm::ConfigurationRequest second_declaration;
  second_declaration.command = tzm::CommandId::from_value(3);
  second_declaration.attempt = tzm::AttemptId::from_value(3);
  second_declaration.epoch = first_epoch;
  second_declaration.expected_generation = first_configuration.generation();
  second_declaration.zones.push_back(make_zone(500000));
  second_declaration.policy.coupling_hops = 1;
  const tzm::Result<tzm::ConfigurationReceipt> second_applied =
      engine->apply_configuration(second_declaration);
  if (!second_applied.ok()) {
    std::cerr << second_applied.error().to_string() << "\n";
    return 1;
  }
  const tzm::Configuration configuration = engine->configuration();
  const tzm::ZoneConfiguration* zone = configuration.find(tzm::ZoneId::first());
  if (zone == nullptr) {
    std::cerr << "the declared zone is missing from the configuration\n";
    return 1;
  }
  const tzm::ZoneGeneration current_zone_generation = zone->generation;
  const tzm::EvidenceGeneration current_evidence_generation =
      configuration.evidence_generation();

  std::cout << "example 05: generation fencing\n";
  std::cout << "  input: synthetic harness observations, not real hardware evidence\n";
  std::cout << "  configuration replacement moved zone generation "
            << tzm::to_string(first_zone_generation) << " -> "
            << tzm::to_string(current_zone_generation) << " and evidence generation "
            << tzm::to_string(first_evidence_generation) << " -> "
            << tzm::to_string(current_evidence_generation) << "\n";

  bool all_refused = true;

  // (a) a zone generation that the current declaration superseded.
  all_refused = expect_refusal(
      "stale zone generation",
      tzm::ErrorCode::StaleZoneGeneration,
      engine->ingest_observation(stamped(*engine, tzm::ZoneId::first(), first_zone_generation,
                                         current_evidence_generation, first_epoch, 1,
                                         clock.now()))) && all_refused;

  // (b) an evidence generation that the replacement superseded.
  all_refused = expect_refusal(
      "stale evidence generation",
      tzm::ErrorCode::StaleEvidenceGeneration,
      engine->ingest_observation(stamped(*engine, tzm::ZoneId::first(), current_zone_generation,
                                         first_evidence_generation, first_epoch, 2, clock.now()))) && all_refused;

  // The control plane moves to a second epoch. Everything stamped with the
  // first epoch is fenced from here on.
  tzm::EpochRequest second_epoch_request;
  second_epoch_request.command = tzm::CommandId::from_value(4);
  second_epoch_request.attempt = tzm::AttemptId::from_value(4);
  second_epoch_request.expected_current = first_epoch;
  second_epoch_request.target = tzm::ControlPlaneEpoch::from_value(2);
  const tzm::Result<tzm::EpochReceipt> second_epoch = engine->advance_epoch(second_epoch_request);
  if (!second_epoch.ok()) {
    std::cerr << second_epoch.error().to_string() << "\n";
    return 1;
  }
  const tzm::ControlPlaneEpoch current_epoch = second_epoch.value().current;
  std::cout << "  control-plane epoch moved " << tzm::to_string(first_epoch) << " -> "
            << tzm::to_string(current_epoch) << "\n";

  // (c) an observation published under the previous epoch.
  all_refused = expect_refusal(
      "epoch mismatch",
      tzm::ErrorCode::EpochMismatch,
      engine->ingest_observation(stamped(*engine, tzm::ZoneId::first(), current_zone_generation,
                                         current_evidence_generation, first_epoch, 3,
                                         clock.now()))) && all_refused;

  // The control: everything stamped with the live state is accepted.
  const tzm::Result<tzm::ObservationReceipt> accepted = engine->ingest_observation(
      stamped(*engine, tzm::ZoneId::first(), current_zone_generation, current_evidence_generation,
              current_epoch, 4, clock.now()));
  if (!accepted.ok()) {
    std::cerr << accepted.error().to_string() << "\n";
    return 1;
  }
  std::cout << "  correctly stamped observation accepted: zone "
            << tzm::to_string(accepted.value().zone) << " sequence "
            << tzm::to_string(accepted.value().sequence) << "\n";

  if (!all_refused) {
    std::cerr << "a fence did not refuse the stamp it was expected to refuse\n";
    return 1;
  }
  std::cout << "  every stale stamp was refused with its own code; nothing was downgraded or "
               "silently re-based\n";
  return 0;
}
