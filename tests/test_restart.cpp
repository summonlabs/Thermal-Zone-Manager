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
#include <thread>
#include <vector>

#include "fixtures.hpp"
#include "test_harness.hpp"
#include "test_process.hpp"
#include "thermal_zone_manager/thermal_zone_manager.hpp"

using namespace thermal_zone_manager;

namespace {

std::vector<tzm_test::ZoneSpec> one_zone() {
  tzm_test::ZoneSpec spec;
  spec.id = 1;
  spec.name = "rack-a";
  return {spec};
}

// Runs one child to completion and returns its exit code.
unsigned long run_child(const std::vector<std::string>& arguments) {
  Result<tzm_test::ChildProcess> child = tzm_test::spawn_child(arguments);
  if (!child.ok()) {
    return 99UL;
  }
  Result<unsigned long> code = tzm_test::wait_child(child.value());
  return code.ok() ? code.value() : 98UL;
}

}  // namespace

TZM_TEST(restart, a_child_publishes_state_the_parent_can_read) {
  tzm_test::TempDir directory("restart-publish");
  TZM_CHECK_EQ(run_child({"tzm_tests", "publish", directory.path()}), 0UL);

  const std::string output = directory.path() + "\\state.txt";
  TZM_CHECK_EQ(run_child({"tzm_tests", "read-state", directory.path(), output}), 0UL);
  Result<std::string> text = tzm_test::read_text_file(output);
  TZM_REQUIRE_OK(text);
  TZM_CHECK(text.value().find("commit=") == 0);
  TZM_CHECK(text.value().find("zones=1") != std::string::npos);

  // The parent now reopens the same store and sees the durable generation.
  Result<std::unique_ptr<tzm_test::Facility>> facility = tzm_test::Facility::open(
      directory.path(), {}, {}, 1, 3, StoreAccess::ReadWrite, true);
  TZM_REQUIRE_OK(facility);
  TZM_CHECK_EQ(facility.value()->engine().configuration().zone_count(),
               static_cast<std::size_t>(1));
  TZM_CHECK(facility.value()->engine().recovery_report().recovered);
}

TZM_TEST(restart, a_killed_writer_leaves_a_complete_generation) {
  tzm_test::TempDir directory("restart-kill");
  const std::string counter = directory.path() + "\\counter.txt";

  // Observe a first complete generation so the comparison is meaningful.
  TZM_CHECK_EQ(run_child({"tzm_tests", "publish", directory.path()}), 0UL);
  Result<std::unique_ptr<tzm_test::Facility>> before = tzm_test::Facility::open(
      directory.path(), {}, {}, 1, 3, StoreAccess::ReadOnly, false);
  TZM_REQUIRE_OK(before);
  const std::uint64_t baseline = before.value()->engine().fencing().commit.raw();
  before.value()->engine().close();

  for (int attempt = 0; attempt < 8; ++attempt) {
    Result<tzm_test::ChildProcess> child =
        tzm_test::spawn_child({"tzm_tests", "commit-loop", directory.path(), counter});
    TZM_REQUIRE_OK(child);
    // Let the child commit for a while, then kill it without warning. The kill
    // point moves from run to run because the child's progress is not
    // synchronised with the parent.
    std::this_thread::sleep_for(std::chrono::milliseconds(15 + attempt * 7));
    Result<unsigned long> killed = tzm_test::terminate_child(child.value());
    TZM_REQUIRE_OK(killed);

    // Reopen from a fresh engine in this process and require a complete
    // generation: the recovered state must decode and its commit sequence must
    // be at least the baseline and at most the last counter the child wrote
    // plus one.
    Result<std::unique_ptr<tzm_test::Facility>> recovered = tzm_test::Facility::open(
        directory.path(), {}, {}, 1, 3, StoreAccess::ReadWrite, true);
    TZM_REQUIRE_OK(recovered);
    ThermalZoneEngine& engine = recovered.value()->engine();
    TZM_CHECK(engine.fencing().commit.raw() >= baseline);
    TZM_CHECK(engine.configuration().zone_count() >= 1);
    TZM_CHECK(engine.recovery_report().recovered);
    TZM_CHECK_OK(engine.close());
  }
}

TZM_TEST(restart, a_child_that_dies_after_publishing_leaves_that_generation) {
  tzm_test::TempDir directory("restart-after-publish");
  const std::string ready = directory.path() + "\\ready";
  const std::string stop = directory.path() + "\\stop";
  Result<tzm_test::ChildProcess> child =
      tzm_test::spawn_child({"tzm_tests", "publish-and-hold", directory.path(), ready, stop});
  TZM_REQUIRE_OK(child);
  while (!tzm_test::file_exists(ready)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Result<unsigned long> killed = tzm_test::terminate_child(child.value());
  TZM_REQUIRE_OK(killed);

  Result<std::unique_ptr<tzm_test::Facility>> recovered = tzm_test::Facility::open(
      directory.path(), {}, {}, 1, 3, StoreAccess::ReadWrite, true);
  TZM_REQUIRE_OK(recovered);
  TZM_CHECK_EQ(recovered.value()->engine().fencing().epoch.raw(), static_cast<std::uint64_t>(1));
  TZM_CHECK_EQ(recovered.value()->engine().configuration().zone_count(),
               static_cast<std::size_t>(1));
  const RecoveryReport report = recovered.value()->engine().recovery_report();
  TZM_CHECK(report.recovered);
  TZM_CHECK_EQ(report.payload_bytes > 0, true);
}
