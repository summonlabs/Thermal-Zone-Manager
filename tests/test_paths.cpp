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


#include <filesystem>
#include <memory>
#include <string>

#include "fixtures.hpp"
#include "test_harness.hpp"
#include "test_process.hpp"
#include "thermal_zone_manager/thermal_zone_manager.hpp"

using namespace thermal_zone_manager;

TZM_TEST(paths, a_relative_root_is_resolved_against_the_working_directory) {
  Result<std::string> resolved = canonical_store_root(".");
  TZM_REQUIRE_OK(resolved);
  TZM_CHECK(!resolved.value().empty());
  Result<std::string> absolute = canonical_store_root(resolved.value());
  TZM_REQUIRE_OK(absolute);
  // Resolving twice is stable, which is what makes the lock identity stable.
  TZM_CHECK_EQ(resolved.value(), absolute.value());
}

TZM_TEST(paths, alternate_separators_resolve_to_one_root) {
  tzm_test::TempDir directory("paths-separators");
  std::string forward = directory.path();
  for (char& item : forward) {
    if (item == '\\') {
      item = '/';
    }
  }
  Result<std::string> native = canonical_store_root(directory.path());
  Result<std::string> alternate = canonical_store_root(forward);
  TZM_REQUIRE_OK(native);
  TZM_REQUIRE_OK(alternate);
  TZM_CHECK_EQ(native.value(), alternate.value());
}

TZM_TEST(paths, traversal_and_device_names_are_refused) {
  TZM_CHECK_ERR(canonical_store_root(".."), ErrorCode::StorePathInvalid);
  TZM_CHECK_ERR(canonical_store_root("a/../b"), ErrorCode::StorePathInvalid);
  TZM_CHECK_ERR(canonical_store_root("C:\\temp\\prn"), ErrorCode::StorePathInvalid);
  TZM_CHECK_ERR(canonical_store_root("C:\\temp\\lpt9.log"), ErrorCode::StorePathInvalid);
  TZM_CHECK_ERR(canonical_store_root("C:\\temp\\aux"), ErrorCode::StorePathInvalid);
  TZM_CHECK_ERR(canonical_store_root("C:\\te<mp"), ErrorCode::StorePathInvalid);
  TZM_CHECK_ERR(canonical_store_root("C:\\temp\\bad|name"), ErrorCode::StorePathInvalid);
}

TZM_TEST(paths, a_file_cannot_be_used_as_a_store_root) {
  tzm_test::TempDir directory("paths-file-root");
  const std::string file = directory.path() + "\\not-a-directory";
  TZM_REQUIRE_OK(tzm_test::write_text_file(file, "x"));
  TZM_CHECK_ERR(canonical_store_root(file), ErrorCode::StoreUnavailable);
}

TZM_TEST(paths, a_restricted_store_file_never_corrupts_the_store) {
  tzm_test::TempDir directory("paths-readonly-file");
  {
    Result<std::unique_ptr<tzm_test::Facility>> facility = tzm_test::Facility::open(
        directory.path(), {}, {}, 1, 3, StoreAccess::ReadWrite, true);
    TZM_REQUIRE_OK(facility);
  }
  const std::string state = directory.path() + "\\thermal-zones.tzm";
  TZM_REQUIRE(tzm_test::file_exists(state));

  std::error_code code;
  std::filesystem::permissions(std::filesystem::path(state),
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::remove, code);
  EngineOptions options;
  options.root = directory.path();
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  {
    Result<std::unique_ptr<ThermalZoneEngine>> restricted = ThermalZoneEngine::open(options);
    if (code) {
      ::tzm_test::report_note("this host refused to change file permissions, so an operating "
                              "system access denial could not be produced");
    } else if (restricted.ok()) {
      // The read-only attribute is not an access control list, so the host may
      // still permit the open. What matters is that the contract holds either
      // way: a refusal is a named refusal, and an accepted open leaves the
      // store structurally intact.
      ::tzm_test::report_note("the host permits writing a read-only attributed file, so the "
                              "access-denial refusal path was not exercised");
      TZM_CHECK(restricted.value()->recovery_report().recovered ||
                restricted.value()->configuration().empty());
    } else {
      TZM_CHECK(restricted.error().code() == ErrorCode::IoFailure ||
                restricted.error().code() == ErrorCode::StoreUnavailable);
    }
  }
  std::filesystem::permissions(std::filesystem::path(state),
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::add, code);
  Result<std::unique_ptr<ThermalZoneEngine>> restored = ThermalZoneEngine::open(options);
  TZM_REQUIRE_OK(restored);
  TZM_CHECK_OK(restored.value()->close());
}

TZM_TEST(paths, a_linked_root_resolves_to_the_same_store) {
  tzm_test::TempDir directory("paths-link");
  {
    Result<std::unique_ptr<tzm_test::Facility>> facility = tzm_test::Facility::open(
        directory.path(), {}, {}, 1, 3, StoreAccess::ReadWrite, true);
    TZM_REQUIRE_OK(facility);
  }
  const std::string link = directory.path() + "-link";
  std::error_code code;
  std::filesystem::create_directory_symlink(std::filesystem::path(directory.path()),
                                            std::filesystem::path(link), code);
  if (code) {
    ::tzm_test::report_note("this host does not permit directory symbolic links, so the "
                            "reparse-point identity check was not exercised");
    return;
  }
  Result<std::string> direct = canonical_store_root(directory.path());
  Result<std::string> through_link = canonical_store_root(link);
  TZM_REQUIRE_OK(direct);
  TZM_REQUIRE_OK(through_link);
  // Both names must resolve to one identity, or two processes could hold two
  // different locks for the same logical store.
  TZM_CHECK_EQ(direct.value(), through_link.value());
  std::filesystem::remove(std::filesystem::path(link), code);
}
TZM_TEST(paths, a_missing_root_is_reported_before_anything_is_created) {
  tzm_test::TempDir directory("paths-missing-root");
  const std::string absent = directory.path() + "\\not-there";
  TZM_CHECK_ERR(canonical_store_root(absent), ErrorCode::StoreUnavailable);
  TZM_CHECK(!tzm_test::file_exists(absent));
}
