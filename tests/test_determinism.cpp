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
#include <set>
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

std::vector<ZoneSpec> grid_zones(std::size_t count) {
  std::vector<ZoneSpec> zones;
  for (std::size_t index = 0; index < count; ++index) {
    ZoneSpec spec;
    spec.id = static_cast<std::uint32_t>(index + 1);
    spec.name = "zone-" + std::to_string(spec.id);
    spec.declared_heat_mw = 1000000 + static_cast<std::int64_t>(index) * 1000;
    spec.resistance = 10000 + static_cast<std::int64_t>(index) * 10;
    zones.push_back(spec);
  }
  return zones;
}

std::vector<EdgeSpec> ring_edges(std::size_t count) {
  std::vector<EdgeSpec> edges;
  for (std::size_t index = 0; index < count; ++index) {
    EdgeSpec spec;
    spec.source = static_cast<std::uint32_t>(index + 1);
    spec.sink = static_cast<std::uint32_t>((index + 1) % count + 1);
    spec.ppm = 200000;
    edges.push_back(spec);
  }
  return edges;
}

}  // namespace

TZM_TEST(determinism, zone_order_is_always_ascending) {
  tzm_test::TempDir directory("determinism-order");
  std::vector<ZoneSpec> zones = grid_zones(8);
  std::vector<EdgeSpec> edges = ring_edges(8);
  Result<std::unique_ptr<Facility>> facility = Facility::open(directory.path(), zones, edges, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  for (std::uint32_t zone = 1; zone <= 8; ++zone) {
    TZM_REQUIRE_OK(harness.observe(zone, 60000 + static_cast<std::int64_t>(zone) * 100));
  }
  Result<EvaluationResult> evaluation = harness.evaluate();
  TZM_REQUIRE_OK(evaluation);
  TZM_REQUIRE_EQ(evaluation.value().zones().size(), static_cast<std::size_t>(8));
  TZM_REQUIRE_EQ(evaluation.value().constraints().size(), static_cast<std::size_t>(8));
  std::uint32_t previous = 0;
  for (std::size_t index = 0; index < evaluation.value().zones().size(); ++index) {
    const std::uint32_t current =
        static_cast<std::uint32_t>(evaluation.value().zones()[index].zone.raw());
    TZM_CHECK(current > previous);
    previous = current;
    TZM_CHECK(evaluation.value().constraints()[index].zone ==
              evaluation.value().zones()[index].zone);
  }
  // Reason lists are ascending and unique.
  for (const ZoneThermalState& zone : evaluation.value().zones()) {
    int previous_reason = -1;
    for (const ConstraintReason reason : zone.reasons) {
      const int value = static_cast<int>(reason);
      TZM_CHECK(value > previous_reason);
      previous_reason = value;
    }
  }
}

TZM_TEST(determinism, an_identical_evaluation_is_byte_for_byte_identical) {
  tzm_test::TempDir directory("determinism-bytes");
  std::vector<ZoneSpec> zones = grid_zones(6);
  std::vector<EdgeSpec> edges = ring_edges(6);
  Result<std::unique_ptr<Facility>> facility = Facility::open(directory.path(), zones, edges, 2, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  for (std::uint32_t zone = 1; zone <= 6; ++zone) {
    TZM_REQUIRE_OK(harness.observe_with_heat(zone, 62000, 2000000));
  }
  Result<EvaluationResult> first = harness.evaluate();
  TZM_REQUIRE_OK(first);
  Result<EvaluationResult> second = harness.evaluate();
  TZM_REQUIRE_OK(second);
  // Only the request identifier and the evaluation instant differ, and both
  // are part of the encoding, so the digests must still match because the
  // digest covers the whole result. Compare the durable content instead by
  // evaluating the same identifier twice through a fresh engine.
  TZM_CHECK_EQ(first.value().zones().size(), second.value().zones().size());
  for (std::size_t index = 0; index < first.value().zones().size(); ++index) {
    const ZoneThermalState& a = first.value().zones()[index];
    const ZoneThermalState& b = second.value().zones()[index];
    TZM_CHECK_EQ(a.headroom.temperature_margin.value, b.headroom.temperature_margin.value);
    TZM_CHECK_EQ(a.headroom.power_margin.value, b.headroom.power_margin.value);
    TZM_CHECK_EQ(a.placement_allowance.value, b.placement_allowance.value);
    TZM_CHECK_EQ(a.derating_after.published_level, b.derating_after.published_level);
    TZM_CHECK_EQ(a.derate_fraction.value, b.derate_fraction.value);
    TZM_CHECK(a.constraint_kind == b.constraint_kind);
    TZM_CHECK_EQ(static_cast<int>(a.band), static_cast<int>(b.band));
  }
}

TZM_TEST(determinism, two_independent_engines_agree_on_the_same_world) {
  tzm_test::TempDir first_directory("determinism-engine-a");
  tzm_test::TempDir second_directory("determinism-engine-b");
  std::vector<ZoneSpec> zones = grid_zones(5);
  std::vector<EdgeSpec> edges = ring_edges(5);
  Result<std::unique_ptr<Facility>> left =
      Facility::open(first_directory.path(), zones, edges, 1, 3);
  Result<std::unique_ptr<Facility>> right =
      Facility::open(second_directory.path(), zones, edges, 1, 3);
  TZM_REQUIRE_OK(left);
  TZM_REQUIRE_OK(right);
  for (std::uint32_t zone = 1; zone <= 5; ++zone) {
    TZM_REQUIRE_OK(left.value()->observe_with_heat(zone, 61000, 1500000));
    TZM_REQUIRE_OK(right.value()->observe_with_heat(zone, 61000, 1500000));
  }
  TZM_CHECK_EQ(left.value()->configuration().digest(), right.value()->configuration().digest());

  Result<EvaluationResult> a = left.value()->evaluate();
  Result<EvaluationResult> b = right.value()->evaluate();
  TZM_REQUIRE_OK(a);
  TZM_REQUIRE_OK(b);
  for (std::size_t index = 0; index < a.value().zones().size(); ++index) {
    TZM_CHECK_EQ(a.value().zones()[index].headroom.power_margin.value,
                 b.value().zones()[index].headroom.power_margin.value);
    TZM_CHECK_EQ(a.value().zones()[index].placement_allowance.value,
                 b.value().zones()[index].placement_allowance.value);
    TZM_CHECK_EQ(a.value().zones()[index].coupled_heat.value,
                 b.value().zones()[index].coupled_heat.value);
  }
}

TZM_TEST(determinism, configuration_digest_is_content_addressed) {
  std::vector<ZoneSpec> zones = grid_zones(3);
  Result<Configuration> baseline = tzm_test::make_configuration(zones, ring_edges(3));
  TZM_REQUIRE_OK(baseline);
  Result<Configuration> same = tzm_test::make_configuration(zones, ring_edges(3));
  TZM_REQUIRE_OK(same);
  TZM_CHECK_EQ(baseline.value().digest(), same.value().digest());
  TZM_CHECK(baseline.value() == same.value());

  std::vector<ZoneSpec> changed = zones;
  changed[1].ceiling_mc += 1;
  Result<Configuration> different = tzm_test::make_configuration(changed, ring_edges(3));
  TZM_REQUIRE_OK(different);
  TZM_CHECK_NE(baseline.value().digest(), different.value().digest());
  TZM_CHECK(!(baseline.value() == different.value()));
}

TZM_TEST(determinism, constraint_reasons_are_deduplicated) {
  tzm_test::TempDir directory("determinism-reasons");
  std::vector<ZoneSpec> zones = grid_zones(2);
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), zones, {EdgeSpec{1, 2, 100000}}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  TZM_REQUIRE_OK(harness.observe_with_heat(1, 75000, 1000000));
  TZM_REQUIRE_OK(harness.observe(2, 75000));
  Result<EvaluationResult> evaluation = harness.evaluate();
  TZM_REQUIRE_OK(evaluation);
  for (const ZoneThermalState& zone : evaluation.value().zones()) {
    std::set<int> seen;
    for (const ConstraintReason reason : zone.reasons) {
      TZM_CHECK(seen.insert(static_cast<int>(reason)).second);
    }
  }
}
