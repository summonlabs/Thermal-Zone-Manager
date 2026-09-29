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


#include <cstdint>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "test_harness.hpp"
#include "thermal_zone_manager/thermal_zone_manager.hpp"

using namespace thermal_zone_manager;
using tzm_test::EdgeSpec;
using tzm_test::Facility;
using tzm_test::ZoneSpec;

namespace {

std::vector<ZoneSpec> two_zones() {
  ZoneSpec first;
  first.id = 1;
  first.name = "rack-a";
  ZoneSpec second;
  second.id = 2;
  second.name = "rack-b";
  return {first, second};
}

}  // namespace

TZM_TEST(fencing, mutations_need_an_epoch) {
  tzm_test::TempDir directory("fencing-epoch");
  EngineOptions options;
  options.root = directory.path();
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  Result<std::unique_ptr<ThermalZoneEngine>> engine = ThermalZoneEngine::open(options);
  TZM_REQUIRE_OK(engine);
  ThermalZoneEngine& target = *engine.value();
  TZM_CHECK(target.fencing().epoch.is_zero());

  ConfigurationRequest request;
  request.command = CommandId::first();
  request.attempt = AttemptId::first();
  request.epoch = ControlPlaneEpoch::zero();
  request.expected_generation = ConfigurationGeneration::zero();
  request.zones.push_back(tzm_test::make_zone(two_zones()[0]));
  TZM_CHECK_ERR(target.apply_configuration(request), ErrorCode::MissingEpoch);

  EvaluationRequest evaluation;
  evaluation.id = EvaluationId::first();
  evaluation.epoch = ControlPlaneEpoch::zero();
  evaluation.configuration_generation = ConfigurationGeneration::zero();
  evaluation.evidence_generation = EvidenceGeneration::zero();
  TZM_CHECK_ERR(target.evaluate(evaluation), ErrorCode::MissingEpoch);
}

TZM_TEST(fencing, epoch_must_match_and_only_moves_forward) {
  tzm_test::TempDir directory("fencing-epoch-forward");
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), two_zones(), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();

  EpochRequest backwards;
  backwards.command = CommandId::from_value(harness.next_command());
  backwards.attempt = AttemptId::from_value(harness.next_attempt());
  backwards.expected_current = ControlPlaneEpoch::first();
  backwards.target = ControlPlaneEpoch::first();
  TZM_CHECK_ERR(harness.engine().advance_epoch(backwards), ErrorCode::EpochRegression);

  EpochRequest stale;
  stale.command = CommandId::from_value(harness.next_command());
  stale.attempt = AttemptId::from_value(harness.next_attempt());
  stale.expected_current = ControlPlaneEpoch::from_value(7);
  stale.target = ControlPlaneEpoch::from_value(8);
  TZM_CHECK_ERR(harness.engine().advance_epoch(stale), ErrorCode::EpochMismatch);

  EpochRequest forward;
  forward.command = CommandId::from_value(harness.next_command());
  forward.attempt = AttemptId::from_value(harness.next_attempt());
  forward.expected_current = ControlPlaneEpoch::first();
  forward.target = ControlPlaneEpoch::from_value(2);
  TZM_REQUIRE_OK(harness.engine().advance_epoch(forward));
  TZM_CHECK_EQ(harness.engine().fencing().epoch.raw(), static_cast<std::uint64_t>(2));

  // The old epoch no longer authorises anything. The fixture always stamps
  // the live epoch, so the stale case is built explicitly.
  TZM_REQUIRE_OK(harness.observe(1, 60000));
  const Configuration live = harness.configuration();
  const ZoneConfiguration* declaration = live.find(ZoneId::from_value(1));
  TZM_REQUIRE(declaration != nullptr);
  TemperatureObservation stale_epoch;
  stale_epoch.zone = ZoneId::from_value(1);
  stale_epoch.zone_generation = declaration->generation;
  stale_epoch.evidence_generation = live.evidence_generation();
  stale_epoch.sequence = ObservationSequence::from_value(900000);
  stale_epoch.temperature = MilliCelsius{61000};
  stale_epoch.source = SourceId::literal("stale-bms");
  stale_epoch.observed_at = harness.clock().now();
  stale_epoch.validity = Nanoseconds{60000000000LL};
  stale_epoch.publisher_epoch = ControlPlaneEpoch::first();
  stale_epoch.publisher_incarnation = harness.engine().incarnation();
  TZM_CHECK_ERR(harness.engine().ingest_observation(stale_epoch), ErrorCode::EpochMismatch);

  // Evidence accepted under the previous epoch stays usable: the epoch fences
  // mutation authority, not physical evidence. Freshness is fenced by the
  // evidence generation.
  EvaluationRequest evaluation;
  evaluation.id = EvaluationId::first();
  evaluation.epoch = harness.engine().fencing().epoch;
  evaluation.configuration_generation = harness.configuration().generation();
  evaluation.evidence_generation = harness.configuration().evidence_generation();
  Result<EvaluationResult> result = harness.engine().evaluate(evaluation);
  TZM_REQUIRE_OK(result);
  const ZoneThermalState* zone = result.value().find(ZoneId::from_value(1));
  TZM_REQUIRE(zone != nullptr);
  TZM_CHECK(zone->evidence_freshness == EvidenceFreshness::Fresh);
}

TZM_TEST(fencing, observation_must_match_the_zone_generation) {
  tzm_test::TempDir directory("fencing-zone-generation");
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), two_zones(), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  const Configuration before = harness.configuration();
  const ZoneConfiguration* zone = before.find(ZoneId::from_value(1));
  TZM_REQUIRE(zone != nullptr);

  TemperatureObservation observation;
  observation.zone = ZoneId::from_value(1);
  observation.zone_generation = ZoneGeneration::from_value(zone->generation.raw() + 1);
  observation.evidence_generation = before.evidence_generation();
  observation.sequence = ObservationSequence::first();
  observation.temperature = MilliCelsius{50000};
  observation.source = SourceId::literal("bms");
  observation.observed_at = harness.clock().now();
  observation.validity = Nanoseconds{60000000000LL};
  observation.publisher_epoch = harness.engine().fencing().epoch;
  observation.publisher_incarnation = harness.engine().incarnation();
  TZM_CHECK_ERR(harness.engine().ingest_observation(observation),
                ErrorCode::FutureZoneGeneration);

  observation.zone_generation = ZoneGeneration::from_value(zone->generation.raw() - 1);
  TZM_CHECK_ERR(harness.engine().ingest_observation(observation),
                ErrorCode::StaleZoneGeneration);
}

TZM_TEST(fencing, configuration_replacement_supersedes_evidence) {
  tzm_test::TempDir directory("fencing-evidence-generation");
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), two_zones(), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  TZM_REQUIRE_OK(harness.observe(1, 60000));
  TZM_REQUIRE_OK(harness.observe(2, 60000));

  Result<EvaluationResult> before = harness.evaluate();
  TZM_REQUIRE_OK(before);
  TZM_CHECK(before.value().find(ZoneId::from_value(1))->evidence_freshness ==
            EvidenceFreshness::Fresh);

  // Replacing the declared world bumps the evidence generation, so evidence
  // taken against the previous declaration is reported as superseded rather
  // than reinterpreted.
  TZM_REQUIRE_OK(harness.apply(two_zones()));
  const Configuration after = harness.configuration();
  TZM_CHECK(after.evidence_generation() > before.value().evidence_generation());

  Result<EvaluationResult> refreshed = harness.evaluate();
  TZM_REQUIRE_OK(refreshed);
  const ZoneThermalState* zone = refreshed.value().find(ZoneId::from_value(1));
  TZM_REQUIRE(zone != nullptr);
  TZM_CHECK(zone->evidence_freshness == EvidenceFreshness::Stale);
  TZM_CHECK(zone->headroom.status == HeadroomStatus::Unknown);
  TZM_CHECK(zone->headroom.status != HeadroomStatus::Known);
  bool superseded = false;
  for (const ConstraintReason reason : zone->reasons) {
    if (reason == ConstraintReason::EvidenceSuperseded) {
      superseded = true;
    }
  }
  TZM_CHECK(superseded);
}

TZM_TEST(fencing, observation_sequences_are_strict_and_conflict_aware) {
  tzm_test::TempDir directory("fencing-sequence");
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), two_zones(), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  TZM_REQUIRE_OK(harness.observe_sequenced(1, 60000, 5));
  TZM_REQUIRE_OK(harness.observe_sequenced(1, 61000, 6));
  TZM_CHECK_ERR(harness.observe_sequenced(1, 62000, 5), ErrorCode::StaleSequence);
  TZM_CHECK_ERR(harness.observe_sequenced(1, 63000, 6), ErrorCode::ConflictingObservation);
  // The identical redelivery of the accepted sequence replays instead.
  TZM_REQUIRE_OK(harness.observe_sequenced(1, 61000, 6));
  const ZoneEvidence evidence = harness.engine().evidence_for(ZoneId::from_value(1));
  TZM_CHECK_EQ(evidence.observation.temperature.value, 61000);
}

TZM_TEST(fencing, evaluation_commit_is_fenced_by_the_revision) {
  tzm_test::TempDir directory("fencing-evaluation");
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), two_zones(), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  TZM_REQUIRE_OK(harness.observe(1, 75000));
  TZM_REQUIRE_OK(harness.observe(2, 60000));
  Result<EvaluationResult> evaluation = harness.evaluate_with_basis();
  TZM_REQUIRE_OK(evaluation);

  // A mutation between evaluate and commit invalidates the evaluation.
  TZM_REQUIRE_OK(harness.observe(2, 61000));

  EvaluationCommitRequest request;
  request.command = CommandId::from_value(harness.next_command());
  request.attempt = AttemptId::from_value(harness.next_attempt());
  request.epoch = harness.engine().fencing().epoch;
  request.evaluation = evaluation.value().id();
  request.configuration_generation = evaluation.value().configuration_generation();
  request.evidence_generation = evaluation.value().evidence_generation();
  request.basis_revision = evaluation.value().basis_revision();
  TZM_CHECK_ERR(harness.engine().commit_evaluation(request), ErrorCode::StaleEvaluation);
}

TZM_TEST(fencing, an_unknown_evaluation_identifier_is_refused) {
  tzm_test::TempDir directory("fencing-unknown-evaluation");
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), two_zones(), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  EvaluationCommitRequest request;
  request.command = CommandId::from_value(harness.next_command());
  request.attempt = AttemptId::from_value(harness.next_attempt());
  request.epoch = harness.engine().fencing().epoch;
  request.evaluation = EvaluationId::from_value(9999);
  request.configuration_generation = harness.engine().fencing().configuration_generation;
  request.evidence_generation = harness.engine().fencing().evidence_generation;
  request.basis_revision = harness.engine().fencing().revision;
  TZM_CHECK_ERR(harness.engine().commit_evaluation(request), ErrorCode::UnknownCommand);
}

TZM_TEST(fencing, idempotent_retry_replays_and_conflicting_reuse_is_refused) {
  tzm_test::TempDir directory("fencing-idempotency");
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), two_zones(), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  ThermalZoneEngine& engine = harness.engine();

  ConfigurationRequest first;
  first.command = CommandId::from_value(500);
  first.attempt = AttemptId::first();
  first.epoch = engine.fencing().epoch;
  first.expected_generation = engine.configuration().generation();
  first.zones.push_back(tzm_test::make_zone(two_zones()[0]));
  Result<ConfigurationReceipt> applied = engine.apply_configuration(first);
  TZM_REQUIRE_OK(applied);
  TZM_CHECK(!applied.value().replayed);

  ConfigurationRequest retry = first;
  retry.attempt = AttemptId::from_value(2);
  Result<ConfigurationReceipt> replayed = engine.apply_configuration(retry);
  TZM_REQUIRE_OK(replayed);
  TZM_CHECK(replayed.value().replayed);
  TZM_CHECK_EQ(replayed.value().revision.raw(), applied.value().revision.raw());
  TZM_CHECK_EQ(replayed.value().commit.raw(), applied.value().commit.raw());
  TZM_CHECK_EQ(engine.configuration().zone_count(), static_cast<std::size_t>(1));

  // The same command identifier with a different body is a conflict, not a
  // silent replay.
  ConfigurationRequest different = first;
  different.zones.clear();
  different.zones.push_back(tzm_test::make_zone(two_zones()[1]));
  different.expected_generation = engine.configuration().generation();
  TZM_CHECK_ERR(engine.apply_configuration(different), ErrorCode::IdempotencyConflict);
}

TZM_TEST(fencing, a_reordered_but_identical_request_still_replays) {
  tzm_test::TempDir directory("fencing-idempotency-order");
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), two_zones(), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  ThermalZoneEngine& engine = facility.value()->engine();

  ConfigurationRequest request;
  request.command = CommandId::from_value(700);
  request.attempt = AttemptId::first();
  request.epoch = engine.fencing().epoch;
  request.expected_generation = engine.configuration().generation();
  request.zones.push_back(tzm_test::make_zone(two_zones()[0]));
  request.zones.push_back(tzm_test::make_zone(two_zones()[1]));
  TZM_REQUIRE_OK(engine.apply_configuration(request));

  ConfigurationRequest reordered = request;
  std::swap(reordered.zones[0], reordered.zones[1]);
  Result<ConfigurationReceipt> replayed = engine.apply_configuration(reordered);
  TZM_REQUIRE_OK(replayed);
  TZM_CHECK(replayed.value().replayed);
}
