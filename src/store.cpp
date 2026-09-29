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

#include "thermal_zone_manager/store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "platform.hpp"
#include "thermal_zone_manager/canonical.hpp"
#include "thermal_zone_manager/clock.hpp"
#include "thermal_zone_manager/digest.hpp"
#include "thermal_zone_manager/limits.hpp"
#include "thermal_zone_manager/version.hpp"

namespace thermal_zone_manager {
namespace {

// ---------------------------------------------------------------------------
// On-disk layout
// ---------------------------------------------------------------------------
//
// File header, 64 bytes, little endian:
//
//   0  magic 'T','Z','M','S'
//   4  u16  format version
//   6  u16  header size (64)
//   8  u32  slot capacity in payload bytes
//  12  u32  slot A offset (64)
//  16  u32  slot B offset (64 + slot capacity + 96)
//  20  u32  reserved, must be zero
//  24  u64  file identity, minted once at creation
//  32  u32  header CRC-32C over bytes [0,32)
//  36  u32  reserved, must be zero
//  40  24 reserved bytes, must be zero
//
// Slot header, 96 bytes, little endian:
//
//   0  magic 'T','Z','S','L'
//   4  u16  format version
//   6  u16  slot header size (96)
//   8  u64  commit sequence
//  16  u64  store revision
//  24  u64  control-plane epoch
//  32  u64  controller incarnation
//  40  u64  configuration generation
//  48  u64  evidence generation
//  56  u64  policy generation
//  64  u32  payload bytes
//  68  u32  payload CRC-32C
//  72  u32  slot header CRC-32C over bytes [0,72)
//  76  u32  reserved, must be zero
//  80  16 reserved bytes, must be zero
//
// Publication order inside a slot is payload first, header second, then the
// flush. A crash before the header lands leaves the slot with a stale header
// whose payload checksum does not match, and a crash during the header leaves
// a header whose own checksum does not match. Both are ignored on recovery.

constexpr std::array<char, 4> kFileMagic = {'T', 'Z', 'M', 'S'};
constexpr std::array<char, 4> kSlotMagic = {'T', 'Z', 'S', 'L'};
constexpr std::size_t kHeaderFieldsEnd = 32;
constexpr std::size_t kSlotHeaderFieldsEnd = 72;

struct SlotHeader {
  CommitSequence commit;
  StoreRevision revision;
  ControlPlaneEpoch epoch;
  ControllerIncarnation incarnation;
  ConfigurationGeneration configuration_generation;
  EvidenceGeneration evidence_generation;
  PolicyGeneration policy_generation;
  std::uint32_t payload_bytes = 0;
};

struct SlotReadResult {
  bool readable = false;   // the header and payload both validated
  bool empty = false;      // the slot area is all zero: never written
  std::string reason;
  SlotHeader header;
  std::vector<std::byte> payload;
};

bool is_all_zero(std::span<const std::byte> data) {
  return std::all_of(data.begin(), data.end(),
                     [](std::byte value) { return value == std::byte{0}; });
}

void write_u32(std::span<std::byte> out, std::size_t offset, std::uint32_t value) {
  store_u32_le(out.subspan(offset, 4), value);
}

void write_u64(std::span<std::byte> out, std::size_t offset, std::uint64_t value) {
  store_u64_le(out.subspan(offset, 8), value);
}

std::uint32_t read_u32(std::span<const std::byte> in, std::size_t offset) {
  return load_u32_le(in.subspan(offset, 4));
}

std::uint64_t read_u64(std::span<const std::byte> in, std::size_t offset) {
  return load_u64_le(in.subspan(offset, 8));
}

}  // namespace

std::string_view to_string(StoreAccess access) {
  switch (access) {
    case StoreAccess::ReadOnly:
      return "read_only";
    case StoreAccess::ReadWrite:
      return "read_write";
  }
  return "unknown_access";
}

Result<std::string> canonical_store_root(const std::string& root) {
  return detail::canonical_directory(root, false);
}

namespace {

constexpr const char* kStateFileLeaf = "thermal-zones.tzm";
constexpr const char* kLockFileLeaf = "thermal-zones.lock";
constexpr const char* kStagingStem = "thermal-zones";

}  // namespace

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------

Result<std::unique_ptr<DurableStore>> DurableStore::open(const StoreOptions& options) {
  if (options.slot_capacity < kMinStoreSlotPayload || options.slot_capacity > kMaxStoreSlotBytes) {
    return Error(ErrorCode::InvalidArgument, "the store slot capacity is outside its range")
        .with("slot_capacity", static_cast<std::uint64_t>(options.slot_capacity))
        .with("min", static_cast<std::uint64_t>(kMinStoreSlotPayload))
        .with("max", static_cast<std::uint64_t>(kMaxStoreSlotBytes));
  }

  const bool want_write = options.access == StoreAccess::ReadWrite;
  Result<std::string> root =
      detail::canonical_directory(options.root, options.create_if_missing && want_write);
  if (!root.ok()) {
    return root.error();
  }

  auto store = std::unique_ptr<DurableStore>(new DurableStore());
  store->canonical_root_ = root.value();
  store->state_path_ = detail::join_path(store->canonical_root_, kStateFileLeaf);
  store->lock_path_ = detail::join_path(store->canonical_root_, kLockFileLeaf);
  store->access_ = options.access;
  store->verify_after_write_ = options.verify_after_write;
  store->recovery_.store_existed = detail::path_exists(store->state_path_);

  if (want_write) {
    Result<detail::NativeFile> lock_file = detail::open_lock_file(store->lock_path_);
    if (!lock_file.ok()) {
      return lock_file.error();
    }
    store->lock_handle_ = lock_file.value();
    const Status locked = detail::lock_exclusive(store->lock_handle_, options.lock_wait);
    if (!locked.ok()) {
      detail::close_file(store->lock_handle_);
      store->lock_handle_ = nullptr;
      return locked.error();
    }
    store->holds_writer_ = true;
  }

  if (!store->recovery_.store_existed) {
    if (!want_write || !options.create_if_missing) {
      if (store->holds_writer_) {
        detail::unlock(store->lock_handle_);
        detail::close_file(store->lock_handle_);
        store->lock_handle_ = nullptr;
        store->holds_writer_ = false;
      }
      return Error(ErrorCode::StoreUnavailable, "the store does not exist and was not created")
          .with("path", store->state_path_);
    }
    const Status created = store->create_new_file(options.slot_capacity);
    if (!created.ok()) {
      return Error(created.error());
    }
    store->recovery_.store_created = true;
  }

  Result<detail::NativeFile> file = detail::open_file(store->state_path_, want_write, false);
  if (!file.ok()) {
    return file.error();
  }
  store->handle_ = file.value();

  const Status loaded = store->load();
  if (!loaded.ok()) {
    detail::close_file(store->handle_);
    store->handle_ = nullptr;
    if (store->holds_writer_) {
      detail::unlock(store->lock_handle_);
      detail::close_file(store->lock_handle_);
      store->lock_handle_ = nullptr;
      store->holds_writer_ = false;
    }
    return Error(loaded.error());
  }
  return store;
}

Status DurableStore::create_new_file(std::size_t slot_capacity) {
  const std::uint64_t token = make_incarnation().raw();
  const std::string staged =
      detail::staging_name(canonical_root_, kStagingStem, token);

  const std::uint64_t total = static_cast<std::uint64_t>(kStoreHeaderBytes) +
                              2ULL * (static_cast<std::uint64_t>(kStoreSlotHeaderBytes) +
                                      static_cast<std::uint64_t>(slot_capacity));
  if (total > static_cast<std::uint64_t>(kMaxStoreFileBytes)) {
    return Error(ErrorCode::InvalidArgument, "the requested store size exceeds its ceiling")
        .with("bytes", total)
        .with("limit", static_cast<std::uint64_t>(kMaxStoreFileBytes));
  }

  Status status = detail::create_file(staged, total);
  if (!status.ok()) {
    detail::delete_file(staged);
    return status;
  }

  std::array<std::byte, kStoreHeaderBytes> header{};
  for (std::size_t index = 0; index < kFileMagic.size(); ++index) {
    header[index] = static_cast<std::byte>(kFileMagic[index]);
  }
  store_u16_le(std::span<std::byte>(header).subspan(4, 2), kStoreFormatVersion);
  store_u16_le(std::span<std::byte>(header).subspan(6, 2),
              static_cast<std::uint16_t>(kStoreHeaderBytes));
  write_u32(header, 8, static_cast<std::uint32_t>(slot_capacity));
  write_u32(header, 12, static_cast<std::uint32_t>(kStoreHeaderBytes));
  write_u32(header, 16,
            static_cast<std::uint32_t>(kStoreHeaderBytes + kStoreSlotHeaderBytes + slot_capacity));
  write_u32(header, 20, 0);
  const std::uint64_t identity = make_incarnation().raw() ^ token;
  write_u64(header, 24, identity == 0 ? 1ULL : identity);
  write_u32(header, 32, crc32c(std::span<const std::byte>(header.data(), kHeaderFieldsEnd)));
  write_u32(header, 36, 0);

  Result<detail::NativeFile> file = detail::open_file(staged, true, false);
  if (!file.ok()) {
    detail::delete_file(staged);
    return Error(file.error());
  }
  status = detail::write_at(file.value(), 0, header);
  if (status.ok()) {
    status = detail::flush_file(file.value());
  }
  detail::close_file(file.value());
  if (!status.ok()) {
    detail::delete_file(staged);
    return status;
  }

  status = detail::rename_file(staged, state_path_);
  if (!status.ok()) {
    detail::delete_file(staged);
    return status;
  }
  file_identity_ = load_u64_le(std::span<const std::byte>(header.data() + 24, 8));
  slot_capacity_ = static_cast<std::uint32_t>(slot_capacity);
  return Status();
}

DurableStore::~DurableStore() {
  if (handle_ != nullptr) {
    detail::close_file(handle_);
    handle_ = nullptr;
  }
  if (lock_handle_ != nullptr) {
    if (holds_writer_) {
      detail::unlock(lock_handle_);
    }
    detail::close_file(lock_handle_);
    lock_handle_ = nullptr;
  }
  holds_writer_ = false;
}

bool DurableStore::holds_writer() const noexcept { return holds_writer_; }

StoreAccess DurableStore::access() const noexcept { return access_; }

// ---------------------------------------------------------------------------
// Load and recovery
// ---------------------------------------------------------------------------

SlotReadResult read_slot(detail::NativeFile handle, std::uint32_t offset,
                       std::uint32_t capacity) {
  SlotReadResult result;
  Result<std::vector<std::byte>> header_bytes =
      detail::read_at(handle, offset, kStoreSlotHeaderBytes);
  if (!header_bytes.ok()) {
    result.reason = header_bytes.error().to_string();
    return result;
  }
  const std::span<const std::byte> header = header_bytes.value();
  if (is_all_zero(header)) {
    result.empty = true;
    result.reason = "never written";
    return result;
  }
  for (std::size_t index = 0; index < kSlotMagic.size(); ++index) {
    if (std::to_integer<char>(header[index]) != kSlotMagic[index]) {
      result.reason = "slot magic mismatch";
      return result;
    }
  }
  if (load_u16_le(header.subspan(4, 2)) != kStoreFormatVersion) {
    result.reason = "slot format version mismatch";
    return result;
  }
  if (load_u16_le(header.subspan(6, 2)) != kStoreSlotHeaderBytes) {
    result.reason = "slot header size mismatch";
    return result;
  }
  if (read_u32(header, 76) != 0) {
    result.reason = "slot reserved field is not zero";
    return result;
  }
  if (!is_all_zero(header.subspan(80, 16))) {
    result.reason = "slot reserved padding is not zero";
    return result;
  }
  const std::uint32_t stored_header_crc = read_u32(header, kSlotHeaderFieldsEnd);
  const std::uint32_t computed_header_crc =
      crc32c(header.subspan(0, kSlotHeaderFieldsEnd));
  if (stored_header_crc != computed_header_crc) {
    result.reason = "slot header checksum mismatch";
    return result;
  }

  SlotHeader decoded;
  decoded.commit = CommitSequence::from_value(read_u64(header, 8));
  decoded.revision = StoreRevision::from_value(read_u64(header, 16));
  decoded.epoch = ControlPlaneEpoch::from_value(read_u64(header, 24));
  decoded.incarnation = ControllerIncarnation::from_value(read_u64(header, 32));
  decoded.configuration_generation = ConfigurationGeneration::from_value(read_u64(header, 40));
  decoded.evidence_generation = EvidenceGeneration::from_value(read_u64(header, 48));
  decoded.policy_generation = PolicyGeneration::from_value(read_u64(header, 56));
  decoded.payload_bytes = read_u32(header, 64);
  const std::uint32_t stored_payload_crc = read_u32(header, 68);

  if (decoded.payload_bytes > capacity || decoded.payload_bytes > kMaxStoreSlotBytes) {
    result.reason = "slot payload length exceeds the slot capacity";
    return result;
  }
  if (decoded.payload_bytes == 0) {
    result.reason = "slot payload is empty";
    return result;
  }

  const std::size_t payload_offset =
      static_cast<std::size_t>(offset) + static_cast<std::size_t>(kStoreSlotHeaderBytes);
  Result<std::vector<std::byte>> payload =
      detail::read_at(handle, payload_offset, decoded.payload_bytes);
  if (!payload.ok()) {
    result.reason = payload.error().to_string();
    return result;
  }
  if (crc32c(payload.value()) != stored_payload_crc) {
    result.reason = "slot payload checksum mismatch";
    return result;
  }

  result.readable = true;
  result.header = decoded;
  result.payload = std::move(payload.value());
  return result;
}

Status DurableStore::load() {
  Result<std::uint64_t> size = detail::file_size(handle_);
  if (!size.ok()) {
    return Error(size.error());
  }
  if (size.value() < kStoreHeaderBytes) {
    return Error(ErrorCode::StoreCorrupt, "the store file is shorter than its header")
        .with("bytes", size.value())
        .with("minimum", static_cast<std::uint64_t>(kStoreHeaderBytes));
  }

  Result<std::vector<std::byte>> header_bytes =
      detail::read_at(handle_, 0, kStoreHeaderBytes);
  if (!header_bytes.ok()) {
    return Error(header_bytes.error());
  }
  const std::span<const std::byte> header = header_bytes.value();
  for (std::size_t index = 0; index < kFileMagic.size(); ++index) {
    if (std::to_integer<char>(header[index]) != kFileMagic[index]) {
      return Error(ErrorCode::StoreCorrupt, "the store file magic does not match")
          .with("path", state_path_);
    }
  }
  if (load_u16_le(header.subspan(4, 2)) != kStoreFormatVersion) {
    return Error(ErrorCode::StoreFormatMismatch, "the store format version is not supported")
        .with("found", static_cast<std::uint64_t>(load_u16_le(header.subspan(4, 2))))
        .with("expected", static_cast<std::uint64_t>(kStoreFormatVersion));
  }
  if (load_u16_le(header.subspan(6, 2)) != kStoreHeaderBytes) {
    return Error(ErrorCode::StoreCorrupt, "the store header size does not match");
  }
  if (read_u32(header, 20) != 0 || read_u32(header, 36) != 0 ||
      !is_all_zero(header.subspan(40, 24))) {
    return Error(ErrorCode::StoreCorrupt, "a reserved store header field is not zero");
  }
  if (read_u32(header, kHeaderFieldsEnd) !=
      crc32c(header.subspan(0, kHeaderFieldsEnd))) {
    return Error(ErrorCode::StoreCorrupt, "the store header checksum does not match");
  }

  const std::uint32_t capacity = read_u32(header, 8);
  const std::uint32_t slot_a = read_u32(header, 12);
  const std::uint32_t slot_b = read_u32(header, 16);
  file_identity_ = read_u64(header, 24);
  if (capacity < kMinStoreSlotPayload || capacity > kMaxStoreSlotBytes) {
    return Error(ErrorCode::StoreCorrupt, "the recorded slot capacity is outside its range")
        .with("slot_capacity", static_cast<std::uint64_t>(capacity));
  }
  const std::uint64_t expected_b =
      static_cast<std::uint64_t>(slot_a) + kStoreSlotHeaderBytes + capacity;
  if (slot_a != kStoreHeaderBytes || slot_b != expected_b) {
    return Error(ErrorCode::StoreCorrupt, "the recorded slot offsets are not consistent")
        .with("slot_a", static_cast<std::uint64_t>(slot_a))
        .with("slot_b", static_cast<std::uint64_t>(slot_b));
  }
  const std::uint64_t required =
      expected_b + kStoreSlotHeaderBytes + static_cast<std::uint64_t>(capacity);
  if (size.value() < required) {
    return Error(ErrorCode::StoreCorrupt, "the store file is shorter than its layout requires")
        .with("bytes", size.value())
        .with("required", required);
  }
  slot_capacity_ = capacity;

  const SlotReadResult first = read_slot(handle_, slot_a, slot_capacity_);
  const SlotReadResult second = read_slot(handle_, slot_b, slot_capacity_);

  const bool first_ok = first.readable;
  const bool second_ok = second.readable;
  if (!first_ok && !second_ok) {
    if (first.empty && second.empty) {
      recovery_.recovered = false;
      recovery_.slot_index = 1;  // so that the first publication targets slot A
      published_slot_ = 1;
      payload_.clear();
      fencing_ = FencingState{};
      return Status();
    }
    return Error(ErrorCode::StoreCorrupt,
                 "neither store generation could be validated")
        .with("slot_a", first.reason)
        .with("slot_b", second.reason);
  }

  const SlotReadResult* chosen = nullptr;
  const SlotReadResult* other = nullptr;
  std::uint32_t chosen_index = 0;
  if (first_ok && second_ok) {
    const bool second_wins =
        second.header.commit > first.header.commit ||
        (second.header.commit == first.header.commit &&
         second.header.revision > first.header.revision);
    chosen = second_wins ? &second : &first;
    other = second_wins ? &first : &second;
    chosen_index = second_wins ? 1U : 0U;
  } else {
    chosen = first_ok ? &first : &second;
    other = first_ok ? &second : &first;
    chosen_index = first_ok ? 0U : 1U;
  }

  if (other != nullptr) {
    if (other->readable) {
      recovery_.abandoned_older_slot = true;
    } else if (!other->empty) {
      recovery_.abandoned_torn_slot = true;
      recovery_.abandoned_reason = other->reason;
    }
  }

  payload_ = chosen->payload;
  published_slot_ = chosen_index;
  fencing_.commit = chosen->header.commit;
  fencing_.revision = chosen->header.revision;
  fencing_.epoch = chosen->header.epoch;
  fencing_.incarnation = chosen->header.incarnation;
  fencing_.configuration_generation = chosen->header.configuration_generation;
  fencing_.evidence_generation = chosen->header.evidence_generation;
  fencing_.policy_generation = chosen->header.policy_generation;

  recovery_.recovered = true;
  recovery_.slot_index = chosen_index;
  recovery_.payload_bytes = payload_.size();
  recovery_.commit = fencing_.commit;
  recovery_.revision = fencing_.revision;
  return Status();
}

// ---------------------------------------------------------------------------
// Commit
// ---------------------------------------------------------------------------

Result<CommitOutcome> DurableStore::commit(const CommitRequest& request) {
  if (!holds_writer_) {
    return Error(ErrorCode::ReadOnlyStore,
                 "a commit requires the writer lock, which this handle does not hold");
  }
  if (request.payload.empty()) {
    return Error(ErrorCode::InvalidArgument, "a commit must carry a payload");
  }
  if (request.payload.size() > slot_capacity_) {
    return Error(ErrorCode::PayloadTooLarge, "the payload does not fit in a store slot")
        .with("bytes", static_cast<std::uint64_t>(request.payload.size()))
        .with("slot_capacity", static_cast<std::uint64_t>(slot_capacity_));
  }

  const std::uint32_t target_slot = published_slot_ == 0 ? 1U : 0U;
  const std::uint64_t slot_offset =
      target_slot == 0
          ? kStoreHeaderBytes
          : static_cast<std::uint64_t>(kStoreHeaderBytes) + kStoreSlotHeaderBytes +
                slot_capacity_;

  SlotHeader header;
  header.commit = fencing_.commit.next();
  header.revision = request.revision;
  header.epoch = request.epoch;
  header.incarnation = request.incarnation;
  header.configuration_generation = request.configuration_generation;
  header.evidence_generation = request.evidence_generation;
  header.policy_generation = request.policy_generation;
  header.payload_bytes = static_cast<std::uint32_t>(request.payload.size());

  // The payload is written first so that a torn publication always leaves a
  // header whose payload checksum cannot match.
  Status status = detail::write_at(handle_, slot_offset + kStoreSlotHeaderBytes, request.payload);
  if (!status.ok()) {
    return Error(status.error());
  }

  std::array<std::byte, kStoreSlotHeaderBytes> slot{};
  for (std::size_t index = 0; index < kSlotMagic.size(); ++index) {
    slot[index] = static_cast<std::byte>(kSlotMagic[index]);
  }
  store_u16_le(std::span<std::byte>(slot).subspan(4, 2), kStoreFormatVersion);
  store_u16_le(std::span<std::byte>(slot).subspan(6, 2),
              static_cast<std::uint16_t>(kStoreSlotHeaderBytes));
  write_u64(slot, 8, header.commit.raw());
  write_u64(slot, 16, header.revision.raw());
  write_u64(slot, 24, header.epoch.raw());
  write_u64(slot, 32, header.incarnation.raw());
  write_u64(slot, 40, header.configuration_generation.raw());
  write_u64(slot, 48, header.evidence_generation.raw());
  write_u64(slot, 56, header.policy_generation.raw());
  write_u32(slot, 64, header.payload_bytes);
  write_u32(slot, 68, crc32c(request.payload));
  write_u32(slot, 72, crc32c(std::span<const std::byte>(slot.data(), kSlotHeaderFieldsEnd)));
  write_u32(slot, 76, 0);

  status = detail::write_at(handle_, slot_offset, slot);
  if (!status.ok()) {
    return Error(status.error());
  }
  status = detail::flush_file(handle_);
  if (!status.ok()) {
    return Error(status.error());
  }

  if (verify_after_write_) {
    // Read the publication back and re-verify it. Only after this read-back
    // does the slot count as the published generation.
    Result<std::vector<std::byte>> read_back =
        detail::read_at(handle_, slot_offset, kStoreSlotHeaderBytes);
    if (!read_back.ok()) {
      return Error(read_back.error());
    }
    const std::span<const std::byte> verified = read_back.value();
    if (std::memcmp(verified.data(), slot.data(), kStoreSlotHeaderBytes) != 0) {
      return Error(ErrorCode::IoFailure,
                   "the published slot header did not read back identically");
    }
    Result<std::vector<std::byte>> payload_back = detail::read_at(
        handle_, slot_offset + kStoreSlotHeaderBytes, request.payload.size());
    if (!payload_back.ok()) {
      return Error(payload_back.error());
    }
    if (crc32c(payload_back.value()) != crc32c(request.payload)) {
      return Error(ErrorCode::IoFailure,
                   "the published payload did not read back identically");
    }
  }

  fencing_.commit = header.commit;
  fencing_.revision = header.revision;
  fencing_.epoch = header.epoch;
  fencing_.incarnation = header.incarnation;
  fencing_.configuration_generation = header.configuration_generation;
  fencing_.evidence_generation = header.evidence_generation;
  fencing_.policy_generation = header.policy_generation;
  payload_.assign(request.payload.begin(), request.payload.end());
  published_slot_ = target_slot;
  recovery_.recovered = true;
  recovery_.payload_bytes = payload_.size();
  recovery_.commit = fencing_.commit;
  recovery_.revision = fencing_.revision;

  CommitOutcome outcome;
  outcome.commit = header.commit;
  outcome.revision = header.revision;
  outcome.slot_index = target_slot;
  outcome.payload_bytes = payload_.size();
  return outcome;
}

std::string DurableStore::describe() const {
  std::string text = "store root        : " + canonical_root_ + "\n";
  text += "state file        : " + state_path_ + "\n";
  text += "lock file         : " + lock_path_ + "\n";
  text += "access            : " + std::string(to_string(access_)) + "\n";
  text += std::string("holds writer lock : ") + (holds_writer_ ? "yes" : "no") + "\n";
  text += "slot capacity     : " + std::to_string(slot_capacity_) + " bytes\n";
  text += "published slot    : " + std::to_string(published_slot_) + "\n";
  text += "commit sequence   : " + thermal_zone_manager::to_string(fencing_.commit) + "\n";
  text += "store revision    : " + thermal_zone_manager::to_string(fencing_.revision) + "\n";
  text += "control epoch     : " + thermal_zone_manager::to_string(fencing_.epoch) + "\n";
  text += std::string("store existed     : ") + (recovery_.store_existed ? "yes" : "no") + "\n";
  text += std::string("recovered         : ") + (recovery_.recovered ? "yes" : "no") + "\n";
  if (recovery_.abandoned_torn_slot) {
    text += "abandoned slot    : torn (" + recovery_.abandoned_reason + ")\n";
  } else if (recovery_.abandoned_older_slot) {
    text += "abandoned slot    : older valid generation\n";
  }
  return text;
}

}  // namespace thermal_zone_manager
