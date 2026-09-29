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

#include "thermal_zone_manager/clock.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace thermal_zone_manager {
namespace {

class SystemClock final : public Clock {
 public:
  Timestamp now() const override {
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
    return Timestamp{static_cast<std::int64_t>(nanos)};
  }
};

std::uint64_t process_identity() {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

}  // namespace

const Clock& system_clock() {
  static const SystemClock instance;
  return instance;
}

Timestamp system_now() { return system_clock().now(); }

ControllerIncarnation make_incarnation() {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t tick =
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::uint64_t sequence = counter.fetch_add(1, std::memory_order_relaxed) + 1ULL;
  std::uint64_t mixed = tick ^ (process_identity() * 0x9E3779B97F4A7C15ULL);
  mixed ^= sequence * 0xBF58476D1CE4E5B9ULL;
  mixed ^= mixed >> 30;
  mixed *= 0xBF58476D1CE4E5B9ULL;
  mixed ^= mixed >> 27;
  mixed *= 0x94D049BB133111EBULL;
  mixed ^= mixed >> 31;
  if (mixed == 0) {
    mixed = 1;
  }
  return ControllerIncarnation::from_value(mixed);
}

}  // namespace thermal_zone_manager
