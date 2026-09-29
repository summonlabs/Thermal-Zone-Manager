# Contributing to Thermal Zone Manager

Thank you for your interest in contributing to Thermal Zone Manager. This
document describes the contribution terms and the engineering expectations for
this repository.

## License

By contributing to this project, you agree that your contributions are licensed
under the **Apache License, Version 2.0**. See the `LICENSE` file for the full
license text and the `NOTICE` file for attribution and license notices. There is
**no separate Contributor License Agreement (CLA)** requirement: you retain
ownership of your contributions and grant the project a license to use them
under the terms of the Apache License 2.0.

## License headers

New source files should carry the following header:

```
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
```

## Coding standards

- C++20 and CMake only. No third-party dependencies, no network access during
  configure, build or test.
- Build cleanly with `/W4 /WX` on MSVC and `-Wall -Wextra -Wpedantic
  -Wconversion -Wsign-conversion -Werror` elsewhere. Fix warning causes rather
  than suppressing warnings.
- Public identities, generations, epochs, revisions, sequences and attempt
  identifiers are distinct strong types. Do not add implicit conversions between
  them.
- Every physical quantity is an exact integer with an explicit unit. Floating
  point must not appear in any authority-bearing computation, and rational
  operations use the checked helpers in `units.hpp` rather than a cast.
- All external input is untrusted. Validate before allocating, use checked
  arithmetic for externally influenced sizes, and reject malformed input instead
  of normalizing it.
- Zero, unknown, indeterminate, unsupported and unavailable are different
  answers. Do not collapse them, and never turn missing evidence into capacity.
- Where iteration order is public or serialized, it is documented, total and
  tested. Do not rely on hash-container iteration order for anything observable.
- A value read out of a `Result` must be held in a named variable; the accessors
  are lvalue-qualified on purpose so that a reference into a temporary cannot
  escape.

## Architecture boundaries

- This repository owns **zone-level thermal semantics**: thermal-zone identity
  and generation, declared temperature envelopes and policy limits, observed
  temperature evidence with provenance and freshness, headroom arithmetic, zone
  derating state with hysteresis, thermal coupling relationships and effects,
  bounded cross-zone constraint propagation, generation-bound placement
  constraints emitted as advice, and the durable zone configuration and state
  with its recovery, replay and fencing.
- This repository does **not** own physical placement, capacity reservation,
  facility-wide thermal control policy, airflow or liquid-cooling actuation,
  cooling topology, cooling-capacity accounting, failover or thermal-emergency
  orchestration, or ASI workload and DFI path scheduling. Placement constraints
  produced here are outputs: they bound additional heat, and they place nothing.
- Facility structure, equipment identity, sensor identity and plant semantics
  are referenced through opaque typed identifiers and generation-stamped
  evidence records. Do not add knowledge of another runtime's internals.
- Do not add actuation, setpoint writing, telemetry emission, background threads
  or observer callbacks.

## Testing

Every behavioral change needs a test that would fail without it. The repository
expects, at minimum:

- deterministic tests for envelope arithmetic, boundary temperatures, headroom
  bounds, coupling propagation, cycles, derating monotonicity, hysteresis,
  stale-generation fencing and emitted placement constraints;
- fixed-seed property tests that compare the implementation against the
  independent reference model in `tests/support/reference_model.cpp`;
- adversarial tests for malformed, truncated, oversized, corrupt, wrong-version
  and semantically impossible input, and for stale or future authority;
- persistence tests that close and reopen the durable store, that restart a real
  operating-system process, and that kill a writer mid-commit;
- concurrency tests for concurrent readers and writers;
- independent multi-process tests for writer authority and fencing.

Tests must pass on their own. A hanging test is a defect to diagnose and fix,
not something to bound with a timeout. Do not add test timeouts.

## Pull requests

Keep changes focused, add the tests that prove the change, and make sure the
repository builds and tests cleanly in Release and Debug with warnings treated
as errors. Do not include generated build output, install trees, benchmark
residue or editor files.
