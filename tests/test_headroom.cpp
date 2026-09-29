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


#include <string>

#include "test_harness.hpp"
#include "thermal_zone_manager/thermal_zone_manager.hpp"

using namespace thermal_zone_manager;

TZM_TEST(headroom, only_known_is_proven) {
  TZM_CHECK(is_proven(HeadroomStatus::Known));
  TZM_CHECK(!is_proven(HeadroomStatus::Unknown));
  TZM_CHECK(!is_proven(HeadroomStatus::Indeterminate));
  TZM_CHECK_EQ(std::string(to_string(HeadroomStatus::Known)), std::string("known"));
  TZM_CHECK_EQ(std::string(to_string(HeadroomStatus::Unknown)), std::string("unknown"));
  TZM_CHECK_EQ(std::string(to_string(HeadroomStatus::Indeterminate)),
               std::string("indeterminate"));
}

TZM_TEST(headroom, over_envelope_is_explicit_and_not_clamped) {
  ThermalHeadroom headroom;
  headroom.status = HeadroomStatus::Known;
  headroom.temperature_margin = MilliCelsius{-3000};
  headroom.power_margin = MilliWatts{-250000};
  TZM_CHECK(headroom.over_envelope());
  TZM_CHECK_EQ(headroom.temperature_margin.value, -3000);
  TZM_CHECK_EQ(headroom.power_margin.value, -250000);

  headroom.temperature_margin = MilliCelsius{0};
  TZM_CHECK(!headroom.over_envelope());

  headroom.status = HeadroomStatus::Unknown;
  headroom.temperature_margin = MilliCelsius{-3000};
  TZM_CHECK(!headroom.over_envelope());
}

TZM_TEST(headroom, unspecified_status_defaults_to_unknown) {
  const ThermalHeadroom headroom;
  TZM_CHECK(headroom.status == HeadroomStatus::Unknown);
  TZM_CHECK(!is_proven(headroom.status));
  TZM_CHECK_EQ(headroom.temperature_margin.value, 0);
  TZM_CHECK_EQ(headroom.power_margin.value, 0);
}
