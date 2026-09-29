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

#ifndef TZM_TEST_REFERENCE_MODEL_HPP
#define TZM_TEST_REFERENCE_MODEL_HPP

#include <cstdint>
#include <vector>

#include "bigint.hpp"

// An independent reference model for simple zone graphs. It is written
// directly from the documented semantics, in a different shape from the
// library: every neighbour is found by a linear scan, every rational operation
// goes through the decimal big-integer helper, and the coupling series is
// evaluated by explicit repeated application with no early exits. It exists to
// disagree with the library when the library is wrong.

namespace tzm_test {
namespace reference {

struct Edge {
  std::uint32_t source = 0;
  std::uint32_t sink = 0;
  std::int64_t coefficient_ppm = 0;
};

struct Zone {
  std::uint32_t id = 0;
  std::int64_t floor_mc = 0;
  std::int64_t onset_mc = 0;
  std::int64_t ceiling_mc = 0;
  std::int64_t critical_mc = 0;
  bool has_declared_heat = false;
  std::int64_t declared_heat_mw = 0;
  std::int64_t resistance_uk_per_w = 0;
  std::uint32_t derate_steps = 1;
  std::int64_t recovery_margin_mc = 0;

  bool has_observation = false;
  bool recovered = false;
  std::uint64_t observation_evidence_generation = 0;
  std::int64_t observed_temperature_mc = 0;
  bool has_observed_heat = false;
  std::int64_t observed_heat_mw = 0;
  std::uint64_t observed_at_ns = 0;
  std::uint64_t validity_ns = 0;

  std::uint32_t published_level = 0;
  std::uint32_t published_steps = 0;
  std::uint32_t hold_count = 0;
};

struct Model {
  std::vector<Zone> zones;
  std::vector<Edge> edges;
  std::uint32_t hops = 1;
  std::uint32_t release_hold = 1;
  std::uint64_t now_ns = 0;
  std::uint64_t future_tolerance_ns = 0;
  std::uint64_t evidence_generation = 1;
};

struct Outcome {
  // 0 fresh, 1 stale, 2 future, 3 recovered, 4 missing.
  int freshness = 4;
  bool temperature_known = false;
  bool heat_known = false;
  int heat_source = 0;  // 0 none, 1 observed, 2 declared
  std::int64_t heat_mw = 0;
  bool coupled_known = true;
  std::int64_t coupled_heat_mw = 0;
  std::int64_t coupled_rise_mc = 0;
  std::uint32_t hops_used = 0;
  std::size_t unknown_neighbours = 0;
  bool effective_known = false;
  std::int64_t effective_mc = 0;
  bool band_known = false;
  int band = 1;  // 0 below floor, 1 nominal, 2 derating, 3 at limit, 4 critical
  // 0 known, 1 unknown, 2 indeterminate
  int headroom_status = 1;
  std::int64_t temperature_margin_mc = 0;
  std::int64_t power_margin_mw = 0;
  std::uint32_t instant_level = 0;
  std::uint32_t published_level = 0;
  std::uint32_t hold_count = 0;
  std::int64_t derate_ppm = 0;
  bool allowance_known = false;
  std::int64_t allowance_mw = 0;
  // 0 bounded, 1 prohibited, 2 indeterminate
  int constraint = 2;
};

std::uint64_t abs_diff(std::uint64_t a, std::uint64_t b);

// Evaluates the whole model. Results are returned in the same order as
// model.zones.
std::vector<Outcome> evaluate(const Model& model);

}  // namespace reference
}  // namespace tzm_test

#endif  // TZM_TEST_REFERENCE_MODEL_HPP
