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

#include "reference_model.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace tzm_test {
namespace reference {
namespace {

constexpr std::int64_t kPpm = 1000000;

std::uint32_t instant_level(const Zone& zone, std::int64_t temperature) {
  const std::uint64_t steps = zone.derate_steps;
  if (steps == 0 || temperature <= zone.onset_mc) {
    return 0;
  }
  if (temperature >= zone.ceiling_mc) {
    return static_cast<std::uint32_t>(steps);
  }
  const std::uint64_t delta = static_cast<std::uint64_t>(temperature - zone.onset_mc);
  const std::uint64_t span = static_cast<std::uint64_t>(zone.ceiling_mc - zone.onset_mc);
  const std::uint64_t level = ref_mul_div_ceil(delta, steps, span);
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(level, steps));
}

std::int64_t fraction_ppm(std::uint32_t level, std::uint32_t steps) {
  if (steps == 0 || level == 0) {
    return 0;
  }
  if (level >= steps) {
    return kPpm;
  }
  return static_cast<std::int64_t>(
      ref_mul_div_ceil(static_cast<std::uint64_t>(level), static_cast<std::uint64_t>(kPpm),
                       static_cast<std::uint64_t>(steps)));
}

std::int64_t level_floor(const Zone& zone, std::uint32_t level) {
  if (level == 0 || zone.derate_steps == 0) {
    return zone.onset_mc;
  }
  const std::uint32_t bounded = std::min(level, zone.derate_steps);
  const std::uint64_t offset = static_cast<std::uint64_t>(bounded) - 1U;
  const std::uint64_t span = static_cast<std::uint64_t>(zone.ceiling_mc - zone.onset_mc);
  const std::uint64_t climb =
      ref_mul_div_floor(offset, span, static_cast<std::uint64_t>(zone.derate_steps));
  return zone.onset_mc + static_cast<std::int64_t>(climb);
}

int classify(const Zone& zone, std::int64_t temperature) {
  if (temperature < zone.floor_mc) {
    return 0;
  }
  if (temperature < zone.onset_mc) {
    return 1;
  }
  if (temperature < zone.ceiling_mc) {
    return 2;
  }
  if (temperature < zone.critical_mc) {
    return 3;
  }
  return 4;
}

}  // namespace

std::uint64_t abs_diff(std::uint64_t a, std::uint64_t b) { return a > b ? a - b : b - a; }

std::vector<Outcome> evaluate(const Model& model) {
  const std::size_t count = model.zones.size();
  std::vector<Outcome> outcomes(count);

  // Freshness, in the documented decision order.
  for (std::size_t index = 0; index < count; ++index) {
    const Zone& zone = model.zones[index];
    Outcome& outcome = outcomes[index];
    if (!zone.has_observation) {
      outcome.freshness = 4;
    } else if (zone.observation_evidence_generation != model.evidence_generation) {
      outcome.freshness = 1;
    } else if (zone.recovered) {
      outcome.freshness = 3;
    } else if (zone.observed_at_ns > model.now_ns + model.future_tolerance_ns) {
      outcome.freshness = 2;
    } else if (model.now_ns < zone.observed_at_ns) {
      outcome.freshness = 2;
    } else if (model.now_ns - zone.observed_at_ns > zone.validity_ns) {
      outcome.freshness = 1;
    } else {
      outcome.freshness = 0;
    }
    outcome.temperature_known = outcome.freshness == 0;
    if (outcome.freshness == 0 && zone.has_observed_heat) {
      outcome.heat_known = true;
      outcome.heat_source = 1;
      outcome.heat_mw = zone.observed_heat_mw;
    } else if (zone.has_declared_heat) {
      outcome.heat_known = true;
      outcome.heat_source = 2;
      outcome.heat_mw = zone.declared_heat_mw;
    } else {
      outcome.heat_known = false;
      outcome.heat_source = 0;
      outcome.heat_mw = 0;
    }
  }

  // The coupling series, applied hop by hop with a linear neighbour scan.
  std::vector<std::int64_t> frontier(count, 0);
  std::vector<bool> frontier_known(count, true);
  for (std::size_t index = 0; index < count; ++index) {
    if (outcomes[index].heat_known) {
      frontier[index] = outcomes[index].heat_mw;
    } else {
      frontier_known[index] = false;
    }
  }
  std::vector<std::int64_t> total(count, 0);
  std::vector<bool> total_known(count, true);

  std::uint32_t hops = model.hops;
  if (hops > 1) {
    std::int64_t worst = 0;
    for (std::size_t sink = 0; sink < count; ++sink) {
      std::int64_t incoming = 0;
      for (const Edge& edge : model.edges) {
        if (edge.sink == model.zones[sink].id) {
          incoming += edge.coefficient_ppm;
        }
      }
      worst = std::max(worst, incoming);
    }
    if (worst >= kPpm) {
      hops = 1;
    }
  }

  for (std::uint32_t hop = 0; hop < hops; ++hop) {
    std::vector<std::int64_t> next(count, 0);
    std::vector<bool> next_known(count, true);
    for (std::size_t sink = 0; sink < count; ++sink) {
      std::int64_t sum = 0;
      bool known = true;
      for (std::size_t source = 0; source < count; ++source) {
        std::int64_t coefficient = 0;
        for (const Edge& edge : model.edges) {
          if (edge.source == model.zones[source].id && edge.sink == model.zones[sink].id) {
            coefficient += edge.coefficient_ppm;
          }
        }
        if (coefficient == 0) {
          continue;
        }
        if (!frontier_known[source]) {
          known = false;
          continue;
        }
        if (frontier[source] <= 0) {
          continue;
        }
        sum += static_cast<std::int64_t>(ref_mul_div_ceil(
            static_cast<std::uint64_t>(frontier[source]),
            static_cast<std::uint64_t>(coefficient), static_cast<std::uint64_t>(kPpm)));
      }
      next[sink] = sum;
      next_known[sink] = known;
      if (known) {
        total[sink] += sum;
      } else {
        total_known[sink] = false;
      }
    }
    frontier = next;
    frontier_known = next_known;
  }

  for (std::size_t index = 0; index < count; ++index) {
    const Zone& zone = model.zones[index];
    Outcome& outcome = outcomes[index];
    outcome.hops_used = hops;
    outcome.coupled_known = total_known[index];
    outcome.coupled_heat_mw = total[index];
    // Distinct declared neighbours whose heat load is unknown and whose
    // coupling coefficient into this zone is non-zero.
    for (std::size_t source = 0; source < count; ++source) {
      if (outcomes[source].heat_known) {
        continue;
      }
      for (const Edge& edge : model.edges) {
        if (edge.source == model.zones[source].id && edge.sink == zone.id &&
            edge.coefficient_ppm != 0) {
          outcome.unknown_neighbours += 1;
          break;
        }
      }
    }
    outcome.coupled_rise_mc =
        total[index] == 0
            ? 0
            : ref_ceil_mul_div(total[index],
                               static_cast<std::uint64_t>(zone.resistance_uk_per_w), 1000000U);
    if (outcome.temperature_known && total_known[index]) {
      outcome.effective_known = true;
      outcome.effective_mc = zone.observed_temperature_mc + outcome.coupled_rise_mc;
      outcome.band = classify(zone, outcome.effective_mc);
      outcome.band_known = true;
    }

    if (!outcome.temperature_known) {
      outcome.headroom_status = 1;
    } else if (!outcome.effective_known) {
      outcome.headroom_status = 2;
    } else {
      outcome.headroom_status = 0;
      outcome.temperature_margin_mc = zone.ceiling_mc - outcome.effective_mc;
      outcome.power_margin_mw =
          ref_floor_mul_div(outcome.temperature_margin_mc, 1000000U,
                            static_cast<std::uint64_t>(zone.resistance_uk_per_w));
    }

    // Hysteresis.
    std::uint32_t published =
        std::min(zone.published_level, zone.derate_steps);
    std::uint32_t hold = zone.published_steps == zone.derate_steps ? zone.hold_count : 0;
    if (!outcome.effective_known) {
      hold = 0;
    } else {
      const std::uint32_t computed = instant_level(zone, outcome.effective_mc);
      outcome.instant_level = computed;
      if (computed > published) {
        published = computed;
        hold = 0;
      } else if (computed < published) {
        const std::int64_t threshold = level_floor(zone, published);
        if (outcome.effective_mc <= threshold - zone.recovery_margin_mc) {
          hold += 1;
          if (hold >= model.release_hold) {
            published -= 1;
            hold = 0;
          }
        } else {
          hold = 0;
        }
      } else {
        hold = 0;
      }
    }
    outcome.published_level = published;
    outcome.hold_count = hold;
    outcome.derate_ppm = fraction_ppm(published, zone.derate_steps);

    if (outcome.headroom_status != 0) {
      outcome.allowance_known = false;
      outcome.allowance_mw = 0;
      outcome.constraint = 2;
    } else if (outcome.temperature_margin_mc <= 0 || outcome.power_margin_mw <= 0) {
      outcome.allowance_known = true;
      outcome.allowance_mw = 0;
      outcome.constraint = 1;
    } else {
      const std::uint64_t remaining =
          static_cast<std::uint64_t>(kPpm - outcome.derate_ppm);
      outcome.allowance_known = true;
      outcome.allowance_mw =
          ref_floor_mul_div(outcome.power_margin_mw, remaining, static_cast<std::uint64_t>(kPpm));
      outcome.constraint = outcome.allowance_mw > 0 ? 0 : 1;
    }
  }

  return outcomes;
}

}  // namespace reference
}  // namespace tzm_test
