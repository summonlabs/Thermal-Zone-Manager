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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace tzm_test {
namespace {

struct Entry {
  std::string suite;
  std::string name;
  TestBody body;
};

std::vector<Entry>& registry() {
  static std::vector<Entry> entries;
  return entries;
}

std::string& current_test() {
  static std::string name;
  return name;
}

int& current_failures() {
  static int failures = 0;
  return failures;
}

}  // namespace

void register_test(const char* suite, const char* name, TestBody body) {
  Entry entry;
  entry.suite = suite;
  entry.name = name;
  entry.body = body;
  registry().push_back(entry);
}

void report_failure(const char* file, int line, const std::string& message) {
  ++current_failures();
  std::fprintf(stderr, "FAIL %s\n  %s:%d: %s\n", current_test().c_str(), file, line,
               message.c_str());
  std::fflush(stderr);
}

void report_note(const std::string& message) {
  std::fprintf(stderr, "note %s: %s\n", current_test().c_str(), message.c_str());
  std::fflush(stderr);
}

std::string debug_text(const std::string& value) { return "\"" + value + "\""; }
std::string debug_text(std::string_view value) { return "\"" + std::string(value) + "\""; }
std::string debug_text(const char* value) {
  return value == nullptr ? std::string("(null)") : "\"" + std::string(value) + "\"";
}
std::string debug_text(bool value) { return value ? "true" : "false"; }
std::string debug_text(std::nullptr_t) { return "(null)"; }

int run_all(int argc, char** argv) {
  std::string filter;
  if (argc > 1) {
    filter = argv[1];
  }

  std::vector<Entry> entries = registry();
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.suite != b.suite) {
      return a.suite < b.suite;
    }
    return a.name < b.name;
  });

  int executed = 0;
  int failed = 0;
  for (const Entry& entry : entries) {
    const std::string full = entry.suite + "." + entry.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    current_test() = full;
    const int before = current_failures();
    try {
      entry.body();
    } catch (const AbortTest&) {
      // The failure has already been reported.
    } catch (const std::exception& error) {
      report_failure(__FILE__, __LINE__,
                     std::string("unhandled standard exception: ") + error.what());
    } catch (...) {
      report_failure(__FILE__, __LINE__, "unhandled non-standard exception");
    }
    if (current_failures() != before) {
      ++failed;
      std::fprintf(stderr, "FAILED %s\n", full.c_str());
    } else {
      std::fprintf(stdout, "ok     %s\n", full.c_str());
    }
    std::fflush(stdout);
  }

  std::fprintf(stdout, "\n%d test(s) executed, %d failed, %d assertion failure(s)\n", executed,
               failed, current_failures());
  std::fflush(stdout);
  if (executed == 0) {
    std::fprintf(stderr, "no test matched the filter\n");
    return 3;
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace tzm_test
