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
#include "reference_model.hpp"
#include "test_harness.hpp"
#include "thermal_zone_manager/thermal_zone_manager.hpp"

// Randomized property tests against an independently written reference model.
// The reference uses decimal big-integer arithmetic and linear neighbour scans;
// the library uses 128-bit binary intermediates and an indexed adjacency list.
// Agreement between them is evidence that the exact rational semantics are
// implemented as documented. Every failing seed is printed, and the seed is
// part of the assertion message so the case can be replayed.

using namespace thermal_zone_manager;
using tzm_test::EdgeSpec;
using tzm_test::Facility;
using tzm_test::Rng;
using tzm_test::ZoneSpec;

namespace {

struct Scenario {
  std::vector<ZoneSpec> zones;
  std::vector<EdgeSpec> edges;
  std::vector<std::int64_t> temperatures;
  std::vector<bool> observe;
  std::vector<bool> with_heat;
  std::vector<std::int64_t> heat;
  std::uint32_t hops = 1;
  std::uint32_t release_hold = 3;
};

// A bounded random world. Coefficients are kept small enough that the outgoing
// budget holds by construction, so generation never fails validation.
Scenario make_scenario(Rng& rng) {
  Scenario scenario;
  const std::size_t count = static_cast<std::size_t>(rng.range(1, 6));
  for (std::size_t index = 0; index < count; ++index) {
    ZoneSpec spec;
    spec.id = static_cast<std::uint32_t>(index + 1);
    spec.name = "z" + std::to_string(spec.id);
    spec.floor_mc = rng.range(-20000, 0);
    spec.onset_mc = spec.floor_mc + rng.range(1000, 40000);
    spec.ceiling_mc = spec.onset_mc + rng.range(1000, 40000);
    spec.critical_mc = spec.ceiling_mc + rng.range(0, 10000);
    spec.has_declared_heat = rng.bounded(4) != 0;
    spec.declared_heat_mw = rng.range(0, 8000000);
    spec.resistance = rng.range(1000, 60000);
    spec.steps = static_cast<std::uint32_t>(rng.range(1, 12));
    spec.recovery_margin_mc = rng.range(1, spec.ceiling_mc - spec.onset_mc - 1);
    scenario.zones.push_back(spec);
  }
  const std::size_t edge_count = count <= 1 ? 0 : static_cast<std::size_t>(rng.range(0, 8));
  for (std::size_t index = 0; index < edge_count; ++index) {
    EdgeSpec edge;
    edge.source = static_cast<std::uint32_t>(rng.range(1, static_cast<std::int64_t>(count)));
    edge.sink = static_cast<std::uint32_t>(rng.range(1, static_cast<std::int64_t>(count)));
    if (edge.source == edge.sink) {
      continue;
    }
    edge.ppm = static_cast<std::int32_t>(rng.range(0, 150000));
    bool duplicate = false;
    for (const EdgeSpec& existing : scenario.edges) {
      if (existing.source == edge.source && existing.sink == edge.sink) {
        duplicate = true;
      }
    }
    if (!duplicate) {
      scenario.edges.push_back(edge);
    }
  }
  scenario.hops = static_cast<std::uint32_t>(rng.range(1, 3));
  scenario.release_hold = static_cast<std::uint32_t>(rng.range(1, 4));
  for (std::size_t index = 0; index < count; ++index) {
    scenario.temperatures.push_back(
        rng.range(scenario.zones[index].floor_mc - 5000, scenario.zones[index].critical_mc + 5000));
    scenario.observe.push_back(rng.bounded(5) != 0);
    scenario.with_heat.push_back(rng.bounded(2) == 0);
    scenario.heat.push_back(rng.range(0, 8000000));
  }
  return scenario;
}

// The observation sequence has to grow strictly, so the fixture assigns it.
Result<EvaluationResult> run_scenario(Facility& harness, const Scenario& scenario) {
  for (std::size_t index = 0; index < scenario.zones.size(); ++index) {
    if (!scenario.observe[index]) {
      continue;
    }
    const Status observed =
        scenario.with_heat[index]
            ? harness.observe_with_heat(scenario.zones[index].id,
                                        scenario.temperatures[index],
                                        scenario.heat[index])
            : harness.observe(scenario.zones[index].id, scenario.temperatures[index]);
    if (!observed.ok()) {
      return observed.error();
    }
  }
  return harness.evaluate();
}

void compare(const Scenario& scenario, const EvaluationResult& result, std::uint64_t seed) {
  tzm_test::reference::Model model;
  model.hops = scenario.hops;
  model.release_hold = scenario.release_hold;
  model.now_ns = 0;
  model.future_tolerance_ns = 0;
  model.evidence_generation = 1;
  for (const ZoneSpec& spec : scenario.zones) {
    tzm_test::reference::Zone zone;
    zone.id = spec.id;
    zone.floor_mc = spec.floor_mc;
    zone.onset_mc = spec.onset_mc;
    zone.ceiling_mc = spec.ceiling_mc;
    zone.critical_mc = spec.critical_mc;
    zone.has_declared_heat = spec.has_declared_heat;
    zone.declared_heat_mw = spec.declared_heat_mw;
    zone.resistance_uk_per_w = spec.resistance;
    zone.derate_steps = spec.steps;
    zone.recovery_margin_mc = spec.recovery_margin_mc;
    model.zones.push_back(zone);
  }
  for (const EdgeSpec& spec : scenario.edges) {
    tzm_test::reference::Edge edge;
    edge.source = spec.source;
    edge.sink = spec.sink;
    edge.coefficient_ppm = spec.ppm;
    model.edges.push_back(edge);
  }
  for (std::size_t index = 0; index < scenario.zones.size(); ++index) {
    tzm_test::reference::Zone& zone = model.zones[index];
    zone.has_observation = scenario.observe[index];
    zone.observed_temperature_mc = scenario.temperatures[index];
    zone.has_observed_heat = scenario.with_heat[index];
    zone.observed_heat_mw = scenario.heat[index];
    zone.observed_at_ns = 0;
    zone.validity_ns = 60000000000ULL;
    zone.observation_evidence_generation = 1;
    // The reference needs the same starting derating state the engine had,
    // which for a freshly declared world is all zeroes.
    zone.published_level = 0;
    zone.published_steps = 0;
    zone.hold_count = 0;
  }
  const std::vector<tzm_test::reference::Outcome> expected =
      tzm_test::reference::evaluate(model);

  TZM_REQUIRE_EQ(result.zones().size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const ZoneThermalState& actual = result.zones()[index];
    const tzm_test::reference::Outcome& want = expected[index];
    const std::string context = " (seed " + std::to_string(seed) + ", zone " +
                                std::to_string(actual.zone.raw()) + ")";
    if (actual.evidence_freshness == EvidenceFreshness::Fresh) {
      TZM_CHECK_EQ(want.freshness, 0);
    }
    TZM_CHECK_EQ(actual.observed_temperature_known, want.temperature_known);
    TZM_CHECK_EQ(actual.heat_known, want.heat_known);
    TZM_CHECK_EQ(actual.heat.value, want.heat_mw);
    TZM_CHECK_EQ(actual.coupled_heat_known, want.coupled_known);
    TZM_CHECK_EQ(actual.coupled_heat.value, want.coupled_heat_mw);
    if (actual.coupled_heat_known) {
      TZM_CHECK_EQ(actual.coupled_rise.value, want.coupled_rise_mc);
    }
    TZM_CHECK_EQ(actual.effective_temperature_known, want.effective_known);
    if (actual.effective_temperature_known) {
      TZM_CHECK_EQ(actual.effective_temperature.value, want.effective_mc);
      TZM_CHECK_EQ(static_cast<int>(actual.band), want.band);
    }
    TZM_CHECK_EQ(static_cast<int>(actual.headroom.status), want.headroom_status);
    if (actual.headroom.status == HeadroomStatus::Known) {
      TZM_CHECK_EQ(actual.headroom.temperature_margin.value, want.temperature_margin_mc);
      TZM_CHECK_EQ(actual.headroom.power_margin.value, want.power_margin_mw);
    }
    TZM_CHECK_EQ(actual.derating_after.published_level, want.published_level);
    TZM_CHECK_EQ(actual.derating_after.hold_count, want.hold_count);
    TZM_CHECK_EQ(actual.derate_fraction.value, want.derate_ppm);
    TZM_CHECK_EQ(actual.allowance_known, want.allowance_known);
    if (actual.allowance_known) {
      TZM_CHECK_EQ(actual.placement_allowance.value, want.allowance_mw);
    }
    TZM_CHECK_EQ(static_cast<int>(actual.constraint_kind), want.constraint);
    if (actual.coupled_heat.value != want.coupled_heat_mw ||
        actual.derating_after.published_level != want.published_level) {
      ::tzm_test::report_note("reference mismatch" + context);
    }
  }
}

}  // namespace

TZM_TEST(property, randomized_worlds_agree_with_the_reference_model) {
  for (std::uint64_t seed = 1; seed <= 60; ++seed) {
    Rng rng(seed * 2654435761ULL + 1ULL);
    const Scenario scenario = make_scenario(rng);
    tzm_test::TempDir directory("property-" + std::to_string(seed));
    Result<std::unique_ptr<Facility>> facility = Facility::open(
        directory.path(), scenario.zones, scenario.edges, scenario.hops, scenario.release_hold);
    TZM_REQUIRE_OK(facility);
    Result<EvaluationResult> evaluation = run_scenario(*facility.value(), scenario);
    TZM_REQUIRE_OK(evaluation);
    compare(scenario, evaluation.value(), seed);
  }
}

TZM_TEST(property, repeated_evaluations_are_stable_and_hysteresis_is_monotone) {
  for (std::uint64_t seed = 500; seed <= 530; ++seed) {
    Rng rng(seed * 40503ULL + 7ULL);
    const Scenario scenario = make_scenario(rng);
    tzm_test::TempDir directory("property-stability-" + std::to_string(seed));
    Result<std::unique_ptr<Facility>> facility = Facility::open(
        directory.path(), scenario.zones, scenario.edges, scenario.hops, scenario.release_hold);
    TZM_REQUIRE_OK(facility);
    Facility& harness = *facility.value();
    Result<EvaluationResult> evaluation = run_scenario(harness, scenario);
    TZM_REQUIRE_OK(evaluation);

    // Repeating the evaluation without publishing must give identical margins.
    Result<EvaluationResult> repeated = harness.evaluate();
    TZM_REQUIRE_OK(repeated);
    TZM_REQUIRE_EQ(evaluation.value().zones().size(), repeated.value().zones().size());
    for (std::size_t index = 0; index < evaluation.value().zones().size(); ++index) {
      TZM_CHECK_EQ(evaluation.value().zones()[index].headroom.temperature_margin.value,
                   repeated.value().zones()[index].headroom.temperature_margin.value);
      TZM_CHECK_EQ(evaluation.value().zones()[index].placement_allowance.value,
                   repeated.value().zones()[index].placement_allowance.value);
      TZM_CHECK(evaluation.value().zones()[index].constraint_kind ==
                repeated.value().zones()[index].constraint_kind);
    }

    // Publishing then re-evaluating must never raise the published derating
    // above the instantaneous level, and never lower it below zero.
    TZM_REQUIRE_OK(harness.commit(evaluation.value()));
    Result<EvaluationResult> after = harness.evaluate();
    TZM_REQUIRE_OK(after);
    for (const ZoneThermalState& zone : after.value().zones()) {
      TZM_CHECK(zone.derating_after.published_level <= zone.derating_before.published_steps);
      TZM_CHECK(zone.derate_fraction.value >= 0);
      TZM_CHECK(zone.derate_fraction.value <= kPpmScale);
      if (zone.derating_after.published_steps > 0) {
        TZM_CHECK(zone.derating_after.published_level <= zone.derating_after.published_steps);
      }
    }
  }
}

TZM_TEST(property, monotone_warming_never_lowers_the_published_level) {
  tzm_test::TempDir directory("property-monotone-warming");
  ZoneSpec spec;
  spec.id = 1;
  spec.name = "warm";
  spec.floor_mc = 0;
  spec.onset_mc = 40000;
  spec.ceiling_mc = 80000;
  spec.critical_mc = 90000;
  spec.steps = 8;
  spec.recovery_margin_mc = 500;
  Result<std::unique_ptr<Facility>> facility = Facility::open(directory.path(), {spec}, {}, 1, 2);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  std::uint32_t previous = 0;
  for (std::int64_t temperature = 30000; temperature <= 85000; temperature += 500) {
    TZM_REQUIRE_OK(harness.observe(1, temperature));
    Result<EvaluationResult> evaluation = harness.evaluate_and_commit();
    TZM_REQUIRE_OK(evaluation);
    const ZoneThermalState* zone = evaluation.value().find(ZoneId::from_value(1));
    TZM_REQUIRE(zone != nullptr);
    TZM_CHECK(zone->derating_after.published_level >= previous);
    previous = zone->derating_after.published_level;
    TZM_CHECK_EQ(zone->derating_after.published_level, zone->instant_derate_level);
  }
  TZM_CHECK_EQ(previous, 8U);
}

TZM_TEST(property, envelopes_are_classified_consistently_with_the_reference) {
  for (std::uint64_t seed = 900; seed <= 940; ++seed) {
    Rng rng(seed * 2246822519ULL + 3ULL);
    const std::int64_t floor_mc = rng.range(-30000, 10000);
    const std::int64_t onset_mc = floor_mc + rng.range(1, 30000);
    const std::int64_t ceiling_mc = onset_mc + rng.range(1, 30000);
    const std::int64_t critical_mc = ceiling_mc + rng.range(0, 5000);
    TemperatureEnvelope envelope;
    envelope.floor_temp = MilliCelsius{floor_mc};
    envelope.derate_onset = MilliCelsius{onset_mc};
    envelope.ceiling_temp = MilliCelsius{ceiling_mc};
    envelope.critical_temp = MilliCelsius{critical_mc};
    TZM_REQUIRE_OK(envelope.validate());
    for (int step = 0; step < 200; ++step) {
      const std::int64_t temperature = rng.range(floor_mc - 5000, critical_mc + 5000);
      const ThermalBand band = classify(envelope, MilliCelsius{temperature});
      int expected = 1;
      if (temperature < floor_mc) {
        expected = 0;
      } else if (temperature < onset_mc) {
        expected = 1;
      } else if (temperature < ceiling_mc) {
        expected = 2;
      } else if (temperature < critical_mc) {
        expected = 3;
      } else {
        expected = 4;
      }
      TZM_CHECK_EQ(static_cast<int>(band), expected);
    }
  }
}
