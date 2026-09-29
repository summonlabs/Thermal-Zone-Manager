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
#include <filesystem>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "test_harness.hpp"
#include "test_process.hpp"
#include "thermal_zone_manager/thermal_zone_manager.hpp"

using namespace thermal_zone_manager;
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

std::uint64_t file_size_of(const std::string& path) {
  std::error_code code;
  const auto size = std::filesystem::file_size(std::filesystem::path(path), code);
  return code ? 0ULL : static_cast<std::uint64_t>(size);
}

}  // namespace

TZM_TEST(persistence, a_new_store_is_created_with_a_valid_header) {
  tzm_test::TempDir directory("persistence-create");
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), two_zones(), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  const std::string state = directory.path() + "\\thermal-zones.tzm";
  const std::string lock = directory.path() + "\\thermal-zones.lock";
  TZM_CHECK(tzm_test::file_exists(state));
  TZM_CHECK(tzm_test::file_exists(lock));
  TZM_CHECK(facility.value()->engine().durable());
  TZM_CHECK(facility.value()->engine().holds_writer());
  // Nothing has been committed yet, so there is no generation.
  TZM_CHECK_EQ(file_size_of(state), static_cast<std::uint64_t>(64 + 2 * (96 + 8 * 1024 * 1024)));
}

TZM_TEST(persistence, state_survives_close_and_reopen) {
  tzm_test::TempDir directory("persistence-reopen");
  const std::vector<ZoneSpec> zones = two_zones();
  std::uint64_t revision = 0;
  std::uint64_t commit = 0;
  {
    Result<std::unique_ptr<Facility>> facility =
        Facility::open(directory.path(), zones, {}, 1, 3);
    TZM_REQUIRE_OK(facility);
    TZM_REQUIRE_OK(facility.value()->observe(1, 70000));
    revision = facility.value()->engine().fencing().revision.raw();
    commit = facility.value()->engine().fencing().commit.raw();
    TZM_CHECK(revision > 0);
  }
  Result<std::unique_ptr<Facility>> reopened =
      Facility::open(directory.path(), {}, {}, 1, 3, StoreAccess::ReadWrite, true);
  TZM_REQUIRE_OK(reopened);
  ThermalZoneEngine& engine = reopened.value()->engine();
  TZM_CHECK_EQ(engine.fencing().revision.raw(), revision);
  TZM_CHECK_EQ(engine.fencing().commit.raw(), commit);
  TZM_CHECK_EQ(engine.configuration().zone_count(), static_cast<std::size_t>(2));
  const RecoveryReport report = engine.recovery_report();
  TZM_CHECK(report.recovered);
  TZM_CHECK(report.store_existed);
  TZM_CHECK(!report.store_created);
  TZM_CHECK_EQ(report.observation_count, static_cast<std::size_t>(1));
}

TZM_TEST(persistence, recovered_observations_are_never_fresh) {
  tzm_test::TempDir directory("persistence-recovered");
  const std::vector<ZoneSpec> zones = two_zones();
  {
    Result<std::unique_ptr<Facility>> facility =
        Facility::open(directory.path(), zones, {}, 1, 3);
    TZM_REQUIRE_OK(facility);
    TZM_REQUIRE_OK(facility.value()->observe(1, 70000));
    TZM_REQUIRE_OK(facility.value()->observe(2, 65000));
  }
  Result<std::unique_ptr<Facility>> reopened =
      Facility::open(directory.path(), {}, {}, 1, 3, StoreAccess::ReadWrite, true);
  TZM_REQUIRE_OK(reopened);
  Facility& harness = *reopened.value();
  for (std::uint32_t zone = 1; zone <= 2; ++zone) {
    const ZoneEvidence evidence = harness.engine().evidence_for(ZoneId::from_value(zone));
    TZM_CHECK(evidence.present);
    TZM_CHECK(evidence.recovered);
    TZM_CHECK(evidence.freshness == EvidenceFreshness::Recovered);
    TZM_CHECK(!is_usable(evidence.freshness));
  }
  Result<EvaluationResult> evaluation = harness.evaluate();
  TZM_REQUIRE_OK(evaluation);
  for (const ZoneThermalState& state : evaluation.value().zones()) {
    TZM_CHECK(state.evidence_freshness == EvidenceFreshness::Recovered);
    TZM_CHECK(state.headroom.status == HeadroomStatus::Unknown);
  }
  for (const PlacementConstraint& constraint : evaluation.value().constraints()) {
    TZM_CHECK(constraint.kind == PlacementConstraintKind::Indeterminate);
    TZM_CHECK(!constraint.allowance_known);
  }

  // Only a strictly newer observation from a live source clears the marker.
  const Configuration configuration = harness.configuration();
  const ZoneConfiguration* zone = configuration.find(ZoneId::from_value(1));
  TZM_REQUIRE(zone != nullptr);
  TemperatureObservation observation;
  observation.zone = ZoneId::from_value(1);
  observation.zone_generation = zone->generation;
  observation.evidence_generation = configuration.evidence_generation();
  observation.sequence = ObservationSequence::from_value(
      harness.engine().evidence_for(ZoneId::from_value(1)).observation.sequence.raw() + 1);
  observation.temperature = MilliCelsius{70000};
  observation.source = SourceId::literal("live-bms");
  observation.provenance = ProvenanceKind::ExternalSensor;
  observation.observed_at = harness.clock().now();
  observation.validity = Nanoseconds{60000000000LL};
  observation.publisher_epoch = harness.engine().fencing().epoch;
  observation.publisher_incarnation = harness.engine().incarnation();
  TZM_REQUIRE_OK(harness.engine().ingest_observation(observation));
  TZM_CHECK(harness.engine().evidence_for(ZoneId::from_value(1)).freshness ==
            EvidenceFreshness::Fresh);

  // Re-delivering the same sequence is a replay and does not refresh anything.
  TZM_REQUIRE_OK(harness.engine().ingest_observation(observation));
  const ZoneEvidence after = harness.engine().evidence_for(ZoneId::from_value(1));
  TZM_CHECK(!after.recovered);
  TZM_CHECK(after.freshness == EvidenceFreshness::Fresh);
}

TZM_TEST(persistence, read_only_engines_cannot_mutate) {
  tzm_test::TempDir directory("persistence-readonly");
  {
    Result<std::unique_ptr<Facility>> facility =
        Facility::open(directory.path(), two_zones(), {}, 1, 3);
    TZM_REQUIRE_OK(facility);
    TZM_REQUIRE_OK(facility.value()->observe(1, 70000));
  }
  EngineOptions options;
  options.root = directory.path();
  options.access = StoreAccess::ReadOnly;
  options.create_if_missing = false;
  Result<std::unique_ptr<ThermalZoneEngine>> reader = ThermalZoneEngine::open(options);
  TZM_REQUIRE_OK(reader);
  TZM_CHECK(!reader.value()->holds_writer());
  TZM_CHECK(reader.value()->configuration().zone_count() == 2);
  TZM_CHECK_ERR(reader.value()->ingest_observation(TemperatureObservation{}),
                ErrorCode::ReadOnlyStore);
}

TZM_TEST(persistence, a_missing_store_is_refused_when_creation_is_not_allowed) {
  tzm_test::TempDir directory("persistence-missing");
  const std::string root = directory.path() + "\\absent";
  EngineOptions options;
  options.root = root;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = false;
  Result<std::unique_ptr<ThermalZoneEngine>> engine = ThermalZoneEngine::open(options);
  TZM_CHECK_ERR(engine, ErrorCode::StoreUnavailable);
}

TZM_TEST(persistence, closing_is_idempotent_and_stops_mutation) {
  tzm_test::TempDir directory("persistence-close");
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), two_zones(), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  TZM_REQUIRE_OK(harness.observe(1, 70000));
  TZM_CHECK_OK(harness.engine().close());
  TZM_CHECK_OK(harness.engine().close());
  TZM_CHECK(harness.engine().closed());
  TZM_CHECK(!harness.engine().holds_writer());
  // Reads still work against the frozen in-memory world.
  TZM_CHECK_EQ(harness.engine().configuration().zone_count(), static_cast<std::size_t>(2));
  TZM_CHECK_ERR(harness.observe(1, 71000), ErrorCode::StoreClosed);
  EvaluationRequest request;
  request.id = EvaluationId::from_value(1);
  request.epoch = harness.engine().fencing().epoch;
  request.configuration_generation = harness.engine().fencing().configuration_generation;
  request.evidence_generation = harness.engine().fencing().evidence_generation;
  TZM_CHECK_ERR(harness.engine().evaluate(request), ErrorCode::StoreClosed);
}

TZM_TEST(persistence, describe_reports_the_durable_shape) {
  tzm_test::TempDir directory("persistence-describe");
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), two_zones(), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  const std::string text = facility.value()->engine().describe();
  TZM_CHECK(text.find("mode              : durable") != std::string::npos);
  TZM_CHECK(text.find("thermal-zones.tzm") != std::string::npos);
  TZM_CHECK(text.find("thermal-zones.lock") != std::string::npos);
  TZM_CHECK(text.find("zones             : 2") != std::string::npos);
}

TZM_TEST(persistence, canonical_root_resolves_and_rejects_bad_paths) {
  tzm_test::TempDir directory("persistence-canonical");
  Result<std::string> resolved = canonical_store_root(directory.path());
  TZM_REQUIRE_OK(resolved);
  TZM_CHECK(!resolved.value().empty());
  TZM_CHECK(resolved.value().find("..") == std::string::npos);

  TZM_CHECK_ERR(canonical_store_root(""), ErrorCode::StorePathInvalid);

  std::string with_nul = directory.path();
  with_nul.push_back('\0');
  with_nul += "x";
  TZM_CHECK_ERR(canonical_store_root(with_nul), ErrorCode::StorePathInvalid);

  TZM_CHECK_ERR(canonical_store_root(directory.path() + "\\..\\..\\etc"),
                ErrorCode::StorePathInvalid);
  TZM_CHECK_ERR(canonical_store_root(directory.path() + "\\nul"), ErrorCode::StorePathInvalid);
  TZM_CHECK_ERR(canonical_store_root(directory.path() + "\\con.txt"),
                ErrorCode::StorePathInvalid);
  TZM_CHECK_ERR(canonical_store_root(directory.path() + "\\trailing. "),
                ErrorCode::StorePathInvalid);
}
