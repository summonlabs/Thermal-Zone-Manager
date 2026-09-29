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

#ifndef THERMAL_ZONE_MANAGER_CLOCK_HPP
#define THERMAL_ZONE_MANAGER_CLOCK_HPP

#include <atomic>
#include <cstdint>

#include "thermal_zone_manager/export.hpp"
#include "thermal_zone_manager/ids.hpp"
#include "thermal_zone_manager/units.hpp"

namespace thermal_zone_manager {

// The single source of "now" used by freshness classification, receipt
// stamping and evaluation. The instant used for a decision is always read from
// the engine's clock; a caller never supplies it.
class Clock {
 public:
  virtual ~Clock() = default;
  virtual Timestamp now() const = 0;
};

// The host wall clock, in nanoseconds since the Unix epoch, UTC.
TZM_API const Clock& system_clock();

// Convenience: system_clock().now().
TZM_API Timestamp system_now();

// A clock whose value only moves when the test moves it. Supplied by the
// public API so a downstream consumer can drive deterministic freshness
// scenarios without waiting on wall time.
class TZM_API ManualClock final : public Clock {
 public:
  ManualClock() = default;
  explicit ManualClock(Timestamp start) : now_(start.value) {}

  Timestamp now() const override { return Timestamp{now_.load(std::memory_order_relaxed)}; }

  void set(Timestamp instant) { now_.store(instant.value, std::memory_order_relaxed); }
  void advance(Nanoseconds delta) {
    now_.store(now_.load(std::memory_order_relaxed) + delta.value, std::memory_order_relaxed);
  }

 private:
  std::atomic<std::int64_t> now_{0};
};

// A process incarnation identity: distinct for each engine instance that does
// not reuse a durable one. Derived from the host clock, the process id and an
// atomic counter; it is an identity, not a secret.
TZM_API ControllerIncarnation make_incarnation();

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_CLOCK_HPP
