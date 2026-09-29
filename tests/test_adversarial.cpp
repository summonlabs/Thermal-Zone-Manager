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
#include <limits>
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

std::vector<ZoneSpec> many_zones(std::size_t count) {
  std::vector<ZoneSpec> zones;
  zones.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    ZoneSpec spec;
    spec.id = static_cast<std::uint32_t>(index + 1);
    spec.name = "z" + std::to_string(spec.id);
    spec.declared_heat_mw = 100000;
    zones.push_back(spec);
  }
  return zones;
}

}  // namespace

TZM_TEST(adversarial, the_zone_ceiling_is_enforced_before_allocation) {
  const std::size_t over = kMaxZones + 1;
  std::vector<ZoneConfiguration> zones;
  zones.reserve(over);
  for (std::size_t index = 0; index < over; ++index) {
    ZoneSpec spec;
    spec.id = static_cast<std::uint32_t>(index + 1);
    spec.name = "z" + std::to_string(spec.id);
    zones.push_back(tzm_test::make_zone(spec));
  }
  Result<Configuration> configuration = Configuration::create(
      std::move(zones), CouplingGraph{}, ThermalPolicy{}, ConfigurationGeneration::first(),
      EvidenceGeneration::first());
  TZM_CHECK_ERR(configuration, ErrorCode::TooManyZones);

  Result<Configuration> none = Configuration::create(
      {}, CouplingGraph{}, ThermalPolicy{}, ConfigurationGeneration::first(),
      EvidenceGeneration::first());
  TZM_CHECK_ERR(none, ErrorCode::EmptyConfiguration);
}

TZM_TEST(adversarial, the_edge_ceiling_is_enforced_before_allocation) {
  std::vector<CouplingEdge> edges;
  edges.reserve(kMaxCouplingEdges + 1);
  std::vector<ZoneId> ids;
  ids.push_back(ZoneId::from_value(1));
  ids.push_back(ZoneId::from_value(2));
  for (std::size_t index = 0; index <= kMaxCouplingEdges; ++index) {
    CouplingEdge edge;
    edge.source = ZoneId::from_value(2);
    edge.sink = ZoneId::from_value(1);
    edge.coefficient = PartsPerMillion{0};
    edges.push_back(edge);
  }
  TZM_CHECK_ERR(CouplingGraph::create(edges, ids), ErrorCode::TooManyCouplingEdges);
}

TZM_TEST(adversarial, a_dense_coupling_graph_stays_bounded) {
  tzm_test::TempDir directory("adversarial-dense");
  const std::size_t count = 64;
  std::vector<ZoneSpec> zones = many_zones(count);
  std::vector<EdgeSpec> edges;
  for (std::size_t source = 0; source < count; ++source) {
    for (std::size_t sink = 0; sink < count; ++sink) {
      if (source == sink) {
        continue;
      }
      EdgeSpec spec;
      spec.source = static_cast<std::uint32_t>(source + 1);
      spec.sink = static_cast<std::uint32_t>(sink + 1);
      // At most 1/64 of unity leaves each zone, so the outgoing budget holds
      // and the graph is contractive.
      spec.ppm = 15000;
      edges.push_back(spec);
    }
  }
  Result<std::unique_ptr<Facility>> facility = Facility::open(directory.path(), zones, edges, 4, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  for (std::uint32_t zone = 1; zone <= count; ++zone) {
    TZM_REQUIRE_OK(harness.observe(zone, 60000));
  }
  Result<EvaluationResult> evaluation = harness.evaluate();
  TZM_REQUIRE_OK(evaluation);
  TZM_CHECK_EQ(evaluation.value().zones().size(), count);
  for (const ZoneThermalState& zone : evaluation.value().zones()) {
    TZM_CHECK(zone.coupled_heat.value >= 0);
    TZM_CHECK(zone.coupled_rise.value >= 0);
    // The series is bounded by the contraction factor: with 100 W per zone and
    // a contraction of 63/64 the total arriving heat stays under 6.4 kW.
    TZM_CHECK(zone.coupled_heat.value < 6400000);
    TZM_CHECK(zone.effective_temperature.value < kMaxPlausibleMilliCelsius);
  }
}

TZM_TEST(adversarial, self_coupling_is_refused_even_at_full_coefficient) {
  std::vector<ZoneId> ids;
  ids.push_back(ZoneId::from_value(1));
  ids.push_back(ZoneId::from_value(2));
  ids.push_back(ZoneId::from_value(3));
  // Every zone tries to hand all of its heat to zone 1.
  std::vector<CouplingEdge> edges;
  for (std::uint32_t source = 1; source <= 3; ++source) {
    CouplingEdge edge;
    edge.source = ZoneId::from_value(source);
    edge.sink = ZoneId::from_value(1);
    edge.coefficient = PartsPerMillion{kPpmScale};
    edges.push_back(edge);
  }
  TZM_CHECK_ERR(CouplingGraph::create(edges, ids), ErrorCode::SelfCoupling);
}

TZM_TEST(adversarial, contradictory_limits_are_refused_in_order) {
  ZoneSpec spec;
  spec.id = 1;
  spec.name = "z1";
  spec.ceiling_mc = 80000;
  spec.critical_mc = 70000;
  Result<Configuration> configuration = tzm_test::make_configuration({spec}, {});
  TZM_CHECK_ERR(configuration, ErrorCode::ContradictoryLimits);

  spec.critical_mc = 90000;
  spec.onset_mc = 80000;
  Result<Configuration> equal = tzm_test::make_configuration({spec}, {});
  TZM_CHECK_ERR(equal, ErrorCode::InvalidEnvelope);

  spec.onset_mc = 60000;
  spec.resistance = 0;
  Result<Configuration> no_resistance = tzm_test::make_configuration({spec}, {});
  TZM_CHECK_ERR(no_resistance, ErrorCode::InvalidThermalResistance);

  spec.resistance = 10000;
  spec.steps = 0;
  Result<Configuration> no_steps = tzm_test::make_configuration({spec}, {});
  TZM_CHECK_ERR(no_steps, ErrorCode::InvalidDerateLadder);

  spec.steps = 8;
  spec.recovery_margin_mc = 0;
  Result<Configuration> no_margin = tzm_test::make_configuration({spec}, {});
  TZM_CHECK_ERR(no_margin, ErrorCode::InvalidDerateLadder);

  spec.recovery_margin_mc = 1000000;
  Result<Configuration> huge_margin = tzm_test::make_configuration({spec}, {});
  TZM_CHECK_ERR(huge_margin, ErrorCode::InvalidDerateLadder);
}

TZM_TEST(adversarial, a_name_that_is_not_a_name_is_refused) {
  TZM_CHECK_ERR(ZoneName::create(""), ErrorCode::InvalidArgument);
  TZM_CHECK_ERR(ZoneName::create(" leading"), ErrorCode::InvalidArgument);
  TZM_CHECK_ERR(ZoneName::create("trailing "), ErrorCode::InvalidArgument);
  TZM_CHECK_ERR(ZoneName::create(std::string("nul\0inside", 10)), ErrorCode::TextNotValidUtf8);
  TZM_CHECK_ERR(ZoneName::create(std::string("\x01")), ErrorCode::TextNotValidUtf8);
  TZM_CHECK_ERR(ZoneName::create("\xC3\x28"), ErrorCode::TextNotValidUtf8);
  // An overlong encoding of '/' is not valid UTF-8.
  TZM_CHECK_ERR(ZoneName::create(std::string("\xC0\xAF")), ErrorCode::TextNotValidUtf8);
  // A surrogate half is not valid UTF-8.
  TZM_CHECK_ERR(ZoneName::create(std::string("\xED\xA0\x80")), ErrorCode::TextNotValidUtf8);
  TZM_CHECK_ERR(ZoneName::create(std::string(200, 'a')), ErrorCode::NameTooLong);
  TZM_REQUIRE_OK(ZoneName::create("rack-a.1"));
  TZM_REQUIRE_OK(ZoneName::create("\xE6\x9C\xBA\xE6\x9F\x9C"));
}

TZM_TEST(adversarial, repeated_configuration_replacement_is_stable) {
  tzm_test::TempDir directory("adversarial-replacement");
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), many_zones(6), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  for (int round = 0; round < 12; ++round) {
    std::vector<ZoneSpec> zones = many_zones(6);
    if (round % 3 == 0) {
      zones[2].ceiling_mc += 100;
    }
    TZM_REQUIRE_OK(harness.apply(zones));
    const std::uint64_t revision = harness.engine().fencing().revision.raw();
    TZM_CHECK(revision > 0);
    TZM_REQUIRE_OK(harness.observe(1, 60000));
    Result<EvaluationResult> evaluation = harness.evaluate();
    TZM_REQUIRE_OK(evaluation);
    TZM_CHECK_EQ(evaluation.value().zones().size(), static_cast<std::size_t>(6));
  }
  TZM_CHECK(harness.engine().configuration().generation().raw() >= 12);
  // The store must still reopen cleanly after all of that churn.
  TZM_CHECK_OK(harness.engine().close());
  Result<std::unique_ptr<Facility>> reopened =
      Facility::open(directory.path(), {}, {}, 1, 3, StoreAccess::ReadWrite, true);
  TZM_REQUIRE_OK(reopened);
  TZM_CHECK_EQ(reopened.value()->engine().configuration().zone_count(),
               static_cast<std::size_t>(6));
}

TZM_TEST(adversarial, extreme_temperatures_saturate_rather_than_overflow) {
  tzm_test::TempDir directory("adversarial-extremes");
  std::vector<ZoneSpec> zones = many_zones(1);
  zones[0].floor_mc = kAbsoluteZeroMilliCelsius;
  zones[0].onset_mc = kAbsoluteZeroMilliCelsius + 1;
  zones[0].ceiling_mc = kMaxPlausibleMilliCelsius;
  zones[0].critical_mc = kMaxPlausibleMilliCelsius;
  zones[0].resistance = 1;
  Result<std::unique_ptr<Facility>> facility = Facility::open(directory.path(), zones, {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  TZM_REQUIRE_OK(harness.observe_with_heat(1, kMaxPlausibleMilliCelsius, kMaxPlausibleMilliWatts));
  Result<EvaluationResult> evaluation = harness.evaluate();
  TZM_REQUIRE_OK(evaluation);
  const ZoneThermalState* zone = evaluation.value().find(ZoneId::from_value(1));
  TZM_REQUIRE(zone != nullptr);
  TZM_CHECK(zone->headroom.status == HeadroomStatus::Known);
  TZM_CHECK_EQ(zone->headroom.temperature_margin.value, 0);
  TZM_CHECK(zone->constraint_kind == PlacementConstraintKind::Prohibited);
  TZM_CHECK(zone->allowance_known);
  TZM_CHECK_EQ(zone->placement_allowance.value, 0);
}

TZM_TEST(adversarial, an_out_of_range_observation_is_refused) {
  tzm_test::TempDir directory("adversarial-observation-range");
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), many_zones(1), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  TZM_CHECK_ERR(harness.observe(1, kMaxPlausibleMilliCelsius + 1), ErrorCode::InvalidTemperature);
  TZM_CHECK_ERR(harness.observe(1, kAbsoluteZeroMilliCelsius - 1),
                ErrorCode::InvalidTemperature);
  TZM_CHECK_ERR(harness.observe_with_heat(1, 60000, -1), ErrorCode::InvalidArgument);
  TZM_CHECK_ERR(harness.observe(1, 60000, 0), ErrorCode::InvalidFreshnessWindow);
  TZM_CHECK_ERR(harness.observe(1, 60000, kMaxFreshnessWindowNs + 1),
                ErrorCode::InvalidFreshnessWindow);
}

TZM_TEST(adversarial, an_unknown_zone_is_never_substituted) {
  tzm_test::TempDir directory("adversarial-unknown-zone");
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), many_zones(2), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  TZM_CHECK_ERR(harness.observe(9, 60000), ErrorCode::UnknownZone);
  TZM_CHECK(harness.engine().configuration().find(ZoneId::from_value(9)) == nullptr);
  EvaluationRequest request;
  request.id = EvaluationId::first();
  request.epoch = harness.engine().fencing().epoch;
  request.configuration_generation = harness.engine().fencing().configuration_generation;
  request.evidence_generation = harness.engine().fencing().evidence_generation;
  request.zones.push_back(ZoneId::from_value(9));
  TZM_CHECK_ERR(harness.engine().evaluate(request), ErrorCode::UnknownZone);
}

TZM_TEST(adversarial, duplicate_identity_is_refused) {
  ZoneSpec first;
  first.id = 1;
  first.name = "a";
  ZoneSpec second;
  second.id = 1;
  second.name = "b";
  Result<Configuration> duplicate_id = tzm_test::make_configuration({first, second}, {});
  TZM_CHECK_ERR(duplicate_id, ErrorCode::DuplicateZoneId);

  ZoneSpec third;
  third.id = 2;
  third.name = "a";
  Result<Configuration> duplicate_name = tzm_test::make_configuration({first, third}, {});
  TZM_CHECK_ERR(duplicate_name, ErrorCode::DuplicateZoneName);
}
