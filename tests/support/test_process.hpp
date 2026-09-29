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

#ifndef TZM_TEST_PROCESS_HPP
#define TZM_TEST_PROCESS_HPP

#include <string>
#include <vector>

#include "thermal_zone_manager/errors.hpp"

namespace tzm_test {

using thermal_zone_manager::Result;
using thermal_zone_manager::Status;

// A real operating-system process started from the test binary itself. Child
// standard streams are not inherited or piped: children communicate through
// files, which keeps the harness free of pipe deadlocks.
struct ChildProcess {
  void* handle = nullptr;
  unsigned long pid = 0;
  bool running = false;
};

std::string executable_path();

Result<ChildProcess> spawn_child(const std::vector<std::string>& arguments);

// Waits for the child to finish and returns its exit code. There is no
// timeout: a child that does not finish is a defect in the test, not something
// to bound.
Result<unsigned long> wait_child(ChildProcess& child);

// Kills the child immediately and returns its exit code.
Result<unsigned long> terminate_child(ChildProcess& child);

// Releases the handle of a child that has already been waited for.
void release_child(ChildProcess& child);

bool file_exists(const std::string& path);

Status write_text_file(const std::string& path, const std::string& text);
Result<std::string> read_text_file(const std::string& path);
Status make_directory(const std::string& path);
Status remove_tree(const std::string& path);

// Returns -1 when the process is not running in a child mode.
int run_child_mode(int argc, char** argv);

// Child modes, exposed so tests can name them.
Status child_hold_lock(const std::string& directory, const std::string& ready_file,
                       const std::string& stop_file);
Status child_try_lock(const std::string& directory, const std::string& output_file);
Status child_commit_loop(const std::string& directory, const std::string& counter_file);
Status child_read_state(const std::string& directory, const std::string& output_file);
Status child_publish_state(const std::string& directory);
Status child_publish_and_hold(const std::string& directory, const std::string& ready_file,
                              const std::string& stop_file);

}  // namespace tzm_test

#endif  // TZM_TEST_PROCESS_HPP
