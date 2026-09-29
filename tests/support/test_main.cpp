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

#include "test_harness.hpp"
#include "test_process.hpp"

// The test binary is also its own child-process helper. When the first argument
// names a child mode the process runs that mode and never touches the test
// registry, so real independent operating-system processes can be used to
// exercise the durable store, the writer lock and process death.
int main(int argc, char** argv) {
  const int child = tzm_test::run_child_mode(argc, argv);
  if (child >= 0) {
    return child;
  }
  return tzm_test::run_all(argc, argv);
}
