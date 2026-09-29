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

#include "test_harness.hpp"
#include "thermal_zone_manager/thermal_zone_manager.hpp"

using namespace thermal_zone_manager;

TZM_TEST(errors, code_names_are_unique_and_stable) {
  std::set<std::string> names;
  const std::vector<ValidationPhase>& phases = validation_precedence();
  TZM_CHECK(!phases.empty());
  for (const ValidationPhase& phase : phases) {
    for (std::uint32_t value = static_cast<std::uint32_t>(phase.first);
         value <= static_cast<std::uint32_t>(phase.last); ++value) {
      const auto code = static_cast<ErrorCode>(value);
      const std::string name(to_string(code));
      TZM_CHECK_NE(name, std::string("unknown_error_code"));
      TZM_CHECK(names.insert(name).second);
    }
  }
  TZM_CHECK_EQ(std::string(to_string(ErrorCode::Ok)), std::string("ok"));
  TZM_CHECK_EQ(std::string(to_string(ErrorCode::StaleConfigurationGeneration)),
               std::string("stale_configuration_generation"));
  TZM_CHECK_EQ(std::string(to_string(static_cast<ErrorCode>(999999))),
               std::string("unknown_error_code"));
}

TZM_TEST(errors, precedence_phases_are_ordered_and_disjoint) {
  const std::vector<ValidationPhase>& phases = validation_precedence();
  std::uint32_t previous_last = 0;
  std::uint32_t expected_index = 1;
  for (const ValidationPhase& phase : phases) {
    TZM_CHECK_EQ(phase.index, expected_index);
    ++expected_index;
    const std::uint32_t first = static_cast<std::uint32_t>(phase.first);
    const std::uint32_t last = static_cast<std::uint32_t>(phase.last);
    TZM_CHECK(first <= last);
    TZM_CHECK(first > previous_last);
    previous_last = last;
    for (std::uint32_t value = first; value <= last; ++value) {
      const auto code = static_cast<ErrorCode>(value);
      TZM_REQUIRE(validation_phase(code).has_value());
      TZM_CHECK_EQ(validation_phase(code).value(), phase.index);
    }
  }
  TZM_CHECK(!validation_phase(ErrorCode::Ok).has_value());
}

TZM_TEST(errors, transient_classification_is_explicit) {
  TZM_CHECK(is_transient(ErrorCode::StoreBusy));
  TZM_CHECK(is_transient(ErrorCode::StoreLocked));
  TZM_CHECK(!is_transient(ErrorCode::StaleConfigurationGeneration));
  TZM_CHECK(!is_transient(ErrorCode::EpochMismatch));
  TZM_CHECK(!is_transient(ErrorCode::IdempotencyConflict));
  TZM_CHECK(!is_transient(ErrorCode::Ok));
}

TZM_TEST(errors, error_carries_code_message_and_context) {
  Error error(ErrorCode::StaleZoneGeneration, "the zone moved on");
  error.with("zone", static_cast<std::uint64_t>(3)).with("detail", std::string("x"));
  TZM_CHECK(error.code() == ErrorCode::StaleZoneGeneration);
  TZM_CHECK_EQ(error.message(), std::string("the zone moved on"));
  TZM_CHECK_EQ(error.context().size(), static_cast<std::size_t>(2));
  const std::string text = error.to_string();
  TZM_CHECK(text.find("stale_zone_generation") == 0);
  TZM_CHECK(text.find("zone=3") != std::string::npos);
  TZM_CHECK(text.find("detail=x") != std::string::npos);
}

TZM_TEST(errors, result_is_exactly_one_of_value_or_error) {
  Result<int> good(7);
  TZM_CHECK(good.ok());
  TZM_CHECK_EQ(good.value(), 7);
  TZM_CHECK(good.value_or(9) == 7);

  Result<int> bad(Error(ErrorCode::StoreBusy, "busy"));
  TZM_CHECK(!bad.ok());
  TZM_CHECK(bad.error().code() == ErrorCode::StoreBusy);
  TZM_CHECK_EQ(bad.value_or(9), 9);

  const Status fine;
  TZM_CHECK(fine.ok());
  const Status refused = Error(ErrorCode::ReadOnlyStore, "no writer authority");
  TZM_CHECK(!refused.ok());
  TZM_CHECK(refused.error().code() == ErrorCode::ReadOnlyStore);
}
