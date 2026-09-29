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

std::vector<ZoneId> ids(std::initializer_list<std::uint32_t> values) {
  std::vector<ZoneId> result;
  for (const std::uint32_t value : values) {
    result.push_back(ZoneId::from_value(value));
  }
  return canonical_zone_ids(result);
}

CouplingEdge edge(std::uint32_t source, std::uint32_t sink, std::int32_t ppm) {
  CouplingEdge result;
  result.source = ZoneId::from_value(source);
  result.sink = ZoneId::from_value(sink);
  result.coefficient = PartsPerMillion{ppm};
  return result;
}

std::vector<ZoneSpec> simple_zones(std::size_t count) {
  std::vector<ZoneSpec> zones;
  for (std::size_t index = 0; index < count; ++index) {
    ZoneSpec spec;
    spec.id = static_cast<std::uint32_t>(index + 1);
    spec.name = "zone-" + std::to_string(spec.id);
    spec.declared_heat_mw = 1000000;
    spec.resistance = 10000;
    zones.push_back(spec);
  }
  return zones;
}

}  // namespace

TZM_TEST(coupling, canonicalises_edges_into_a_total_order) {
  std::vector<CouplingEdge> edges = {edge(3, 1, 100000), edge(1, 2, 200000), edge(2, 1, 300000)};
  Result<CouplingGraph> graph = CouplingGraph::create(edges, ids({1, 2, 3}));
  TZM_REQUIRE_OK(graph);
  const CouplingGraph::EdgeList& list = graph.value().edges();
  TZM_REQUIRE_EQ(list.size(), static_cast<std::size_t>(3));
  TZM_CHECK(list[0].source == ZoneId::from_value(1) && list[0].sink == ZoneId::from_value(2));
  TZM_CHECK(list[1].source == ZoneId::from_value(2) && list[1].sink == ZoneId::from_value(1));
  TZM_CHECK(list[2].source == ZoneId::from_value(3) && list[2].sink == ZoneId::from_value(1));
  const CouplingGraph::EdgeList& incoming = graph.value().incoming(ZoneId::from_value(1));
  TZM_REQUIRE_EQ(incoming.size(), static_cast<std::size_t>(2));
  TZM_CHECK(incoming[0].source == ZoneId::from_value(2));
  TZM_CHECK(incoming[1].source == ZoneId::from_value(3));
}

TZM_TEST(coupling, refuses_every_malformed_shape) {
  TZM_CHECK_ERR(CouplingGraph::create({edge(1, 1, 1)}, ids({1})), ErrorCode::SelfCoupling);
  TZM_CHECK_ERR(CouplingGraph::create({edge(1, 9, 1)}, ids({1})),
                ErrorCode::UnknownCouplingEndpoint);
  TZM_CHECK_ERR(CouplingGraph::create({edge(1, 2, 1000000), edge(1, 2, 0)}, ids({1, 2})),
                ErrorCode::DuplicateCouplingEdge);
  TZM_CHECK_ERR(CouplingGraph::create({edge(1, 2, 1000001)}, ids({1, 2})),
                ErrorCode::InvalidCoefficient);
  TZM_CHECK_ERR(CouplingGraph::create({edge(1, 2, -1)}, ids({1, 2})),
                ErrorCode::InvalidCoefficient);
  // Outgoing budget: more than unity leaving one zone.
  TZM_CHECK_ERR(CouplingGraph::create({edge(1, 2, 600000), edge(1, 3, 500000)}, ids({1, 2, 3})),
                ErrorCode::CouplingBudgetExceeded);
  // Incoming budget: more than four times unity arriving at one zone.
  std::vector<CouplingEdge> heavy;
  for (std::uint32_t source = 2; source <= 6; ++source) {
    heavy.push_back(edge(source, 1, 999999));
  }
  std::vector<ZoneId> all = ids({1, 2, 3, 4, 5, 6});
  TZM_CHECK_ERR(CouplingGraph::create(heavy, all), ErrorCode::CouplingBudgetExceeded);
}

TZM_TEST(coupling, contraction_reflects_the_incoming_budget) {
  Result<CouplingGraph> contractive =
      CouplingGraph::create({edge(1, 2, 500000)}, ids({1, 2}));
  TZM_REQUIRE_OK(contractive);
  TZM_CHECK(contractive.value().is_contractive());
  TZM_CHECK_EQ(contractive.value().contraction_ppm(), 500000);

  Result<CouplingGraph> boundary = CouplingGraph::create({edge(1, 2, 1000000)}, ids({1, 2}));
  TZM_REQUIRE_OK(boundary);
  TZM_CHECK(!boundary.value().is_contractive());
  TZM_CHECK_EQ(boundary.value().contraction_ppm(), 1000000);
}

TZM_TEST(coupling, engine_applies_first_order_coupling_with_conservative_rounding) {
  tzm_test::TempDir directory("coupling-first-order");
  std::vector<ZoneSpec> zones = simple_zones(2);
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), zones, {EdgeSpec{1, 2, 250000}}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  TZM_REQUIRE_OK(harness.observe_with_heat(1, 70000, 4000000));
  TZM_REQUIRE_OK(harness.observe(2, 70000));
  Result<EvaluationResult> evaluation = harness.evaluate();
  TZM_REQUIRE_OK(evaluation);

  const ZoneThermalState* sink = evaluation.value().find(ZoneId::from_value(2));
  TZM_REQUIRE(sink != nullptr);
  // Zone 1 dissipates 4000 W and 25 percent of it reaches zone 2: 1000 W.
  TZM_CHECK_EQ(sink->coupled_heat.value, 1000000);
  // 1000 W through 0.01 K/W is 10 K.
  TZM_CHECK_EQ(sink->coupled_rise.value, 10000);
  TZM_CHECK_EQ(sink->effective_temperature.value, 80000);
  TZM_CHECK(sink->coupled_heat_known);
  TZM_CHECK_EQ(sink->unknown_neighbour_count, static_cast<std::size_t>(0));
  TZM_CHECK(sink->band == ThermalBand::AtLimit);

  // The source zone is unaffected by its own outgoing coupling.
  const ZoneThermalState* source = evaluation.value().find(ZoneId::from_value(1));
  TZM_REQUIRE(source != nullptr);
  TZM_CHECK_EQ(source->coupled_heat.value, 0);
  TZM_CHECK_EQ(source->effective_temperature.value, 70000);
}

TZM_TEST(coupling, cycles_are_legal_and_stay_bounded) {
  tzm_test::TempDir directory("coupling-cycle");
  std::vector<ZoneSpec> zones = simple_zones(3);
  const std::vector<EdgeSpec> ring = {EdgeSpec{1, 2, 400000}, EdgeSpec{2, 3, 400000},
                                      EdgeSpec{3, 1, 400000}};
  Result<std::unique_ptr<Facility>> facility = Facility::open(directory.path(), zones, ring, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  for (std::uint32_t zone = 1; zone <= 3; ++zone) {
    TZM_REQUIRE_OK(harness.observe_with_heat(zone, 60000, 4000000));
  }
  Result<EvaluationResult> evaluation = harness.evaluate();
  TZM_REQUIRE_OK(evaluation);
  for (const ZoneThermalState& state : evaluation.value().zones()) {
    // Each zone receives 40 percent of one neighbour's 4000 W: 1600 W, which
    // over 0.01 K/W is 16 K.
    TZM_CHECK_EQ(state.coupled_heat.value, 1600000);
    TZM_CHECK_EQ(state.coupled_rise.value, 16000);
    TZM_CHECK_EQ(state.effective_temperature.value, 76000);
    TZM_CHECK(state.headroom.status == HeadroomStatus::Known);
  }
}

TZM_TEST(coupling, multi_hop_refinement_is_bounded_and_terminates) {
  tzm_test::TempDir directory("coupling-hops");
  std::vector<ZoneSpec> zones = simple_zones(3);
  // A chain 1 -> 2 -> 3 that is strictly contractive, so the series converges.
  const std::vector<EdgeSpec> chain = {EdgeSpec{1, 2, 500000}, EdgeSpec{2, 3, 500000}};
  Result<std::unique_ptr<Facility>> facility = Facility::open(directory.path(), zones, chain, 2, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  TZM_REQUIRE_OK(harness.observe_with_heat(1, 60000, 4000000));
  TZM_REQUIRE_OK(harness.observe(2, 60000));
  TZM_REQUIRE_OK(harness.observe(3, 60000));
  Result<EvaluationResult> evaluation = harness.evaluate();
  TZM_REQUIRE_OK(evaluation);
  const ZoneThermalState* third = evaluation.value().find(ZoneId::from_value(3));
  TZM_REQUIRE(third != nullptr);
  TZM_CHECK_EQ(third->coupling_hops_used, 2U);
  TZM_CHECK(!third->coupling_refinement_refused);
  // Hop one carries half of zone 1's observed 4000 W to zone 2 and half of
  // zone 2's declared 1000 W to zone 3. Hop two carries half of the 2000 W now
  // sitting in zone 2 on to zone 3. Zone 2 has no second-hop source because the
  // chain only runs one way, so it accumulates exactly its first-hop 2000 W.
  const ZoneThermalState* second = evaluation.value().find(ZoneId::from_value(2));
  TZM_REQUIRE(second != nullptr);
  TZM_CHECK_EQ(second->coupled_heat.value, 2000000);
  TZM_CHECK_EQ(third->coupled_heat.value, 1500000);
  TZM_CHECK_EQ(third->coupled_rise.value, 15000);
}

TZM_TEST(coupling, non_contractive_graphs_refuse_the_refinement) {
  tzm_test::TempDir directory("coupling-noncontractive");
  std::vector<ZoneSpec> zones = simple_zones(2);
  const std::vector<EdgeSpec> full = {EdgeSpec{1, 2, 1000000}};
  Result<std::unique_ptr<Facility>> facility = Facility::open(directory.path(), zones, full, 4, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  TZM_REQUIRE_OK(harness.observe_with_heat(1, 60000, 1000000));
  TZM_REQUIRE_OK(harness.observe(2, 60000));
  Result<EvaluationResult> evaluation = harness.evaluate();
  TZM_REQUIRE_OK(evaluation);
  const ZoneThermalState* sink = evaluation.value().find(ZoneId::from_value(2));
  TZM_REQUIRE(sink != nullptr);
  TZM_CHECK(sink->coupling_refinement_refused);
  TZM_CHECK_EQ(sink->coupling_hops_used, 1U);
  TZM_CHECK_EQ(sink->coupled_heat.value, 1000000);
  bool saw_reason = false;
  for (const ConstraintReason reason : sink->reasons) {
    if (reason == ConstraintReason::CouplingNotContractive) {
      saw_reason = true;
    }
  }
  TZM_CHECK(saw_reason);
}

TZM_TEST(coupling, an_undeclared_neighbour_makes_the_rise_indeterminate) {
  tzm_test::TempDir directory("coupling-unknown-neighbour");
  std::vector<ZoneSpec> zones = simple_zones(2);
  zones[0].has_declared_heat = false;
  Result<std::unique_ptr<Facility>> facility =
      Facility::open(directory.path(), zones, {EdgeSpec{1, 2, 250000}}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  TZM_REQUIRE_OK(harness.observe(1, 60000));
  TZM_REQUIRE_OK(harness.observe(2, 60000));
  Result<EvaluationResult> evaluation = harness.evaluate();
  TZM_REQUIRE_OK(evaluation);

  const ZoneThermalState* source = evaluation.value().find(ZoneId::from_value(1));
  TZM_REQUIRE(source != nullptr);
  TZM_CHECK(!source->heat_known);
  TZM_CHECK(source->heat_source == HeatSource::None);
  // Its own headroom is still computable: the missing figure is about what it
  // contributes elsewhere.
  TZM_CHECK(source->headroom.status == HeadroomStatus::Known);

  const ZoneThermalState* sink = evaluation.value().find(ZoneId::from_value(2));
  TZM_REQUIRE(sink != nullptr);
  TZM_CHECK(!sink->coupled_heat_known);
  TZM_CHECK_EQ(sink->unknown_neighbour_count, static_cast<std::size_t>(1));
  TZM_CHECK(sink->headroom.status == HeadroomStatus::Indeterminate);
  const PlacementConstraint* constraint = evaluation.value().constraint_for(ZoneId::from_value(2));
  TZM_REQUIRE(constraint != nullptr);
  TZM_CHECK(constraint->kind == PlacementConstraintKind::Indeterminate);
  TZM_CHECK(!constraint->allowance_known);
}
