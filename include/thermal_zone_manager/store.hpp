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

#ifndef THERMAL_ZONE_MANAGER_STORE_HPP
#define THERMAL_ZONE_MANAGER_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "thermal_zone_manager/errors.hpp"
#include "thermal_zone_manager/export.hpp"
#include "thermal_zone_manager/ids.hpp"
#include "thermal_zone_manager/limits.hpp"
#include "thermal_zone_manager/units.hpp"

namespace thermal_zone_manager {

enum class StoreAccess : std::uint8_t {
  // Opens, validates and recovers the store but never takes the writer lock
  // and never writes.
  ReadOnly = 0,
  // Takes the exclusive writer lock for the lifetime of the handle.
  ReadWrite = 1,
};

TZM_API std::string_view to_string(StoreAccess access);

struct StoreOptions {
  // Directory that holds the store. Canonicalised on open; the canonical form
  // is what the lock is derived from, so two processes cannot disagree about
  // which file they are locking.
  std::string root;
  StoreAccess access = StoreAccess::ReadOnly;
  bool create_if_missing = false;
  // How long to wait for the writer lock before refusing with StoreLocked.
  // Zero means "try once".
  Nanoseconds lock_wait{0};
  // Re-read and re-verify a slot after flushing it, and fail the commit when
  // the read-back does not match. Durability is only claimed when this is on.
  bool verify_after_write = true;
  // Payload capacity of each of the two slots. This is recorded in the store
  // header when the store is created and is read back from the file
  // afterwards, so it only takes effect on creation.
  std::size_t slot_capacity = kDefaultStoreSlotPayload;
};

// Everything a decision is bound to. Persisted in the slot header so a reader
// recovers the exact authority that produced the state.
struct FencingState {
  ControlPlaneEpoch epoch;
  ControllerIncarnation incarnation;
  StoreRevision revision;
  CommitSequence commit;
  ConfigurationGeneration configuration_generation;
  EvidenceGeneration evidence_generation;
  PolicyGeneration policy_generation;

  bool operator==(const FencingState& other) const {
    return epoch == other.epoch && incarnation == other.incarnation && revision == other.revision &&
           commit == other.commit &&
           configuration_generation == other.configuration_generation &&
           evidence_generation == other.evidence_generation &&
           policy_generation == other.policy_generation;
  }
};

// What happened when the store was opened.
struct RecoveryReport {
  bool store_existed = false;
  bool store_created = false;
  // True when a committed generation was found and loaded.
  bool recovered = false;
  // The slot the recovered generation came from: 0 or 1.
  std::uint32_t slot_index = 0;
  // True when the other slot held an older but valid generation.
  bool abandoned_older_slot = false;
  // True when the other slot held bytes that failed validation: the trace of a
  // torn or interrupted publication.
  bool abandoned_torn_slot = false;
  std::string abandoned_reason;
  std::size_t payload_bytes = 0;
  std::size_t zone_count = 0;
  std::size_t observation_count = 0;
  CommitSequence commit;
  StoreRevision revision;
};

// A commit request. The store assigns the commit sequence.
struct CommitRequest {
  std::span<const std::byte> payload;
  ControlPlaneEpoch epoch;
  ControllerIncarnation incarnation;
  StoreRevision revision;
  ConfigurationGeneration configuration_generation;
  EvidenceGeneration evidence_generation;
  PolicyGeneration policy_generation;
};

struct CommitOutcome {
  CommitSequence commit;
  StoreRevision revision;
  std::uint32_t slot_index = 0;
  std::size_t payload_bytes = 0;
};

// The durable store: one file, two generations, integrity checked.
//
// Layout
//   <root>/thermal-zones.tzm     header + slot A + slot B
//   <root>/thermal-zones.lock    the OS-level exclusion file
//
// Commit protocol
//   1. the inactive slot is written in full;
//   2. the file is flushed to the device;
//   3. the slot is read back and its header and payload checksums are
//      re-verified;
//   4. only then is the slot the published generation.
//
// The commit point is step 3. A crash before it leaves the previous generation
// authoritative; a crash during step 1 or 2 leaves a slot that fails its
// checksum, and open() ignores it. Recovery always selects the valid slot with
// the greatest commit sequence, so the store resolves to exactly one complete
// generation and never to a hybrid.
class DurableStore {
 public:
  static Result<std::unique_ptr<DurableStore>> open(const StoreOptions& options);

  ~DurableStore();
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;
  DurableStore(DurableStore&&) = delete;
  DurableStore& operator=(DurableStore&&) = delete;

  bool holds_writer() const noexcept;
  StoreAccess access() const noexcept;
  const FencingState& fencing() const noexcept { return fencing_; }
  const RecoveryReport& recovery() const noexcept { return recovery_; }

  // Canonical absolute path of the store root, the state file and the lock
  // file, as actually used.
  const std::string& canonical_root() const noexcept { return canonical_root_; }
  const std::string& state_path() const noexcept { return state_path_; }
  const std::string& lock_path() const noexcept { return lock_path_; }

  // The payload of the recovered generation. Empty when nothing was committed.
  std::span<const std::byte> payload() const noexcept { return payload_; }

  // Publishes a new generation. Requires the writer lock.
  Result<CommitOutcome> commit(const CommitRequest& request);

  // Compact description of the store's current occupancy, for inspection.
  std::string describe() const;

 private:
  DurableStore() = default;

  // Reads, validates and decodes the durable generation. Called once from
  // open(), which is the only place recovery happens.
  Status load();

  // Creates a fresh store file through a staging name and an atomic rename, so
  // a partially created file can never be mistaken for an authoritative one.
  Status create_new_file(std::size_t slot_capacity);

  FencingState fencing_;
  RecoveryReport recovery_;
  std::string canonical_root_;
  std::string state_path_;
  std::string lock_path_;
  std::vector<std::byte> payload_;
  std::uint64_t file_identity_ = 0;
  std::uint32_t slot_capacity_ = 0;
  std::uint32_t published_slot_ = 0;
  void* handle_ = nullptr;   // native file handle
  void* lock_handle_ = nullptr;
  bool holds_writer_ = false;
  StoreAccess access_ = StoreAccess::ReadOnly;
  bool verify_after_write_ = true;
};

// Canonicalises a store root: absolute, no traversal, no reparse points, no
// reserved device names, no trailing dot or space in a component, and bounded
// length. Returns StorePathInvalid for anything ambiguous, because two
// processes must not be able to obtain different locks for the same logical
// store.
TZM_API Result<std::string> canonical_store_root(const std::string& root);

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_STORE_HPP
