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

#ifndef TZM_TEST_FIXTURES_HPP
#define TZM_TEST_FIXTURES_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_process.hpp"
#include "thermal_zone_manager/thermal_zone_manager.hpp"

namespace tzm_test {

using thermal_zone_manager::AttemptId;
using thermal_zone_manager::CommandId;
using thermal_zone_manager::Configuration;
using thermal_zone_manager::ConfigurationRequest;
using thermal_zone_manager::ControlPlaneEpoch;
using thermal_zone_manager::CouplingEdge;
using thermal_zone_manager::EngineOptions;
using thermal_zone_manager::Error;
using thermal_zone_manager::ErrorCode;
using thermal_zone_manager::EvaluationCommitRequest;
using thermal_zone_manager::EvaluationId;
using thermal_zone_manager::EvaluationRequest;
using thermal_zone_manager::EvaluationResult;
using thermal_zone_manager::ManualClock;
using thermal_zone_manager::MilliCelsius;
using thermal_zone_manager::MilliWatts;
using thermal_zone_manager::MicroKelvinPerWatt;
using thermal_zone_manager::Nanoseconds;
using thermal_zone_manager::PartsPerMillion;
using thermal_zone_manager::ProvenanceKind;
using thermal_zone_manager::Result;
using thermal_zone_manager::Status;
using thermal_zone_manager::StoreAccess;
using thermal_zone_manager::TemperatureEnvelope;
using thermal_zone_manager::TemperatureObservation;
using thermal_zone_manager::ThermalPolicy;
using thermal_zone_manager::ThermalZoneEngine;
using thermal_zone_manager::Timestamp;
using thermal_zone_manager::ZoneConfiguration;
using thermal_zone_manager::ZoneEvidence;
using thermal_zone_manager::ZoneId;
using thermal_zone_manager::ZoneName;

inline constexpr std::int64_t kSecondNs = 1000000000LL;

// A directory that removes itself. Every test that needs durable state creates
// one under the process temporary directory and never leaves it behind.
class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    static std::uint64_t counter = 0;
    ++counter;
    std::error_code code;
    const std::filesystem::path base = std::filesystem::temp_directory_path(code);
    path_ = (base / ("tzm-test-" + tag + "-" + std::to_string(stamp) + "-" +
                     std::to_string(counter)))
                .string();
    std::filesystem::create_directories(std::filesystem::path(path_), code);
  }

  ~TempDir() {
    std::error_code code;
    std::filesystem::remove_all(std::filesystem::path(path_), code);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

struct ZoneSpec {
  std::uint32_t id = 1;
  std::string name = "zone";
  std::int64_t floor_mc = -5000;
  std::int64_t onset_mc = 60000;
  std::int64_t ceiling_mc = 80000;
  std::int64_t critical_mc = 90000;
  bool has_declared_heat = true;
  std::int64_t declared_heat_mw = 4000000;
  std::int64_t resistance = 12000;
  std::uint32_t steps = 8;
  std::int64_t recovery_margin_mc = 1000;
};

struct EdgeSpec {
  std::uint32_t source = 1;
  std::uint32_t sink = 2;
  std::int32_t ppm = 100000;
};

inline ZoneConfiguration make_zone(const ZoneSpec& spec) {
  ZoneConfiguration zone;
  zone.id = ZoneId::from_value(spec.id);
  Result<ZoneName> name = ZoneName::create(spec.name);
  zone.name = name.ok() ? name.value() : ZoneName::literal("zone");
  zone.generation = thermal_zone_manager::ZoneGeneration::first();
  zone.envelope.floor_temp = MilliCelsius{spec.floor_mc};
  zone.envelope.derate_onset = MilliCelsius{spec.onset_mc};
  zone.envelope.ceiling_temp = MilliCelsius{spec.ceiling_mc};
  zone.envelope.critical_temp = MilliCelsius{spec.critical_mc};
  if (spec.has_declared_heat) {
    zone.declared_heat = MilliWatts{spec.declared_heat_mw};
  }
  zone.thermal_resistance = MicroKelvinPerWatt{spec.resistance};
  zone.derate_steps = spec.steps;
  zone.recovery_margin = MilliCelsius{spec.recovery_margin_mc};
  return zone;
}

inline std::vector<CouplingEdge> make_edges(const std::vector<EdgeSpec>& specs) {
  std::vector<CouplingEdge> edges;
  edges.reserve(specs.size());
  for (const EdgeSpec& spec : specs) {
    CouplingEdge edge;
    edge.source = ZoneId::from_value(spec.source);
    edge.sink = ZoneId::from_value(spec.sink);
    edge.coefficient = PartsPerMillion{spec.ppm};
    edges.push_back(edge);
  }
  return edges;
}

// Builds a configuration the way a caller would, through the validated
// factory. The policy generation is left at one.
inline Result<Configuration> make_configuration(const std::vector<ZoneSpec>& zones,
                                                const std::vector<EdgeSpec>& edges,
                                                std::uint32_t hops = 1,
                                                std::uint32_t release_hold = 3,
                                                std::int64_t future_tolerance_ns = 0) {
  std::vector<ZoneConfiguration> built;
  built.reserve(zones.size());
  for (const ZoneSpec& spec : zones) {
    built.push_back(make_zone(spec));
  }
  ThermalPolicy policy;
  policy.generation = thermal_zone_manager::PolicyGeneration::first();
  policy.coupling_hops = hops;
  policy.release_hold_observations = release_hold;
  policy.future_tolerance = Nanoseconds{future_tolerance_ns};
  Result<thermal_zone_manager::CouplingGraph> graph = thermal_zone_manager::CouplingGraph::create(
      make_edges(edges), thermal_zone_manager::canonical_zone_ids([&zones]() {
        std::vector<ZoneId> ids;
        for (const ZoneSpec& spec : zones) {
          ids.push_back(ZoneId::from_value(spec.id));
        }
        return ids;
      }()));
  if (!graph.ok()) {
    return graph.error();
  }
  return Configuration::create(std::move(built), std::move(graph.value()), policy,
                               thermal_zone_manager::ConfigurationGeneration::first(),
                               thermal_zone_manager::EvidenceGeneration::first());
}

// A complete engine lifecycle with a controlled clock, so every freshness
// scenario is exact rather than timing dependent.
class Facility {
 public:
  static Result<std::unique_ptr<Facility>> open(const std::string& root,
                                                const std::vector<ZoneSpec>& zones,
                                                const std::vector<EdgeSpec>& edges = {},
                                                std::uint32_t hops = 1,
                                                std::uint32_t release_hold = 3,
                                                StoreAccess access = StoreAccess::ReadWrite,
                                                bool create_if_missing = true) {
    auto facility = std::unique_ptr<Facility>(new Facility());
    facility->clock_.set(Timestamp{1700000000LL * kSecondNs});
    EngineOptions options;
    options.root = root;
    options.access = access;
    options.create_if_missing = create_if_missing;
    options.clock = &facility->clock_;
    options.actor = thermal_zone_manager::ActorId::literal("test");
    Result<std::unique_ptr<ThermalZoneEngine>> engine = ThermalZoneEngine::open(options);
    if (!engine.ok()) {
      return engine.error();
    }
    facility->engine_ = std::move(engine.value());
    // Identities are derived from the recovered revision so that reopening a
    // store never reuses a command, evaluation or observation identity that the
    // durable state already records.
    const std::uint64_t base = facility->engine_->fencing().revision.raw();
    facility->next_command_ = 1000000 + base;
    facility->next_evaluation_ = 1000000 + base;
    facility->next_sequence_ = 1000000 + base;
    const Status established = facility->ensure_epoch();
    if (!established.ok()) {
      return established.error();
    }
    if (!zones.empty()) {
      const Status applied = facility->apply(zones, edges, hops, release_hold);
      if (!applied.ok()) {
        return applied.error();
      }
    }
    return facility;
  }

  ThermalZoneEngine& engine() { return *engine_; }
  ManualClock& clock() { return clock_; }

  Configuration configuration() { return engine_->configuration(); }

  // Establishes epoch one when the engine has no epoch yet, and leaves an
  // existing epoch alone so a recovered store can be reopened.
  Status ensure_epoch() {
    if (!engine_->fencing().epoch.is_zero()) {
      return Status();
    }
    return establish_epoch(1);
  }

  Status establish_epoch(std::uint64_t target) {
    thermal_zone_manager::EpochRequest request;
    request.command = CommandId::from_value(next_command_++);
    request.attempt = AttemptId::from_value(next_attempt_++);
    request.expected_current = engine_->fencing().epoch;
    request.target = ControlPlaneEpoch::from_value(target);
    Result<thermal_zone_manager::EpochReceipt> receipt = engine_->advance_epoch(request);
    if (!receipt.ok()) {
      return receipt.error();
    }
    return Status();
  }

  Status apply(const std::vector<ZoneSpec>& zones, const std::vector<EdgeSpec>& edges = {},
               std::uint32_t hops = 1, std::uint32_t release_hold = 3) {
    ConfigurationRequest request;
    request.command = CommandId::from_value(next_command_++);
    request.attempt = AttemptId::from_value(next_attempt_++);
    request.epoch = engine_->fencing().epoch;
    request.expected_generation = engine_->configuration().generation();
    for (const ZoneSpec& spec : zones) {
      request.zones.push_back(make_zone(spec));
    }
    request.coupling = make_edges(edges);
    request.policy.generation = engine_->configuration().policy().generation;
    request.policy.coupling_hops = hops;
    request.policy.release_hold_observations = release_hold;
    Result<thermal_zone_manager::ConfigurationReceipt> receipt =
        engine_->apply_configuration(request);
    if (!receipt.ok()) {
      return receipt.error();
    }
    return Status();
  }

  // A fresh observation stamped with the generations that are current now.
  Status observe(std::uint32_t zone, std::int64_t temperature_mc,
                 std::int64_t validity_ns = 60 * kSecondNs) {
    return observe_impl(zone, temperature_mc, validity_ns, false, 0);
  }

  Status observe_with_heat(std::uint32_t zone, std::int64_t temperature_mc, std::int64_t heat_mw,
                           std::int64_t validity_ns = 60 * kSecondNs) {
    return observe_impl(zone, temperature_mc, validity_ns, true, heat_mw);
  }

  // Stamps an observation with an explicit sequence, so sequencing rules can be
  // exercised directly.
  Status observe_sequenced(std::uint32_t zone, std::int64_t temperature_mc,
                           std::uint64_t sequence) {
    return observe_impl(zone, temperature_mc, 60 * kSecondNs, false, 0, sequence);
  }

  Result<EvaluationResult> evaluate() {
    EvaluationRequest request;
    request.id = EvaluationId::from_value(next_evaluation_++);
    request.epoch = engine_->fencing().epoch;
    request.configuration_generation = engine_->configuration().generation();
    request.evidence_generation = engine_->configuration().evidence_generation();
    return engine_->evaluate(request);
  }

  Result<EvaluationResult> evaluate_with_basis() {
    EvaluationRequest request;
    request.id = EvaluationId::from_value(next_evaluation_++);
    request.epoch = engine_->fencing().epoch;
    request.configuration_generation = engine_->configuration().generation();
    request.evidence_generation = engine_->configuration().evidence_generation();
    request.basis_revision = engine_->fencing().revision;
    return engine_->evaluate(request);
  }

  Status commit(const EvaluationResult& evaluation) {
    EvaluationCommitRequest request;
    request.command = CommandId::from_value(next_command_++);
    request.attempt = AttemptId::from_value(next_attempt_++);
    request.epoch = engine_->fencing().epoch;
    request.evaluation = evaluation.id();
    request.configuration_generation = evaluation.configuration_generation();
    request.evidence_generation = evaluation.evidence_generation();
    request.basis_revision = evaluation.basis_revision();
    Result<thermal_zone_manager::EvaluationCommitReceipt> receipt =
        engine_->commit_evaluation(request);
    if (!receipt.ok()) {
      return receipt.error();
    }
    return Status();
  }

  // Evaluates and publishes in one step, which is the normal test rhythm.
  Result<EvaluationResult> evaluate_and_commit() {
    Result<EvaluationResult> evaluation = evaluate();
    if (!evaluation.ok()) {
      return evaluation.error();
    }
    const Status published = commit(evaluation.value());
    if (!published.ok()) {
      return published.error();
    }
    return evaluation.value();
  }

  std::uint64_t next_command() { return next_command_++; }
  std::uint64_t next_attempt() { return next_attempt_++; }

 private:
  Facility() = default;

  Status observe_impl(std::uint32_t zone_id, std::int64_t temperature_mc,
                      std::int64_t validity_ns, bool with_heat, std::int64_t heat_mw,
                      std::uint64_t explicit_sequence = 0) {
    const Configuration configuration_now = engine_->configuration();
    const ZoneConfiguration* zone = configuration_now.find(ZoneId::from_value(zone_id));
    if (zone == nullptr) {
      return Error(ErrorCode::UnknownZone, "the fixture names a zone that is not declared");
    }
    TemperatureObservation observation;
    observation.zone = ZoneId::from_value(zone_id);
    observation.zone_generation = zone->generation;
    observation.evidence_generation = configuration_now.evidence_generation();
    if (explicit_sequence != 0) {
      observation.sequence =
          thermal_zone_manager::ObservationSequence::from_value(explicit_sequence);
    } else {
      // A source sequence is monotone, so it is always ahead of whatever the
      // durable state already holds for this zone.
      const ZoneEvidence held = engine_->evidence_for(ZoneId::from_value(zone_id));
      const std::uint64_t held_sequence =
          held.present ? held.observation.sequence.raw() : 0;
      std::uint64_t next = next_sequence_;
      if (next <= held_sequence) {
        next = held_sequence + 1;
      }
      next_sequence_ = next + 1;
      observation.sequence = thermal_zone_manager::ObservationSequence::from_value(next);
    }
    observation.temperature = MilliCelsius{temperature_mc};
    if (with_heat) {
      observation.heat = MilliWatts{heat_mw};
    }
    observation.source = thermal_zone_manager::SourceId::literal("test-harness");
    observation.provenance = ProvenanceKind::SyntheticHarness;
    observation.observed_at = clock_.now();
    observation.validity = Nanoseconds{validity_ns};
    observation.publisher_epoch = engine_->fencing().epoch;
    observation.publisher_incarnation = engine_->incarnation();
    Result<thermal_zone_manager::ObservationReceipt> receipt =
        engine_->ingest_observation(observation);
    if (!receipt.ok()) {
      return receipt.error();
    }
    return Status();
  }

  std::unique_ptr<ThermalZoneEngine> engine_;
  ManualClock clock_;
  std::uint64_t next_command_ = 1;
  std::uint64_t next_attempt_ = 1;
  std::uint64_t next_evaluation_ = 1;
  std::uint64_t next_sequence_ = 1;
};

// A deterministic generator. Seeded tests print the seed on failure so a
// randomized failure can always be reproduced.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

  std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }

  std::uint64_t bounded(std::uint64_t limit) { return limit == 0 ? 0 : next() % limit; }

  std::int64_t range(std::int64_t low, std::int64_t high) {
    if (high <= low) {
      return low;
    }
    const auto span = static_cast<std::uint64_t>(high - low + 1);
    return low + static_cast<std::int64_t>(bounded(span));
  }

  std::uint64_t seed() const { return state_; }

 private:
  std::uint64_t state_;
};

}  // namespace tzm_test

#endif  // TZM_TEST_FIXTURES_HPP
