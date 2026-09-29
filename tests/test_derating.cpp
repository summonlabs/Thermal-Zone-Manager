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
#include <optional>

#include "test_harness.hpp"
#include "thermal_zone_manager/thermal_zone_manager.hpp"

using namespace thermal_zone_manager;

namespace {

TemperatureEnvelope ladder_envelope() {
  TemperatureEnvelope envelope;
  envelope.floor_temp = MilliCelsius{-5000};
  envelope.derate_onset = MilliCelsius{60000};
  envelope.ceiling_temp = MilliCelsius{80000};
  envelope.critical_temp = MilliCelsius{90000};
  return envelope;
}

}  // namespace

TZM_TEST(derating, level_is_zero_at_or_below_the_onset) {
  const TemperatureEnvelope envelope = ladder_envelope();
  TZM_CHECK_EQ(instant_derate_level(envelope, 8, MilliCelsius{-100000}), 0U);
  TZM_CHECK_EQ(instant_derate_level(envelope, 8, MilliCelsius{60000}), 0U);
  TZM_CHECK_EQ(instant_derate_level(envelope, 8, MilliCelsius{60001}), 1U);
}

TZM_TEST(derating, level_saturates_at_the_ceiling) {
  const TemperatureEnvelope envelope = ladder_envelope();
  TZM_CHECK_EQ(instant_derate_level(envelope, 8, MilliCelsius{80000}), 8U);
  TZM_CHECK_EQ(instant_derate_level(envelope, 8, MilliCelsius{90000}), 8U);
  TZM_CHECK_EQ(instant_derate_level(envelope, 1, MilliCelsius{80000}), 1U);
  TZM_CHECK_EQ(instant_derate_level(envelope, 1, MilliCelsius{60001}), 1U);
  TZM_CHECK_EQ(instant_derate_level(envelope, 1, MilliCelsius{60000}), 0U);
}

TZM_TEST(derating, level_is_monotone_in_temperature_and_bounded_by_steps) {
  const TemperatureEnvelope envelope = ladder_envelope();
  for (std::uint32_t steps = 1; steps <= 16; ++steps) {
    std::uint32_t previous = 0;
    for (std::int64_t value = 55000; value <= 85000; value += 13) {
      const std::uint32_t level = instant_derate_level(envelope, steps, MilliCelsius{value});
      TZM_CHECK(level >= previous);
      TZM_CHECK(level <= steps);
      previous = level;
    }
  }
}

TZM_TEST(derating, level_floor_is_the_highest_temperature_of_the_previous_level) {
  const TemperatureEnvelope envelope = ladder_envelope();
  for (std::uint32_t steps = 1; steps <= 8; ++steps) {
    for (std::uint32_t level = 1; level <= steps; ++level) {
      const MilliCelsius floor_value = derate_level_floor(envelope, steps, level);
      const MilliCelsius onset = derate_level_onset(envelope, steps, level);
      TZM_CHECK_EQ(onset.value, floor_value.value + 1);
      TZM_CHECK(instant_derate_level(envelope, steps, floor_value) <= level - 1);
      TZM_CHECK(instant_derate_level(envelope, steps, onset) >= level);
      if (floor_value.value + 1 < envelope.ceiling_temp.value) {
        TZM_CHECK(instant_derate_level(envelope, steps,
                                       MilliCelsius{floor_value.value + 1}) == level);
      }
    }
  }
}

TZM_TEST(derating, fraction_is_monotone_and_saturates) {
  TZM_CHECK_EQ(derate_fraction(0, 8).value, 0);
  TZM_CHECK_EQ(derate_fraction(8, 8).value, kPpmScale);
  TZM_CHECK_EQ(derate_fraction(9, 8).value, kPpmScale);
  TZM_CHECK_EQ(derate_fraction(1, 2).value, 500000);
  TZM_CHECK_EQ(derate_fraction(1, 3).value, 333334);
  TZM_CHECK_EQ(derate_fraction(2, 3).value, 666667);
  TZM_CHECK_EQ(derate_fraction(1, 1).value, kPpmScale);
  int previous = -1;
  for (std::uint32_t level = 0; level <= 16; ++level) {
    const int fraction = derate_fraction(level, 16).value;
    TZM_CHECK(fraction >= previous);
    previous = fraction;
  }
}

TZM_TEST(derating, escalation_is_immediate) {
  const TemperatureEnvelope envelope = ladder_envelope();
  DeratingState state;
  const DeratingOutcome first =
      advance_derating(state, envelope, 8, MilliCelsius{1000}, 3, MilliCelsius{75000});
  TZM_CHECK(first.escalated);
  TZM_CHECK(!first.released);
  TZM_CHECK(!first.held_for_evidence);
  const std::uint32_t expected = instant_derate_level(envelope, 8, MilliCelsius{75000});
  TZM_CHECK_EQ(first.state.published_level, expected);
  TZM_CHECK_EQ(first.state.published_steps, 8U);
  TZM_CHECK_EQ(first.state.hold_count, 0U);
}

TZM_TEST(derating, recovery_requires_the_full_hold_at_the_release_threshold) {
  const TemperatureEnvelope envelope = ladder_envelope();
  DeratingState state;
  const DeratingOutcome escalated =
      advance_derating(state, envelope, 8, MilliCelsius{1000}, 3, MilliCelsius{80000});
  TZM_CHECK_EQ(escalated.state.published_level, 8U);

  // A temperature that computes a lower level but sits above the release
  // threshold with margin does not start a recovery.
  const MilliCelsius below_level_7 = derate_level_floor(envelope, 8, 8);
  DeratingOutcome outcome = advance_derating(escalated.state, envelope, 8, MilliCelsius{1000}, 3,
                                             MilliCelsius{below_level_7.value});
  TZM_CHECK_EQ(outcome.state.published_level, 8U);
  TZM_CHECK_EQ(outcome.state.hold_count, 0U);
  TZM_CHECK(!outcome.released);

  const MilliCelsius releasing = MilliCelsius{below_level_7.value - 1000};
  std::uint32_t releases = 0;
  for (int attempt = 0; attempt < 3; ++attempt) {
    outcome = advance_derating(outcome.state, envelope, 8, MilliCelsius{1000}, 3, releasing);
    if (outcome.released) {
      ++releases;
    }
  }
  TZM_CHECK_EQ(releases, 1U);
  TZM_CHECK_EQ(outcome.state.published_level, 7U);
  TZM_CHECK_EQ(outcome.state.hold_count, 0U);

  // Published has now reached the instantaneous level for that temperature, so
  // more observations at the same temperature release nothing: hysteresis never
  // drops below what the current temperature implies.
  for (int attempt = 0; attempt < 6; ++attempt) {
    outcome = advance_derating(outcome.state, envelope, 8, MilliCelsius{1000}, 3, releasing);
  }
  TZM_CHECK_EQ(outcome.state.published_level, 7U);

  // A temperature that implies one further step down releases it after the
  // full hold, one step at a time.
  const MilliCelsius releasing_6 = MilliCelsius{derate_level_floor(envelope, 8, 7).value - 1000};
  for (int attempt = 0; attempt < 3; ++attempt) {
    outcome = advance_derating(outcome.state, envelope, 8, MilliCelsius{1000}, 3, releasing_6);
  }
  TZM_CHECK_EQ(outcome.state.published_level, 6U);
}

TZM_TEST(derating, absent_evidence_never_releases_a_step) {
  const TemperatureEnvelope envelope = ladder_envelope();
  DeratingState state;
  const DeratingOutcome escalated =
      advance_derating(state, envelope, 8, MilliCelsius{1000}, 1, MilliCelsius{80000});
  DeratingState current = escalated.state;
  for (int attempt = 0; attempt < 50; ++attempt) {
    const DeratingOutcome held =
        advance_derating(current, envelope, 8, MilliCelsius{1000}, 1, std::nullopt);
    TZM_CHECK(held.held_for_evidence);
    TZM_CHECK(!held.released);
    TZM_CHECK(!held.escalated);
    TZM_CHECK_EQ(held.state.published_level, 8U);
    TZM_CHECK_EQ(held.state.hold_count, 0U);
    current = held.state;
  }
}

TZM_TEST(derating, clamping_follows_the_ladder_resolution) {
  DeratingState state;
  state.published_level = 8;
  state.published_steps = 8;
  state.hold_count = 2;
  const DeratingState narrowed = clamp_derating(state, 4);
  TZM_CHECK_EQ(narrowed.published_level, 4U);
  TZM_CHECK_EQ(narrowed.published_steps, 4U);
  TZM_CHECK_EQ(narrowed.hold_count, 0U);
  const DeratingState widened = clamp_derating(state, 16);
  TZM_CHECK_EQ(widened.published_level, 8U);
  TZM_CHECK_EQ(widened.published_steps, 16U);
  TZM_CHECK_EQ(widened.hold_count, 0U);
  const DeratingState same = clamp_derating(state, 8);
  TZM_CHECK_EQ(same.hold_count, 2U);
}

TZM_TEST(derating, flapping_is_bounded_by_the_release_hold) {
  // Alternating temperature across the release threshold must not produce more
  // than one release per release_hold observations.
  const TemperatureEnvelope envelope = ladder_envelope();
  const std::uint32_t hold = 4;
  DeratingState state;
  state = advance_derating(state, envelope, 8, MilliCelsius{1000}, hold, MilliCelsius{80000}).state;
  TZM_CHECK_EQ(state.published_level, 8U);
  const MilliCelsius releasing =
      MilliCelsius{derate_level_floor(envelope, 8, 8).value - 1000};
  std::uint32_t releases = 0;
  std::uint32_t observations = 0;
  for (int attempt = 0; attempt < 200; ++attempt) {
    const MilliCelsius temperature =
        (attempt % 2 == 0) ? releasing : MilliCelsius{79999};
    const DeratingOutcome outcome =
        advance_derating(state, envelope, 8, MilliCelsius{1000}, hold, temperature);
    ++observations;
    if (outcome.released) {
      ++releases;
    }
    state = outcome.state;
  }
  TZM_CHECK(releases <= observations / hold + 1U);
  TZM_CHECK_EQ(state.published_level, 8U);
}

TZM_TEST(derating, escalation_outruns_a_pending_recovery) {
  const TemperatureEnvelope envelope = ladder_envelope();
  DeratingState state;
  state = advance_derating(state, envelope, 8, MilliCelsius{1000}, 8, MilliCelsius{65000}).state;
  TZM_CHECK_EQ(state.published_level, instant_derate_level(envelope, 8, MilliCelsius{65000}));

  // One observation below the release threshold starts a recovery but the hold
  // is long, so nothing is released yet.
  const MilliCelsius releasing =
      MilliCelsius{derate_level_floor(envelope, 8, state.published_level).value - 1000};
  DeratingOutcome outcome = advance_derating(state, envelope, 8, MilliCelsius{1000}, 8, releasing);
  TZM_CHECK_EQ(outcome.state.hold_count, 1U);
  TZM_CHECK_EQ(outcome.state.published_level, state.published_level);
  TZM_CHECK(!outcome.released);

  // A warmer observation escalates immediately and discards the pending hold.
  outcome = advance_derating(outcome.state, envelope, 8, MilliCelsius{1000}, 8, MilliCelsius{75000});
  TZM_CHECK(outcome.escalated);
  TZM_CHECK_EQ(outcome.state.published_level, instant_derate_level(envelope, 8, MilliCelsius{75000}));
  TZM_CHECK_EQ(outcome.state.hold_count, 0U);
}
