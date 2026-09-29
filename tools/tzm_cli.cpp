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

// tzm - the Thermal Zone Manager command line.
//
// A thin, non-interactive shell over the public API. Every command opens an
// engine, declares a world, hands it evidence and prints what the library
// decided. Nothing here places, reserves, ranks or actuates anything, and the
// observations it feeds in are labelled as synthetic harness data.

#include <thermal_zone_manager/thermal_zone_manager.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tzm = thermal_zone_manager;

namespace {

// A deliberately boring envelope shared by every command of this binary.
constexpr std::int64_t kFloorMc = 15000;         // 15.000 C
constexpr std::int64_t kDerateOnsetMc = 70000;   // 70.000 C
constexpr std::int64_t kCeilingMc = 90000;       // 90.000 C
constexpr std::int64_t kCriticalMc = 100000;     // 100.000 C
constexpr std::int64_t kResistanceUkw = 400000;  // uK/W
constexpr std::int64_t kDeclaredHeatMw = 20000;  // mW
constexpr std::int64_t kValidityNs = 60000000000LL;  // 60 s
constexpr std::uint64_t kFacilityZones = 4;

void print_usage(std::ostream& out) {
  out << "tzm - Thermal Zone Manager command line\n";
  out << "\n";
  out << "usage: tzm <command> [options]\n";
  out << "\n";
  out << "commands:\n";
  out << "  self-check              in-memory smoke path on an ephemeral engine\n";
  out << "  inspect --store <dir>   read-only inspection of an existing durable store\n";
  out << "  scenario --store <dir>  declare a fixed four-zone facility, evaluate, commit\n";
  out << "  --help                  print this help and exit 0\n";
  out << "  --version               print the library version and build information\n";
  out << "\n";
  out << "options:\n";
  out << "  --store <dir>           directory that holds thermal-zones.tzm\n";
}

// Monotone command, attempt and evaluation identities. The starting value is
// derived from the live store revision, so a second run against the same store
// can never reuse an identifier the previous run persisted.
class IdSource {
 public:
  explicit IdSource(std::uint64_t start)
      : next_command_(start == 0 ? 1 : start), next_evaluation_(start == 0 ? 1 : start) {}

  tzm::CommandId command() { return tzm::CommandId::from_value(next_command_++); }
  tzm::AttemptId attempt() { return tzm::AttemptId::from_value(next_attempt_++); }
  tzm::EvaluationId evaluation() { return tzm::EvaluationId::from_value(next_evaluation_++); }

 private:
  std::uint64_t next_command_ = 1;
  std::uint64_t next_attempt_ = 1;
  std::uint64_t next_evaluation_ = 1;
};

int refuse(const tzm::Error& error) {
  std::cerr << error.to_string() << "\n";
  return 2;
}

// Establishes a first control-plane epoch when the engine still carries epoch
// zero, and otherwise reports the epoch the store recovered. Every later
// request must carry the returned epoch.
bool ensure_epoch(tzm::ThermalZoneEngine& engine, std::uint64_t command_value,
                  tzm::ControlPlaneEpoch& epoch_out) {
  const tzm::FencingState fencing = engine.fencing();
  if (!fencing.epoch.is_zero()) {
    epoch_out = fencing.epoch;
    return true;
  }
  tzm::EpochRequest request;
  request.command = tzm::CommandId::from_value(command_value);
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

// The zone generation is assigned by the engine on apply_configuration, so a
// request only declares the shape of the world.
tzm::ZoneConfiguration make_zone(std::uint64_t id, const std::string& name, bool declare_heat) {
  tzm::ZoneConfiguration zone;
  zone.id = tzm::ZoneId::from_value(id);
  zone.name = tzm::ZoneName::literal(name);
  zone.envelope.floor_temp = tzm::MilliCelsius{kFloorMc};
  zone.envelope.derate_onset = tzm::MilliCelsius{kDerateOnsetMc};
  zone.envelope.ceiling_temp = tzm::MilliCelsius{kCeilingMc};
  zone.envelope.critical_temp = tzm::MilliCelsius{kCriticalMc};
  if (declare_heat) {
    zone.declared_heat = tzm::MilliWatts{kDeclaredHeatMw};
  }
  zone.thermal_resistance = tzm::MicroKelvinPerWatt{kResistanceUkw};
  zone.derate_steps = 8;
  zone.recovery_margin = tzm::MilliCelsius{1000};
  return zone;
}

// A synthetic observation stamped with the current zone generation, evidence
// generation and control-plane epoch of the engine. Nothing here is real
// hardware evidence, and the provenance says so.
tzm::TemperatureObservation make_observation(const tzm::ThermalZoneEngine& engine,
                                             const tzm::Configuration& configuration,
                                             tzm::ZoneId zone, tzm::ObservationSequence sequence,
                                             std::int64_t temperature_mc,
                                             tzm::Timestamp observed_at) {
  tzm::TemperatureObservation observation;
  observation.zone = zone;
  const tzm::ZoneConfiguration* declared = configuration.find(zone);
  if (declared != nullptr) {
    observation.zone_generation = declared->generation;
  }
  observation.evidence_generation = configuration.evidence_generation();
  observation.sequence = sequence;
  observation.temperature = tzm::MilliCelsius{temperature_mc};
  observation.source = tzm::SourceId::literal("tzm-cli-synthetic-harness");
  observation.provenance = tzm::ProvenanceKind::SyntheticHarness;
  observation.observed_at = observed_at;
  observation.validity = tzm::Nanoseconds{kValidityNs};
  observation.publisher_epoch = engine.fencing().epoch;
  observation.publisher_incarnation = engine.incarnation();
  return observation;
}

tzm::EvaluationRequest make_evaluation_request(tzm::EvaluationId id,
                                               const tzm::Configuration& configuration,
                                               tzm::ControlPlaneEpoch epoch) {
  tzm::EvaluationRequest request;
  request.id = id;
  request.epoch = epoch;
  request.configuration_generation = configuration.generation();
  request.evidence_generation = configuration.evidence_generation();
  return request;
}

// The next per-zone observation sequence, so a repeated run never reuses a
// sequence that the durable state already holds.
tzm::ObservationSequence next_sequence(const tzm::ThermalZoneEngine& engine, tzm::ZoneId zone) {
  const tzm::ZoneEvidence held = engine.evidence_for(zone);
  if (held.present) {
    return held.observation.sequence.next();
  }
  return tzm::ObservationSequence::first();
}

int run_self_check() {
  tzm::EngineOptions options;
  options.actor = tzm::ActorId::literal("tzm-cli-self-check");
  tzm::Result<std::unique_ptr<tzm::ThermalZoneEngine>> opened = tzm::ThermalZoneEngine::open(options);
  if (!opened.ok()) {
    return refuse(opened.error());
  }
  const std::unique_ptr<tzm::ThermalZoneEngine> engine = std::move(opened.value());

  tzm::ControlPlaneEpoch epoch;
  if (!ensure_epoch(*engine, 1, epoch)) {
    return 2;
  }
  IdSource ids(engine->fencing().revision.raw() + 1);

  std::vector<tzm::ZoneConfiguration> zones;
  zones.push_back(make_zone(1, "cpu-bay", true));
  zones.push_back(make_zone(2, "psu-bay", true));
  std::vector<tzm::CouplingEdge> coupling;
  tzm::CouplingEdge edge;
  edge.source = tzm::ZoneId::from_value(2);
  edge.sink = tzm::ZoneId::from_value(1);
  edge.coefficient = tzm::PartsPerMillion{400000};
  coupling.push_back(edge);

  tzm::ConfigurationRequest declaration;
  declaration.command = ids.command();
  declaration.attempt = ids.attempt();
  declaration.epoch = epoch;
  declaration.expected_generation = tzm::ConfigurationGeneration::zero();
  declaration.zones = zones;
  declaration.coupling = coupling;
  declaration.policy.coupling_hops = 1;
  declaration.policy.release_hold_observations = 3;

  const tzm::Result<tzm::ConfigurationReceipt> applied =
      engine->apply_configuration(declaration);
  if (!applied.ok()) {
    return refuse(applied.error());
  }
  std::cout << tzm::render(applied.value());

  const tzm::Configuration configuration = engine->configuration();
  const tzm::Timestamp now = engine->clock().now();
  for (std::uint64_t id = 1; id <= 2; ++id) {
    const tzm::ZoneId zone = tzm::ZoneId::from_value(id);
    const std::int64_t temperature = 40000 + 3000 * static_cast<std::int64_t>(id);
    const tzm::Result<tzm::ObservationReceipt> ingested = engine->ingest_observation(
        make_observation(*engine, configuration, zone, next_sequence(*engine, zone), temperature,
                         now));
    if (!ingested.ok()) {
      return refuse(ingested.error());
    }
    std::cout << tzm::render(ingested.value());
  }

  const tzm::Result<tzm::EvaluationResult> evaluated =
      engine->evaluate(make_evaluation_request(ids.evaluation(), configuration, epoch));
  if (!evaluated.ok()) {
    return refuse(evaluated.error());
  }
  std::cout << tzm::render(evaluated.value());

  tzm::EvaluationCommitRequest commit;
  commit.command = ids.command();
  commit.attempt = ids.attempt();
  commit.epoch = epoch;
  commit.evaluation = evaluated.value().id();
  commit.configuration_generation = evaluated.value().configuration_generation();
  commit.evidence_generation = evaluated.value().evidence_generation();
  commit.basis_revision = evaluated.value().basis_revision();
  const tzm::Result<tzm::EvaluationCommitReceipt> committed = engine->commit_evaluation(commit);
  if (!committed.ok()) {
    return refuse(committed.error());
  }
  std::cout << tzm::render(committed.value());

  const tzm::Status closed = engine->close();
  if (!closed.ok()) {
    return refuse(closed.error());
  }
  std::cout << "self-check: ok\n";
  return 0;
}

int run_inspect(const std::string& store_root) {
  tzm::EngineOptions options;
  options.root = store_root;
  options.access = tzm::StoreAccess::ReadOnly;
  options.create_if_missing = false;
  options.actor = tzm::ActorId::literal("tzm-cli-inspect");

  tzm::Result<std::unique_ptr<tzm::ThermalZoneEngine>> opened = tzm::ThermalZoneEngine::open(options);
  if (!opened.ok()) {
    return refuse(opened.error());
  }
  const std::unique_ptr<tzm::ThermalZoneEngine>& engine = opened.value();
  std::cout << engine->describe();
  std::cout << tzm::render(engine->configuration());
  std::cout << tzm::render(engine->fencing());
  std::cout << tzm::render(engine->recovery_report());
  return 0;
}

int run_scenario(const std::string& store_root) {
  tzm::EngineOptions options;
  options.root = store_root;
  options.access = tzm::StoreAccess::ReadWrite;
  options.create_if_missing = true;
  options.actor = tzm::ActorId::literal("tzm-cli-scenario");

  tzm::Result<std::unique_ptr<tzm::ThermalZoneEngine>> opened = tzm::ThermalZoneEngine::open(options);
  if (!opened.ok()) {
    return refuse(opened.error());
  }
  const std::unique_ptr<tzm::ThermalZoneEngine> engine = std::move(opened.value());

  // Command identifier 1 is reserved for the first epoch of a brand new store.
  tzm::ControlPlaneEpoch epoch;
  if (!ensure_epoch(*engine, 1, epoch)) {
    return 2;
  }
  IdSource ids(engine->fencing().revision.raw() + 1);

  // A fixed facility: a four-zone coupling ring plus one cross link. The
  // declaration is identical on every run, so a second run keeps every zone
  // generation and only advances the configuration and evidence generations.
  std::vector<tzm::ZoneConfiguration> zones;
  for (std::uint64_t id = 1; id <= kFacilityZones; ++id) {
    zones.push_back(make_zone(id, "rack-" + std::to_string(id), true));
  }
  std::vector<tzm::CouplingEdge> coupling;
  for (std::uint64_t id = 1; id <= kFacilityZones; ++id) {
    tzm::CouplingEdge edge;
    edge.source = tzm::ZoneId::from_value(id);
    edge.sink = tzm::ZoneId::from_value((id % kFacilityZones) + 1);
    edge.coefficient = tzm::PartsPerMillion{150000};
    coupling.push_back(edge);
  }
  tzm::CouplingEdge cross;
  cross.source = tzm::ZoneId::from_value(1);
  cross.sink = tzm::ZoneId::from_value(3);
  cross.coefficient = tzm::PartsPerMillion{150000};
  coupling.push_back(cross);

  tzm::ConfigurationRequest declaration;
  declaration.command = ids.command();
  declaration.attempt = ids.attempt();
  declaration.epoch = epoch;
  declaration.expected_generation = engine->configuration().generation();
  declaration.zones = zones;
  declaration.coupling = coupling;
  declaration.policy.coupling_hops = 2;
  declaration.policy.release_hold_observations = 3;

  const tzm::Result<tzm::ConfigurationReceipt> applied =
      engine->apply_configuration(declaration);
  if (!applied.ok()) {
    return refuse(applied.error());
  }
  std::cout << tzm::render(applied.value());

  const tzm::Configuration configuration = engine->configuration();
  const tzm::Timestamp now = engine->clock().now();
  for (std::uint64_t id = 1; id <= kFacilityZones; ++id) {
    const tzm::ZoneId zone = tzm::ZoneId::from_value(id);
    const std::int64_t temperature = 40000 + 3000 * static_cast<std::int64_t>(id);
    const tzm::Result<tzm::ObservationReceipt> ingested = engine->ingest_observation(
        make_observation(*engine, configuration, zone, next_sequence(*engine, zone), temperature,
                         now));
    if (!ingested.ok()) {
      return refuse(ingested.error());
    }
    std::cout << tzm::render(ingested.value());
  }

  const tzm::Result<tzm::EvaluationResult> evaluated =
      engine->evaluate(make_evaluation_request(ids.evaluation(), configuration, epoch));
  if (!evaluated.ok()) {
    return refuse(evaluated.error());
  }
  std::cout << tzm::render(evaluated.value());
  for (const tzm::PlacementConstraint& constraint : evaluated.value().constraints()) {
    std::cout << "constraint: " << constraint.explanation() << "\n";
  }

  tzm::EvaluationCommitRequest commit;
  commit.command = ids.command();
  commit.attempt = ids.attempt();
  commit.epoch = epoch;
  commit.evaluation = evaluated.value().id();
  commit.configuration_generation = evaluated.value().configuration_generation();
  commit.evidence_generation = evaluated.value().evidence_generation();
  commit.basis_revision = evaluated.value().basis_revision();
  const tzm::Result<tzm::EvaluationCommitReceipt> committed = engine->commit_evaluation(commit);
  if (!committed.ok()) {
    return refuse(committed.error());
  }
  std::cout << tzm::render(committed.value());

  const tzm::Status closed = engine->close();
  if (!closed.ok()) {
    return refuse(closed.error());
  }
  std::cout << "scenario: ok\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int index = 1; index < argc; ++index) {
    args.emplace_back(argv[index]);
  }

  if (args.empty() || args[0] == "--help" || args[0] == "-h") {
    print_usage(std::cout);
    return 0;
  }
  if (args[0] == "--version") {
    std::cout << tzm::version_string() << "\n";
    std::cout << tzm::build_information();
    return 0;
  }
  if (args[0] == "self-check") {
    if (args.size() != 1) {
      std::cerr << "self-check takes no options\n";
      return 2;
    }
    return run_self_check();
  }
  if (args[0] == "inspect" || args[0] == "scenario") {
    std::string store_root;
    for (std::size_t index = 1; index < args.size(); ++index) {
      if (args[index] == "--store") {
        if (index + 1 >= args.size()) {
          std::cerr << "--store requires a directory\n";
          return 2;
        }
        store_root = args[index + 1];
        ++index;
      } else {
        std::cerr << "unknown option: " << args[index] << "\n";
        return 2;
      }
    }
    if (store_root.empty()) {
      std::cerr << args[0] << " requires --store <dir>\n";
      return 2;
    }
    if (args[0] == "inspect") {
      return run_inspect(store_root);
    }
    return run_scenario(store_root);
  }

  std::cerr << "unknown command: " << args[0] << "\n";
  print_usage(std::cerr);
  return 2;
}
