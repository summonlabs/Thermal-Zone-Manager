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


#include <chrono>
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

std::uint64_t counter_value(const std::string& path) {
  Result<std::string> text = tzm_test::read_text_file(path);
  if (!text.ok() || text.value().empty()) {
    return 0;
  }
  std::uint64_t value = 0;
  for (const char item : text.value()) {
    if (item < '0' || item > '9') {
      break;
    }
    value = value * 10 + static_cast<std::uint64_t>(item - '0');
  }
  return value;
}

}  // namespace

TZM_TEST(crash, killing_a_writer_repeatedly_never_leaves_a_hybrid) {
  tzm_test::TempDir directory("crash-writer");
  const std::string counter = directory.path() + "\\counter.txt";

  {
    Result<tzm_test::ChildProcess> initial =
        tzm_test::spawn_child({"tzm_tests", "publish", directory.path()});
    TZM_REQUIRE_OK(initial);
    Result<unsigned long> code = tzm_test::wait_child(initial.value());
    TZM_REQUIRE_OK(code);
    TZM_CHECK_EQ(code.value(), 0UL);
  }
  std::uint64_t baseline = 0;
  {
    EngineOptions options;
    options.root = directory.path();
    options.access = StoreAccess::ReadOnly;
    options.create_if_missing = false;
    Result<std::unique_ptr<ThermalZoneEngine>> engine = ThermalZoneEngine::open(options);
    TZM_REQUIRE_OK(engine);
    baseline = engine.value()->fencing().commit.raw();
    TZM_CHECK(baseline > 0);
  }

  for (int attempt = 1; attempt <= 10; ++attempt) {
    Result<tzm_test::ChildProcess> child =
        tzm_test::spawn_child({"tzm_tests", "commit-loop", directory.path(), counter});
    TZM_REQUIRE_OK(child);
    std::this_thread::sleep_for(std::chrono::milliseconds(5 + attempt * 6));
    TZM_REQUIRE_OK(tzm_test::terminate_child(child.value()));
    // The child writes its commit sequence immediately after each successful
    // commit, so once it is dead the counter is either the last committed
    // sequence or exactly one behind it (if it died between the commit and the
    // counter write). Reading it after the kill, rather than before, is what
    // makes the bound sound.
    const std::uint64_t progress = counter_value(counter);

    EngineOptions options;
    options.root = directory.path();
    options.access = StoreAccess::ReadWrite;
    options.create_if_missing = true;
    Result<std::unique_ptr<ThermalZoneEngine>> recovered = ThermalZoneEngine::open(options);
    TZM_REQUIRE_OK(recovered);
    const FencingState fencing = recovered.value()->fencing();
    // The recovered generation is never behind the last complete one and never
    // ahead of what the child could have published.
    TZM_CHECK(fencing.commit.raw() >= baseline);
    if (progress != 0) {
      TZM_CHECK(fencing.commit.raw() >= progress);
      TZM_CHECK(fencing.commit.raw() <= progress + 1);
    }
    TZM_CHECK(recovered.value()->configuration().zone_count() == 1);
    const ZoneEvidence evidence = recovered.value()->evidence_for(ZoneId::from_value(1));
    if (evidence.present) {
      TZM_CHECK(evidence.recovered);
      TZM_CHECK(!is_usable(evidence.freshness));
    }
    TZM_CHECK_OK(recovered.value()->close());
    baseline = fencing.commit.raw();
  }
}

TZM_TEST(crash, a_writer_that_dies_before_committing_changes_nothing) {
  tzm_test::TempDir directory("crash-before-commit");
  {
    Result<std::unique_ptr<tzm_test::Facility>> facility = tzm_test::Facility::open(
        directory.path(), one_zone(), {}, 1, 3, StoreAccess::ReadWrite, true);
    TZM_REQUIRE_OK(facility);
    TZM_REQUIRE_OK(facility.value()->observe(1, 60000));
  }
  EngineOptions options;
  options.root = directory.path();
  options.access = StoreAccess::ReadOnly;
  options.create_if_missing = false;
  Result<std::unique_ptr<ThermalZoneEngine>> before = ThermalZoneEngine::open(options);
  TZM_REQUIRE_OK(before);
  const std::uint64_t revision = before.value()->fencing().revision.raw();
  const std::uint64_t commit = before.value()->fencing().commit.raw();

  // A child that holds the store open and does nothing is killed; the durable
  // generation must be untouched.
  const std::string ready = directory.path() + "\\ready";
  const std::string stop = directory.path() + "\\stop";
  Result<tzm_test::ChildProcess> child =
      tzm_test::spawn_child({"tzm_tests", "hold-lock", directory.path(), ready, stop});
  TZM_REQUIRE_OK(child);
  while (!tzm_test::file_exists(ready)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  TZM_REQUIRE_OK(tzm_test::terminate_child(child.value()));

  Result<std::unique_ptr<ThermalZoneEngine>> after = ThermalZoneEngine::open(options);
  TZM_REQUIRE_OK(after);
  TZM_CHECK_EQ(after.value()->fencing().revision.raw(), revision);
  TZM_CHECK_EQ(after.value()->fencing().commit.raw(), commit);
  TZM_CHECK_EQ(after.value()->configuration().zone_count(), static_cast<std::size_t>(1));
}

TZM_TEST(crash, staging_files_never_become_authoritative) {
  tzm_test::TempDir directory("crash-staging");
  {
    Result<std::unique_ptr<tzm_test::Facility>> facility = tzm_test::Facility::open(
        directory.path(), one_zone(), {}, 1, 3, StoreAccess::ReadWrite, true);
    TZM_REQUIRE_OK(facility);
    TZM_REQUIRE_OK(facility.value()->observe(1, 60000));
  }
  // A leftover staging file from an interrupted creation must be ignored and
  // must not be mistaken for the store.
  TZM_REQUIRE_OK(tzm_test::write_text_file(
      directory.path() + "\\thermal-zones.stage-1-0", "not a store at all"));
  EngineOptions options;
  options.root = directory.path();
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  Result<std::unique_ptr<ThermalZoneEngine>> engine = ThermalZoneEngine::open(options);
  TZM_REQUIRE_OK(engine);
  TZM_CHECK_EQ(engine.value()->configuration().zone_count(), static_cast<std::size_t>(1));
  TZM_CHECK(engine.value()->recovery_report().recovered);
}
