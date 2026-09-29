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

}  // namespace

TZM_TEST(writer_lock, a_second_process_cannot_take_the_writer_lock) {
  tzm_test::TempDir directory("writer-lock-exclusion");
  Result<std::unique_ptr<tzm_test::Facility>> holder = tzm_test::Facility::open(
      directory.path(), one_zone(), {}, 1, 3, StoreAccess::ReadWrite, true);
  TZM_REQUIRE_OK(holder);
  TZM_CHECK(holder.value()->engine().holds_writer());

  const std::string output = directory.path() + "\\probe.txt";
  tzm_test::ChildProcess child;
  {
    Result<tzm_test::ChildProcess> spawned =
        tzm_test::spawn_child({"tzm_tests", "try-lock", directory.path(), output});
    TZM_REQUIRE_OK(spawned);
    child = spawned.value();
  }
  Result<unsigned long> code = tzm_test::wait_child(child);
  TZM_REQUIRE_OK(code);
  TZM_CHECK_EQ(code.value(), 0UL);
  Result<std::string> text = tzm_test::read_text_file(output);
  TZM_REQUIRE_OK(text);
  TZM_CHECK_EQ(text.value(), std::string("refused store_locked"));
}

TZM_TEST(writer_lock, releasing_the_lock_lets_another_process_in) {
  tzm_test::TempDir directory("writer-lock-release");
  {
    Result<std::unique_ptr<tzm_test::Facility>> holder = tzm_test::Facility::open(
        directory.path(), one_zone(), {}, 1, 3, StoreAccess::ReadWrite, true);
    TZM_REQUIRE_OK(holder);
    TZM_CHECK_OK(holder.value()->engine().close());
    TZM_CHECK(!holder.value()->engine().holds_writer());
  }
  const std::string output = directory.path() + "\\probe.txt";
  tzm_test::ChildProcess child;
  {
    Result<tzm_test::ChildProcess> spawned =
        tzm_test::spawn_child({"tzm_tests", "try-lock", directory.path(), output});
    TZM_REQUIRE_OK(spawned);
    child = spawned.value();
  }
  Result<unsigned long> code = tzm_test::wait_child(child);
  TZM_REQUIRE_OK(code);
  TZM_CHECK_EQ(code.value(), 0UL);
  Result<std::string> text = tzm_test::read_text_file(output);
  TZM_REQUIRE_OK(text);
  TZM_CHECK_EQ(text.value(), std::string("acquired"));
}

TZM_TEST(writer_lock, a_child_holding_the_lock_blocks_a_local_writer) {
  tzm_test::TempDir directory("writer-lock-child");
  const std::string ready = directory.path() + "\\ready";
  const std::string stop = directory.path() + "\\stop";
  tzm_test::ChildProcess child;
  {
    Result<tzm_test::ChildProcess> spawned =
        tzm_test::spawn_child({"tzm_tests", "hold-lock", directory.path(), ready, stop});
    TZM_REQUIRE_OK(spawned);
    child = spawned.value();
  }
  // Wait for the child to report that it holds the lock.
  while (!tzm_test::file_exists(ready)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  EngineOptions options;
  options.root = directory.path();
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  Result<std::unique_ptr<ThermalZoneEngine>> blocked = ThermalZoneEngine::open(options);
  TZM_CHECK_ERR(blocked, ErrorCode::StoreLocked);

  // A read-only open is still allowed while the writer lock is held.
  options.access = StoreAccess::ReadOnly;
  Result<std::unique_ptr<ThermalZoneEngine>> reader = ThermalZoneEngine::open(options);
  TZM_REQUIRE_OK(reader);
  TZM_CHECK(!reader.value()->holds_writer());

  TZM_REQUIRE_OK(tzm_test::write_text_file(stop, "stop"));
  Result<unsigned long> code = tzm_test::wait_child(child);
  TZM_REQUIRE_OK(code);
  TZM_CHECK_EQ(code.value(), 0UL);
}

TZM_TEST(writer_lock, a_wait_budget_is_honoured) {
  tzm_test::TempDir directory("writer-lock-wait");
  const std::string ready = directory.path() + "\\ready";
  const std::string stop = directory.path() + "\\stop";
  tzm_test::ChildProcess child;
  {
    Result<tzm_test::ChildProcess> spawned =
        tzm_test::spawn_child({"tzm_tests", "hold-lock", directory.path(), ready, stop});
    TZM_REQUIRE_OK(spawned);
    child = spawned.value();
  }
  while (!tzm_test::file_exists(ready)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  // Release the child from another thread shortly after the wait starts, so the
  // waiting writer must succeed rather than fail.
  std::thread releaser([&stop]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    tzm_test::write_text_file(stop, "stop");
  });

  EngineOptions options;
  options.root = directory.path();
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  options.lock_wait = Nanoseconds{5000000000LL};
  Result<std::unique_ptr<ThermalZoneEngine>> engine = ThermalZoneEngine::open(options);
  releaser.join();
  TZM_REQUIRE_OK(engine);
  TZM_CHECK(engine.value()->holds_writer());
  Result<unsigned long> code = tzm_test::wait_child(child);
  TZM_REQUIRE_OK(code);
}

TZM_TEST(writer_lock, repeated_open_and_close_is_safe) {
  tzm_test::TempDir directory("writer-lock-repeat");
  for (int attempt = 0; attempt < 6; ++attempt) {
    Result<std::unique_ptr<tzm_test::Facility>> facility = tzm_test::Facility::open(
        directory.path(), one_zone(), {}, 1, 3, StoreAccess::ReadWrite, true);
    TZM_REQUIRE_OK(facility);
    TZM_CHECK(facility.value()->engine().holds_writer());
    TZM_REQUIRE_OK(facility.value()->observe(1, 60000 + attempt * 100));
    TZM_CHECK_OK(facility.value()->engine().close());
  }
  Result<std::unique_ptr<tzm_test::Facility>> final_open = tzm_test::Facility::open(
      directory.path(), {}, {}, 1, 3, StoreAccess::ReadWrite, true);
  TZM_REQUIRE_OK(final_open);
  TZM_CHECK_EQ(final_open.value()->engine().recovery_report().observation_count,
               static_cast<std::size_t>(1));
}
