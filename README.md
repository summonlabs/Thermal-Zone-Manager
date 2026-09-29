# Thermal Zone Manager

Generation-bound thermal zone state, headroom and derating for the Data Center
Control Plane.

The core question this runtime answers is:

> What is the current generation-bound thermal state of each zone, how much
> headroom exists within its declared envelope, how do neighbouring or coupled
> zones constrain that answer, what derating state follows, and which placement
> constraints must be surfaced without taking over placement authority?

Thermal Zone Manager is released as **1.0.0** and is built from the DCCP
program's shared conventions: strongly typed identities and generations, exact
integer physical units, a bounded dependency-light C++20 library, and refusals
that are machine-readable rather than textual.

---

## 1. Systems boundary

### What this repository owns

- **Thermal-zone identity and generation.** A zone is an operator-declared
  handle with a stable name and a generation that moves whenever its
  declaration changes.
- **Temperature envelopes and policy limits.** Floor, derating onset, ceiling
  and critical threshold, plus the derating ladder resolution and the
  hysteresis release margin.
- **Observed temperature evidence with provenance and freshness.** Every
  observation carries its source, its provenance kind, the zone generation and
  evidence generation it was taken against, the publishing control-plane epoch
  and process incarnation, the instant it was taken, and the window over which
  it stays usable.
- **Headroom calculations.** The margin from the effective temperature to the
  declared ceiling, expressed both in temperature and in additional heat, with
  exact integer arithmetic and conservative rounding.
- **Zone derating state.** A quantised ladder over the derating ramp with
  hysteresis that resists flapping.
- **Thermal coupling relationships and effects.** A validated directed graph of
  bounded rational coefficients with explicit incoming and outgoing budgets.
- **Cross-zone constraint propagation.** A bounded, deterministic, cycle-safe
  evaluation with a mathematically stated termination and bound.
- **Zone-local degradation and recovery hysteresis.** Published derating rises
  immediately and falls one step at a time, and only on repeated usable
  evidence.
- **Generation-bound placement constraints emitted as evidence and advice.**
  Each zone yields a bounded heat allowance, a proven zero, or an explicit
  "no proven allowance".
- **Durable zone configuration and state, recovery, replay and fencing.** Two
  integrity-checked generations in one file, atomic publication, and idempotent
  command replay.

### What this repository explicitly does not own

- **Physical placement.** A constraint says how much additional heat a zone can
  accept, or that no proven amount exists. It never selects a location, ranks a
  candidate, or reserves anything.
- **Capacity reservation.** Reservation belongs to the facility reservation
  runtimes; this runtime emits bounds, not holds.
- **Facility-wide thermal control policy.** Setpoints, plant modes and
  facility-wide objectives belong to the thermal control plane.
- **Airflow and liquid-cooling actuation.** Nothing is written to a fan, pump,
  valve, CDU or CRAH/CRAC.
- **Cooling topology and cooling-capacity accounting.** Those are separate DCCP
  runtimes. Thermal Zone Manager references them only through opaque identifiers
  and evidence records.
- **Failover and thermal-emergency orchestration.** A critical band is reported;
  no emergency action is taken.
- **ASI workload scheduling and DFI path scheduling.** This runtime neither
  schedules nor routes.

### Boundary in one sentence

Thermal Zone Manager turns declared zone envelopes plus generation-stamped
temperature evidence into per-zone headroom, derating state and bounded
placement advice, and it stops there: it places nothing, reserves nothing and
actuates nothing.

---

## 2. Architecture

```
                 declared configuration                    observed evidence
   operator / planner                     external sensor / facility model / test
            |                                             |
            v                                             v
   +-------------------------+                 +---------------------------+
   | ZoneConfiguration       |                 | TemperatureObservation    |
   |  envelope, resistance,  |                 |  temperature, optional    |
   |  declared heat, ladder, |                 |  heat, source, provenance,|
   |  release margin         |                 |  generations, epoch,      |
   +-----------+-------------+                 |  observed_at, validity    |
               |                               +-------------+-------------+
               |                                             |
        +------v---------------------------------------------v------+
        |                    ThermalZoneEngine                      |
        |  configuration generation, evidence generation, policy   |
        |  generation, control-plane epoch, store revision          |
        +------+---------------------+----------------------+-------+
               |                     |                      |
     apply_configuration     ingest_observation        evaluate
               |                     |                      |
               |                     |            +---------v----------+
               |                     |            | freshness verdict  |
               |                     |            | bounded coupling   |
               |                     |            | effective temp     |
               |                     |            | band, headroom     |
               |                     |            | derating + hold    |
               |                     |            | constraint         |
               |                     |            +---------+----------+
               |                     |                      |
        +------v---------------------v----------------------v------+
        |              DurableStore: two integrity-checked slots     |
        |   canonical payload, atomic publication, recovery, fencing |
        +------------------------------------------------------------+
```

Public headers live under `include/thermal_zone_manager/`, one self-contained
header per concept. The implementation is under `src/`. Nothing in `src/` is
installed, and the installed headers do not expose an internal type.

### Major types

| Type | Meaning |
| --- | --- |
| `ZoneId`, `ZoneGeneration`, `ConfigurationGeneration`, `EvidenceGeneration`, `PolicyGeneration`, `StoreRevision`, `ControlPlaneEpoch`, `ObservationSequence`, `EvaluationId`, `CommandId`, `AttemptId`, `CommitSequence`, `ControllerIncarnation` | distinct strong identities; none converts to another |
| `MilliCelsius`, `MilliWatts`, `MicroKelvinPerWatt`, `PartsPerMillion`, `Nanoseconds`, `Timestamp` | exact integer physical quantities with explicit units |
| `TemperatureEnvelope` | the declared operating band and the derating ramp |
| `ZoneConfiguration` | one zone's declaration plus its generation |
| `CouplingGraph` | validated directed coupling with coefficients in parts per million |
| `ThermalPolicy` | coupling hop budget, hysteresis hold, future skew tolerance |
| `Configuration` | zones plus coupling plus policy, with both generations and a digest |
| `TemperatureObservation` | one generation-stamped piece of temperature evidence |
| `ZoneEvidence` | the held observation plus its freshness verdict |
| `DeratingState`, `DeratingOutcome` | durable ladder level and the result of one update |
| `ThermalHeadroom` | known, unknown or indeterminate, with margins when known |
| `ZoneThermalState` | one zone's complete state for one evaluation |
| `PlacementConstraint` | the emitted advice for one zone |
| `EvaluationResult` | the immutable answer, in ascending zone order, with a digest |
| `ThermalZoneEngine` | the runtime: configuration, evidence, evaluation, publication |
| `DurableStore` | the two-slot integrity-checked file |
| `Error`, `ErrorCode`, `Result<T>`, `Status` | the machine contract |

---

## 3. State and authority model

### Four answers, never collapsed

A headroom question has exactly three answers, and a freshness question has six:

| `HeadroomStatus` | Meaning |
| --- | --- |
| `Known` | every input was usable and the exact margin was computed |
| `Unknown` | a critical input was missing, stale, superseded, recovered or from the future |
| `Indeterminate` | the evidence was usable but the model cannot resolve it, for example a neighbour with a non-zero coefficient whose heat load is undeclared |

| `EvidenceFreshness` | Meaning |
| --- | --- |
| `Fresh` | current evidence generation, inside its validity window, published at or before the evaluation instant |
| `Stale` | superseded evidence generation, or past its validity window |
| `Future` | observed later than the evaluation instant plus the declared skew tolerance |
| `Recovered` | read back from the durable store and not yet revalidated by a current source |
| `Missing` | nothing was ever accepted for this zone |
| `Unsupported` | a source that cannot produce this evidence |

Only `Fresh` is usable, and only `Known` is proven. `Unknown` and
`Indeterminate` are not zero and not safe: the emitted constraint is
`Indeterminate` and `allowance_known` is false, so a consumer cannot mistake
either for capacity.

### The freshness decision order

Fixed, total and tested:

1. no observation at all -> `Missing`
2. the evidence generation is not the current one -> `Stale` (superseded)
3. the value came back from the durable store -> `Recovered`
4. `observed_at` is later than now plus the tolerance -> `Future`
5. `observed_at` is older than the validity window -> `Stale` (expired)
6. otherwise -> `Fresh`

The evidence generation is checked before the recovered marker, so a
configuration replacement makes held observations superseded rather than
recovered. The recovered marker is checked before the timestamp, so state read
back from disk is never fresh physical evidence even when its window has not
expired.

### Identity, observation and authority are separate

- `ZoneId` identifies a zone. `ZoneGeneration` says which declaration of it.
- `ConfigurationGeneration` moves on every committed replacement of the
  declared world. `EvidenceGeneration` moves with it, and fences every
  observation: evidence taken against the previous declaration is reported as
  superseded rather than reinterpreted.
- `ControlPlaneEpoch` fences **authority to mutate**. Advancing it refuses every
  request still carrying the old epoch. It deliberately does not invalidate
  physical evidence, which is fenced by the evidence generation instead.
- `StoreRevision` moves on every durable mutation and is the fence for a
  published evaluation.
- `ObservationSequence` is a per-zone monotone source sequence. A lower sequence
  is refused as stale; an equal sequence with a different payload is refused as
  conflicting; an equal sequence with an identical payload replays.
- `CommandId` binds one mutation to one request body through a fingerprint over
  the canonical encoding of that body. `AttemptId` is part of the delivery, not
  of the request identity.

### Exact integer units

Every authoritative quantity is an exact integer:

```
MilliCelsius          1 mC   = 0.001 C
MilliWatts            1 mW   = 0.001 W
MicroKelvinPerWatt    1 uK/W = 1e-6 K/W
PartsPerMillion       1 ppm  = 1e-6
Nanoseconds           duration
Timestamp             nanoseconds since the Unix epoch, UTC
```

The dimensional identity used by the headroom arithmetic is:

```
rise[mC] = heat[mW] * resistance[uK/W] / 1000000
heat[mW] = rise[mC] * 1000000 / resistance[uK/W]
```

Both directions are exact rational operations evaluated with a portable 128-bit
intermediate, so no authoritative result depends on floating point. Rounding is
always towards the safe answer: coupled heat is rounded **up** so a rise is
never understated, and headroom is rounded **down** so a margin is never
overstated. `long double` and `double` do not appear anywhere in the library.

---

## 4. Thermal semantics

### Envelope and band

The declared envelope requires

```
floor_temp < derate_onset < ceiling_temp <= critical_temp
```

with every bound physically plausible. All four orderings are refused with a
specific code: an implausible bound is `InvalidTemperature`, a broken ordering
is `InvalidEnvelope`, and a critical threshold below the ceiling is
`ContradictoryLimits`.

`classify()` maps a temperature to one of `BelowFloor`, `Nominal`,
`Derating`, `AtLimit`, `Critical`. The enumerators are ordered by increasing
temperature, so classification is monotone in temperature; that monotonicity is
tested.

### Coupling

A coupling edge carries the fraction of the source zone's dissipated heat that
reaches the sink zone, as a bounded ratio in parts per million. The graph is
validated before it is used:

- every endpoint must name a declared zone;
- self coupling is refused;
- a duplicate (source, sink) pair is refused;
- a coefficient outside `[0, 1000000]` is refused;
- the outgoing coefficients of one zone may not exceed unity;
- the incoming coefficients of one zone may not exceed four times unity.

Cycles are legal configuration, not an error.

### Bounded, cycle-safe propagation

The evaluation is a fixed number of passes over the graph, decided before the
first pass:

```
total = h + A h + A^2 h + ... + A^(K-1) h          K = policy.coupling_hops
```

where `A[i][j]` is the coefficient from zone `j` to zone `i` and `h` is each
zone's base heat (an observed concurrent heat load when the evidence is usable,
otherwise the declared heat load).

- **Termination** is by construction: exactly `K` passes, with `K` in `[1, 8]`,
  and an early exit once no zone carries heat forward. No convergence loop and
  no iteration limit exists to be tuned.
- **Bound.** With `sigma = max_i sum_j A[i][j]` (the largest incoming total,
  exposed as `CouplingGraph::contraction_ppm()`), the series is bounded by
  `h / (1 - sigma)` whenever `sigma < 1`.
- **Non-contractive graphs.** If `sigma >= 1` and more than one pass was
  requested, the refinement is refused and the first-order answer is used, with
  `coupling_refinement_refused` set and the `coupling_not_contractive` reason
  emitted. The engine never evaluates an unbounded series approximately.
- **Unknown neighbours.** A neighbour with a non-zero coefficient whose heat load
  is neither observed nor declared makes the coupled rise unresolved. The zone's
  headroom becomes `Indeterminate`, distinct from `Unknown`.

### Derating with hysteresis

Derating is a quantised ladder over the ramp between the derating onset and the
ceiling:

```
level(T)    = 0                                            T <= derate_onset
            = ceil((T - derate_onset) * steps / span)      otherwise, clamped
            = steps                                        T >= ceiling
fraction(l) = ceil(l * 1000000 / steps)                    parts per million
```

The published level rises immediately to the instantaneous level and falls one
step at a time, and only after `release_hold_observations` consecutive usable
observations at or below the release threshold:

```
release threshold for level L = level_floor(L) - recovery_margin
level_floor(L)                = derate_onset + floor((L-1) * span / steps)
```

`level_floor(L)` is the highest temperature whose instantaneous level is at most
`L-1`, so the release condition carries a real margin. A missing, stale, future
or recovered observation never releases a step: the update is held, the
published level is unchanged, and the recovery progress is reset. The number of
level changes over a sequence of N observations is therefore at most the number
of escalations plus `N / release_hold_observations`.

### Emitted placement constraints

Each zone yields exactly one `PlacementConstraint`:

| Kind | Meaning |
| --- | --- |
| `Bounded` | a proven positive allowance in additional heat |
| `Prohibited` | a proven **zero**: the band is at or above the ceiling, or the headroom is exhausted |
| `Indeterminate` | no proven allowance exists; `allowance_known` is false |

The allowance is

```
allowance = floor(power_margin * (1000000 - derate_fraction) / 1000000)
```

clamped to a **known** zero when the zone is at or above its ceiling. Every
constraint carries the zone generation, both generations, the policy generation,
the evaluation identifier, the store revision it was computed against, the band,
the derating fraction, and an ascending unique reason list.

A constraint is an output. It never places, reserves, ranks or actuates.

---

## 5. Persistence and recovery

### Layout

```
<root>/thermal-zones.tzm     file header + slot A + slot B
<root>/thermal-zones.lock    the OS-level exclusion file
```

The file header is 64 bytes: magic, little-endian format version, slot
geometries, a file identity minted at creation, a CRC-32C over the first 32
bytes, and reserved fields that must be zero. Each slot is a 96-byte header
(commit sequence, store revision, epoch, incarnation, three generations, payload
length, payload CRC-32C, header CRC-32C, reserved) followed by the canonical
payload. Every reserved field is validated, every declared length is bounded
before allocation, and any mismatch is refused rather than repaired.

### The commit protocol

1. The inactive slot's payload is written.
2. The slot header is written second, so a torn publication always leaves a
   header whose payload checksum cannot match.
3. The file is flushed to the device.
4. The slot is read back and both checksums are re-verified.

**The commit point is step 4.** Only after the read-back matches does the slot
count as the published generation. A crash before it leaves the previous
generation authoritative; a crash during steps 1 or 2 leaves a slot that fails
its checksum. Recovery always selects the valid slot with the greatest commit
sequence, so the store resolves to exactly one complete generation and never to
a hybrid.

### Recovery

Opening the store validates the file header, then both slots. A slot whose bytes
are entirely zero was never written; a slot that is non-zero but fails
validation is the trace of a torn publication and is reported in the recovery
report. If neither slot validates and at least one is non-zero, the open is
refused with `StoreCorrupt`.

Store creation goes through a staging name and an atomic rename, so a partially
created file can never be mistaken for an authoritative one.

### What a restart means for authority

**Recovered durable state is not fresh physical evidence.** Every observation
that comes back from disk is marked recovered, and a recovered observation is
never fresh even when its timestamp is still inside its validity window. After a
restart every zone therefore reports `Unknown` headroom and an
`Indeterminate` constraint until a strictly newer observation arrives from a
live source. Re-delivering an identical observation is a replay: it returns the
original receipt and does not clear the recovered marker.

### Path hardening

The store root is canonicalised before anything else happens: the input is
rejected if it contains a NUL byte, a reserved device name, a component ending
in a dot or a space, an invalid character or a traversal component; it is made
absolute and normalised; the root is opened and the kernel's final resolved path
is used as the identity. Two processes naming the same store through different
separators or through a directory link therefore agree on one lock.

---

## 6. Concurrency and process authority

### Ownership

One writer at a time, enforced with a real OS-level exclusive lock over
`thermal-zones.lock`, held for the lifetime of the engine. A read-only engine
opens, validates and recovers without taking the lock and can never mutate.

### Lock order

```
OS writer lock  ->  state mutex  ->  (released)  ->  recent-evaluation mutex
```

The state mutex is never held while the recent-evaluation mutex is acquired, and
the two are never held at the same time. Mutations hold the state mutex
exclusively and then perform store I/O; reads take it in shared mode only.

### Audit results

The call paths were audited by hand for the hazards that matter:

| Hazard | Finding |
| --- | --- |
| read lock then write acquisition on the same lock | none; no path upgrades a shared lock |
| write lock held while re-entering the same lock | none; helpers take no locks |
| mutex re-entry through callbacks | none; the library has no callbacks and no observers |
| event emission or user callbacks under a lock | none; no callbacks exist |
| joining workers while holding state | none; there are no worker threads |
| cancellation/shutdown lock inversion | `close()` takes the state mutex in two sequential scopes, then the recent-evaluation mutex, then releases the file lock last |
| nested acquisition with inconsistent ordering | none; the order above is the only order |
| persistence callbacks that re-enter mutable state | none; the store is a leaf |
| stale asynchronous completion publishing after authority changed | refused: `commit_evaluation` re-validates the evaluation identifier, both generations and the store revision under the write lock |

The audit also found and removed a real race: a mutation used to check "is the
engine open" before taking the state lock and then dereference the store after
taking it, so a concurrent `close()` could have released the store in between.
Every mutation now re-checks the same condition with the lock held, and the
publication helper refuses a released store rather than dereferencing it.

### What is proved with real processes

The test binary re-executes itself as a child process for the multi-process
proof. Children are started with no inherited handles and no pipes, and they
communicate through files, so the harness has no pipe deadlock and no timeout.
The proof covers: a second process being refused the writer lock with
`store_locked`; the lock becoming available once the holder closes; a child
holding the lock blocking a local writer while a read-only open still succeeds;
a bounded wait budget succeeding once the holder releases; a child publishing a
generation the parent then recovers; and repeated process kills during a commit
loop.

---

## 7. Error model

Every refusal is an `Error` carrying a stable `ErrorCode`, a human-readable
message, and ordered key/value context. The code is the contract;
`Error::to_string()` is for people.

### Validation precedence

Validation runs in a fixed phase order, and the primary code is the one from the
earliest violated phase:

| # | Phase | Codes |
| --- | --- | --- |
| 1 | store availability | `StoreUnavailable`, `StoreClosed`, `ReadOnlyStore`, `StoreBusy`, `WriterAuthorityLost`, `ProcessFailure` |
| 2 | structural validity | `InvalidArgument`, `EmptyConfiguration`, `TooManyZones`, `TooManyCouplingEdges`, `NameTooLong`, `TextNotValidUtf8`, `PayloadTooLarge` |
| 3 | control-plane epoch | `MissingEpoch`, `EpochMismatch`, `EpochRegression` |
| 4 | generation binding | `StaleConfigurationGeneration`, `FutureConfigurationGeneration`, `StaleZoneGeneration`, `FutureZoneGeneration`, `StaleEvidenceGeneration`, `FutureEvidenceGeneration`, `StaleEvaluation`, `StaleStateRevision`, `CrossGenerationAuthor` |
| 5 | numeric domain | `InvalidEnvelope`, `InvalidTemperature`, `InvalidThermalResistance`, `InvalidCoefficient`, `CouplingBudgetExceeded`, `InvalidPolicy`, `InvalidDerateLadder`, `InvalidFreshnessWindow`, `ContradictoryLimits` |
| 6 | identity | `DuplicateZoneId`, `DuplicateZoneName`, `UnknownZone` |
| 7 | coupling graph | `UnknownCouplingEndpoint`, `SelfCoupling`, `DuplicateCouplingEdge`, `CouplingNotContractive`, `HopBudgetExceeded` |
| 8 | observation sequencing | `StaleSequence`, `ConflictingObservation`, `DuplicateObservation`, `ObservationForGenerationMismatch` |
| 9 | command identity | `IdempotencyConflict`, `UnknownCommand`, `CommandAlreadyApplied` |
| 10 | arithmetic | `ArithmeticOverflow`, `DivisionByZero` |
| 11 | store integrity | `StoreCorrupt`, `StoreFormatMismatch`, `StorePathInvalid`, `StoreLocked`, `TruncatedRecord`, `TrailingBytes`, `UnexpectedEndOfInput`, `IoFailure` |
| 12 | capability | `Unsupported`, `Internal` |

`validation_precedence()` returns this table as data, and a test walks it to
prove the phases are ordered, disjoint and total, and that every code has a
unique stable name.

### Idempotent retry

Each mutating command carries a `CommandId` and an `AttemptId`. The engine
computes a fingerprint over the canonical encoding of the request's semantic
content - the declared world and the epoch for a configuration command, the
observation for an ingest, the evaluation and its generations for a commit - and
records it durably together with the original outcome.

- The same `CommandId` with the same fingerprint **replays** the original
  receipt: the same generations, store revision and commit sequence, with
  `replayed = true`, and nothing is actuated a second time.
- The same `CommandId` with a different fingerprint is refused with
  `IdempotencyConflict`.
- The expected configuration generation is a *precondition*, not content, so it
  is excluded from the fingerprint and a lost-response retry is recognised even
  though the live generation has moved on to the value that command produced. A
  **new** command carrying a superseded expected generation is still refused
  with `StaleConfigurationGeneration`.

Receipts are bounded to `kMaxRetainedReceipts` (4096) and evicted oldest first,
so a long-lived store cannot grow without limit. A retry older than the retained
window is treated as a new command; that boundary is documented and bounded
rather than hidden.

---

## 8. The command line tool

```
tzm --help
tzm --version
tzm self-check
tzm inspect --store <dir>
tzm scenario --store <dir>
```

- `self-check` drives an in-memory engine through the whole lifecycle -
  establish an epoch, declare a coupled facility, ingest fresh evidence,
  evaluate, publish the derating outcome - and prints the rendered result.
- `inspect` opens an existing store read-only and prints the store description,
  the declared world, the fencing state and the recovery report.
- `scenario` declares a fixed four-zone facility with a coupling ring and a
  cross link, ingests deterministic synthetic observations, evaluates, publishes
  and prints every emitted placement constraint. It is safe to run twice against
  the same store: command identities are derived from the live revision, and the
  configuration precondition is read from the engine.
- An unknown command or option exits 2 with a message on stderr.

---

## 9. Examples

| Program | What it shows |
| --- | --- |
| `tzm_example_01` | one zone, one fresh observation, known headroom and a bounded constraint |
| `tzm_example_02` | a coupled zone: the arriving heat, the reduced allowance, and that the constraint is advice |
| `tzm_example_03` | missing, expired and unresolved evidence side by side, and that neither is zero |
| `tzm_example_04` | the derating ladder and its hysteresis under a controlled clock |
| `tzm_example_05` | stale zone generation, superseded evidence generation and epoch mismatch |
| `tzm_example_06` | a real store: commit, close, reopen, recovered evidence, and revalidation |
| `tzm_example_07` | idempotent retry: replay, conflict, and a new command against a stale world |
| `tzm_example_08` | a facility-wide survey with summary counts and a result digest |

Every example labels its facility data as synthetic harness input and states
that this runtime placed, reserved and actuated nothing.

---

## 10. Build, test and install

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
ctest --test-dir build-debug --output-on-failure

cmake --install build --prefix /some/prefix

cmake -S tests/package_consumer -B consumer-build \
      -DCMAKE_PREFIX_PATH=/some/prefix
cmake --build consumer-build
./consumer-build/tzm_package_consumer
```

Options: `TZM_BUILD_TESTS`, `TZM_BUILD_TOOLS`, `TZM_BUILD_EXAMPLES`,
`TZM_BUILD_BENCHMARKS`, `TZM_WARNINGS_AS_ERRORS` (default `ON`),
`TZM_SANITIZERS` (default `OFF`), `BUILD_SHARED_LIBS` (default `OFF`).

### Downstream use

The installed package exports one namespaced target:

```cmake
find_package(ThermalZoneManager 1.0 REQUIRED)
target_link_libraries(my_target PRIVATE ThermalZoneManager::thermal_zone_manager)
```

`tests/package_consumer` is an independent out-of-tree consumer that configures
against the installed package, links the exported target, and runs a real
library lifecycle. It never references the source tree.

---

## 11. Validation

### What was actually run

| Configuration | Build | Test |
| --- | --- | --- |
| MSVC 19.44 / Release / `/W4 /permissive- /WX /utf-8` | clean, zero warnings | `ctest`: 13 of 13 passed |
| MSVC 19.44 / Debug / `/W4 /permissive- /WX /utf-8` | clean, zero warnings | `ctest`: 13 of 13 passed |
| MSVC 19.44 / Debug / `/fsanitize=address` | clean, zero warnings | `ctest`: 13 of 13 passed |

The ctest surface is the unit, property, adversarial, corruption, concurrency,
multi-process, restart and process-death suite, the eight examples, three CLI
paths and a quick benchmark smoke run. The library test binary contains one
hundred and twenty-two test cases; the binary was also run repeatedly in Release
and Debug to check for order-dependent or timing-dependent behaviour.

Sanitizer notes, stated plainly: MSVC AddressSanitizer was used
(`/fsanitize=address`). It reports memory errors, and it does **not** support
leak detection on this platform - the runtime prints
`detect_leaks is not supported on this platform` - so **leak detection is
UNSUPPORTED here** and no leak-freedom claim is made.

### Independent reference model

`tests/support/reference_model.cpp` is a second, independently written model of
the same documented semantics. It finds neighbours by linear scan rather than
through an adjacency index, and it performs every rational operation with a
decimal multi-limb big-integer helper rather than the library's binary 128-bit
ones, so it shares no arithmetic and no data layout with the implementation. The
property tests compare the library against it over fixed-seed random worlds that
include cycles, multi-hop refinement, undeclared heat loads, unknown evidence and
empty graphs. Every failing seed is printed, and the seed is repeated in the
failure message so a case can be replayed exactly.

### Test inventory

| Area | What it proves |
| --- | --- |
| `test_units` | checked arithmetic, 128-bit intermediates, conservative rounding, CRCs, limits, version |
| `test_envelope` | every broken ordering, boundary temperatures, band monotonicity |
| `test_derating` | ladder boundaries, floor/onset inverse relation, escalation, hold-gated release, bounded flapping |
| `test_headroom` | the three answers, over-envelope is not clamped |
| `test_evidence` | field validation and the total freshness decision order |
| `test_coupling` | graph validation, first-order coupling, cycles, multi-hop, contractivity refusal, unknown neighbours |
| `test_fencing` | epoch, zone generation, evidence generation, sequences, evaluation staleness, idempotency |
| `test_persistence` | creation, recovery, recovered-is-not-fresh, read-only, close, describe, path canonicalisation |
| `test_corruption` | damaged headers, single-byte corruption, an exhaustive truncation sweep, slots that pass integrity but fail semantics |
| `test_paths` | relative roots, separator equivalence, traversal and device names, linked roots, restricted files |
| `test_concurrency` | concurrent readers with a writer, concurrent evaluations, concurrent read-only engines, close racing readers and writers |
| `test_writer_lock` | real processes: exclusion, release, a held lock, a wait budget, repeated open and close |
| `test_restart` | real processes: publish, recover, and repeated kills during a commit loop |
| `test_crash` | repeated process death mid-commit, death before a commit, staging files never authoritative |
| `test_adversarial` | ceiling enforcement before allocation, dense coupling, contradictory limits, invalid text, repeated replacement, extreme values |
| `test_property` | randomized agreement with the reference model, evaluation stability, warming monotonicity, band consistency |
| `test_determinism` | total ordering, reason ordering, independent-engine agreement, content-addressed digests |
| `test_errors` | code names, phase table, transient classification, `Result` semantics |

---

## 12. Benchmarks

`tzm_benchmark` measures **completed** operations with
`std::chrono::steady_clock` around the whole call, after a warm-up. It never
measures submission latency, and when persistence is part of the operation the
durability cost (payload write, device flush and read-back verification) is
included in the measured time.

Workload, stated in full: a generated facility of 256 zones with 512 coupling
edges, a fixed deterministic graph, fresh synthetic heat and temperature
observations, 200 iterations, Release build with `/O2`, MSVC 19.44.35222, Ninja,
Windows x64, local NTFS store. The workload is **SYNTHETIC**: the zone graph and
the observations are generated by the benchmark, and no real facility, sensor or
cooling plant is exercised.

Observed on one run of the above:

```
benchmark evaluate_zones zones=256 edges=512 iterations=200 total_ns=184547600 ns_per_op=922738 workload=SYNTHETIC
benchmark evaluate_and_commit zones=256 edges=512 iterations=200 total_ns=440070900 ns_per_op=2200354 workload=SYNTHETIC
```

That is 922.738 microseconds per completed evaluation of 256 coupled zones, and
2.200 milliseconds per completed evaluation plus durable publication, including
the flush and the read-back verification. These are single-run observations on
one host, not a distribution, and no before/after claim is made from them.
`--quick` runs a small workload as a smoke test.

---

## 13. Limitations

- **No live facility was available.** No BMS, DCIM, sensor bus, chiller, CDU,
  CRAH/CRAC, pump, valve or PLC was reachable from the development host. Every
  facility, sensor and thermal workload in this repository is **SYNTHETIC**, and
  the test evidence is likewise synthetic. No hardware validation was performed
  and none is claimed. Evidence produced by the harness carries
  `ProvenanceKind::SyntheticHarness` so that it can never be mistaken for plant
  evidence.
- **The coupling model is first-order with a bounded refinement.** It is a fixed
  point evaluation of the documented heat-transfer relation, not a
  computational-fluid-dynamics or lumped-capacitance model. It propagates heat
  through declared coefficients and bounds the result; it does not model
  transient thermal mass, convection detail or airflow patterns.
- **One writer at a time.** Concurrent mutation from two processes is refused by
  design rather than merged.
- **Idempotency state is bounded.** Receipts are retained up to a fixed ceiling
  and evicted oldest first; a retry older than that window is a new command.
- **The recent-evaluation cache is in-process and bounded to eight entries.**
  `commit_evaluation` must follow the `evaluate` that produced it in the same
  process; an evaluation identifier this engine did not produce is refused with
  `UnknownCommand`.
- **Leak detection is UNSUPPORTED on this platform.** MSVC AddressSanitizer was
  run and passed for memory errors, but its leak detector is not available on
  Windows, so no leak-freedom claim is made.
- **A true operating-system access denial on the store file could not be forced
  on this host.** The read-only attribute is not an access control list, and
  constructing a denying ACL needs privileges the development session did not
  have. The repository therefore tests the contract that matters - a restricted
  store file either produces a named refusal or opens with the store structurally
  intact - and does not claim to have exercised the denial path itself.
- **Epoch semantics.** Advancing the control-plane epoch fences mutation
  authority. It deliberately does not invalidate physical evidence, which is
  fenced by the evidence generation instead.
- **This runtime reports, it does not act.** A critical band, an exhausted
  allowance or a fully derated zone produces evidence and advice; no setpoint is
  written and no emergency action is taken.

---

## 14. Repository layout

```
include/thermal_zone_manager/   the public API, one self-contained header per concept
src/                            the implementation
tools/tzm_cli.cpp               the inspection and administration tool
examples/                       eight programs over the stable public API
benchmarks/                     completed-operation benchmarks
tests/                          the proof obligations, one file per area
tests/support/                  harness, fixtures, real child processes, reference model
tests/package_consumer/         an independent out-of-tree find_package consumer
```

`CONTRIBUTING.md` describes the contribution terms and the engineering
expectations. `NOTICE` records attribution.

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
