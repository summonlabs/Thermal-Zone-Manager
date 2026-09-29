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

#include "test_harness.hpp"
#include "thermal_zone_manager/thermal_zone_manager.hpp"

using namespace thermal_zone_manager;

namespace {

TemperatureEnvelope make_envelope(std::int64_t floor_mc, std::int64_t onset_mc,
                                  std::int64_t ceiling_mc, std::int64_t critical_mc) {
  TemperatureEnvelope envelope;
  envelope.floor_temp = MilliCelsius{floor_mc};
  envelope.derate_onset = MilliCelsius{onset_mc};
  envelope.ceiling_temp = MilliCelsius{ceiling_mc};
  envelope.critical_temp = MilliCelsius{critical_mc};
  return envelope;
}

TemperatureEnvelope valid_envelope() { return make_envelope(-5000, 60000, 80000, 90000); }

}  // namespace

TZM_TEST(envelope, accepts_a_well_ordered_envelope) {
  const TemperatureEnvelope envelope = valid_envelope();
  TZM_CHECK_OK(envelope.validate());
  TZM_CHECK_EQ(envelope.derate_span(), 20000);
}

TZM_TEST(envelope, rejects_every_broken_ordering) {
  // Floor equal to the derating onset leaves no nominal band.
  const Status equal_floor = make_envelope(60000, 60000, 80000, 90000).validate();
  TZM_CHECK_ERR(equal_floor, ErrorCode::InvalidEnvelope);
  // Derating onset equal to the ceiling leaves no ramp to divide.
  const Status equal_ceiling = make_envelope(-5000, 80000, 80000, 90000).validate();
  TZM_CHECK_ERR(equal_ceiling, ErrorCode::InvalidEnvelope);
  // A critical threshold below the ceiling is contradictory.
  const Status inverted_critical = make_envelope(-5000, 60000, 80000, 70000).validate();
  TZM_CHECK_ERR(inverted_critical, ErrorCode::ContradictoryLimits);
  // Reversed ordering.
  const Status reversed = make_envelope(90000, 60000, 80000, 90000).validate();
  TZM_CHECK_ERR(reversed, ErrorCode::InvalidEnvelope);
}

TZM_TEST(envelope, rejects_implausible_bounds_before_ordering) {
  // An implausible bound is reported as a temperature fault even when the
  // ordering is also broken, because plausibility is checked first.
  const Status too_cold = make_envelope(-273151, -273150, -273149, -273148).validate();
  TZM_CHECK_ERR(too_cold, ErrorCode::InvalidTemperature);
  const Status too_hot =
      make_envelope(-5000, 60000, 80000, kMaxPlausibleMilliCelsius + 1).validate();
  TZM_CHECK_ERR(too_hot, ErrorCode::InvalidTemperature);
}

TZM_TEST(envelope, classifies_boundary_temperatures_exactly) {
  const TemperatureEnvelope envelope = valid_envelope();
  TZM_CHECK(classify(envelope, MilliCelsius{-5001}) == ThermalBand::BelowFloor);
  TZM_CHECK(classify(envelope, MilliCelsius{-5000}) == ThermalBand::Nominal);
  TZM_CHECK(classify(envelope, MilliCelsius{59999}) == ThermalBand::Nominal);
  TZM_CHECK(classify(envelope, MilliCelsius{60000}) == ThermalBand::Derating);
  TZM_CHECK(classify(envelope, MilliCelsius{79999}) == ThermalBand::Derating);
  TZM_CHECK(classify(envelope, MilliCelsius{80000}) == ThermalBand::AtLimit);
  TZM_CHECK(classify(envelope, MilliCelsius{89999}) == ThermalBand::AtLimit);
  TZM_CHECK(classify(envelope, MilliCelsius{90000}) == ThermalBand::Critical);
  TZM_CHECK(classify(envelope, MilliCelsius{1000000}) == ThermalBand::Critical);
}

TZM_TEST(envelope, classification_is_monotone_in_temperature) {
  const TemperatureEnvelope envelope = valid_envelope();
  int previous = -1;
  for (std::int64_t value = -20000; value <= 120000; value += 137) {
    const int band = static_cast<int>(classify(envelope, MilliCelsius{value}));
    TZM_CHECK(band >= previous);
    previous = band;
  }
}

TZM_TEST(envelope, containment_is_the_closed_interval) {
  const TemperatureEnvelope envelope = valid_envelope();
  TZM_CHECK(is_within_envelope(envelope, MilliCelsius{-5000}));
  TZM_CHECK(is_within_envelope(envelope, MilliCelsius{80000}));
  TZM_CHECK(!is_within_envelope(envelope, MilliCelsius{-5001}));
  TZM_CHECK(!is_within_envelope(envelope, MilliCelsius{80001}));
}

TZM_TEST(envelope, band_names_are_stable) {
  TZM_CHECK_EQ(std::string(to_string(ThermalBand::BelowFloor)), std::string("below_floor"));
  TZM_CHECK_EQ(std::string(to_string(ThermalBand::Nominal)), std::string("nominal"));
  TZM_CHECK_EQ(std::string(to_string(ThermalBand::Derating)), std::string("derating"));
  TZM_CHECK_EQ(std::string(to_string(ThermalBand::AtLimit)), std::string("at_limit"));
  TZM_CHECK_EQ(std::string(to_string(ThermalBand::Critical)), std::string("critical"));
}
