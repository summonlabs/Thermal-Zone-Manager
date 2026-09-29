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

#include "thermal_zone_manager/engine.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "serialize.hpp"
#include "thermal_zone_manager/canonical.hpp"
#include "thermal_zone_manager/digest.hpp"
#include "thermal_zone_manager/limits.hpp"
#include "world.hpp"

namespace thermal_zone_manager {
namespace {

using detail::CommandKind;
using detail::IdempotencyRecord;
using detail::PersistedZoneState;
using detail::WorldState;

constexpr std::size_t kRecentEvaluationCapacity = 8;

void push_reason(std::vector<ConstraintReason>& reasons, ConstraintReason reason) {
  const auto found = std::lower_bound(reasons.begin(), reasons.end(), reason);
  if (found == reasons.end() || *found != reason) {
    reasons.insert(found, reason);
  }
}

std::uint64_t fingerprint_bytes(const ByteWriter& writer) {
  return fnv1a64(writer.bytes());
}

void encode_policy(ByteWriter& writer, const ThermalPolicy& policy) {
  writer.put_u64(policy.generation.raw());
  writer.put_u32(policy.coupling_hops);
  writer.put_u32(policy.release_hold_observations);
  writer.put_i64(policy.future_tolerance.value);
}

void encode_zone(ByteWriter& writer, const ZoneConfiguration& zone) {
  writer.put_u32(static_cast<std::uint32_t>(zone.id.raw()));
  writer.put_text(zone.name.str());
  writer.put_u64(zone.generation.raw());
  writer.put_i64(zone.envelope.floor_temp.value);
  writer.put_i64(zone.envelope.derate_onset.value);
  writer.put_i64(zone.envelope.ceiling_temp.value);
  writer.put_i64(zone.envelope.critical_temp.value);
  writer.put_optional_i64(zone.declared_heat.has_value(),
                          zone.declared_heat.has_value() ? zone.declared_heat->value : 0);
  writer.put_i64(zone.thermal_resistance.value);
  writer.put_u32(zone.derate_steps);
  writer.put_i64(zone.recovery_margin.value);
}

bool same_declaration(const ZoneConfiguration& a, const ZoneConfiguration& b) {
  return a.name == b.name && a.envelope == b.envelope && a.declared_heat == b.declared_heat &&
         a.thermal_resistance == b.thermal_resistance && a.derate_steps == b.derate_steps &&
         a.recovery_margin == b.recovery_margin;
}

bool same_policy(const ThermalPolicy& a, const ThermalPolicy& b) {
  return a.coupling_hops == b.coupling_hops &&
         a.release_hold_observations == b.release_hold_observations &&
         a.future_tolerance == b.future_tolerance;
}

// Canonicalises a request's declared world so that a retry with a reordered
// but otherwise identical body produces the same idempotency fingerprint.
struct CanonicalRequest {
  std::vector<ZoneConfiguration> zones;
  std::vector<CouplingEdge> edges;
};

CanonicalRequest canonicalise(const ConfigurationRequest& request) {
  CanonicalRequest result;
  result.zones = request.zones;
  std::sort(result.zones.begin(), result.zones.end(), zone_less);
  result.edges = request.coupling;
  std::sort(result.edges.begin(), result.edges.end(),
            [](const CouplingEdge& a, const CouplingEdge& b) {
              if (a.source != b.source) {
                return a.source < b.source;
              }
              return a.sink < b.sink;
            });
  return result;
}

std::uint64_t configuration_request_fingerprint(const ConfigurationRequest& request) {
  const CanonicalRequest canonical = canonicalise(request);
  // The fingerprint covers the semantic content of the declared world and the
  // epoch it is declared under. The expected generation is the precondition,
  // not the content, so it is deliberately excluded: a redelivery of the same
  // world must be recognised as the same command even though the live
  // generation has moved on to the value this command produced.
  ByteWriter writer;
  writer.put_u8(static_cast<std::uint8_t>(CommandKind::ApplyConfiguration));
  writer.put_u64(request.epoch.raw());
  writer.put_u32(static_cast<std::uint32_t>(canonical.zones.size()));
  for (const ZoneConfiguration& zone : canonical.zones) {
    encode_zone(writer, zone);
  }
  writer.put_u32(static_cast<std::uint32_t>(canonical.edges.size()));
  for (const CouplingEdge& edge : canonical.edges) {
    writer.put_u32(static_cast<std::uint32_t>(edge.source.raw()));
    writer.put_u32(static_cast<std::uint32_t>(edge.sink.raw()));
    writer.put_u32(static_cast<std::uint32_t>(edge.coefficient.value));
  }
  encode_policy(writer, request.policy);
  return fingerprint_bytes(writer);
}

std::uint64_t epoch_request_fingerprint(const EpochRequest& request) {
  ByteWriter writer;
  writer.put_u8(static_cast<std::uint8_t>(CommandKind::AdvanceEpoch));
  writer.put_u64(request.expected_current.raw());
  writer.put_u64(request.target.raw());
  return fingerprint_bytes(writer);
}

std::uint64_t evaluation_commit_fingerprint(const EvaluationResult& evaluation,
                                            const EvaluationCommitRequest& request) {
  ByteWriter writer;
  writer.put_u8(static_cast<std::uint8_t>(CommandKind::CommitEvaluation));
  writer.put_u64(request.epoch.raw());
  writer.put_u64(evaluation.id().raw());
  writer.put_u64(evaluation.basis_revision().raw());
  writer.put_u64(evaluation.configuration_generation().raw());
  writer.put_u64(evaluation.evidence_generation().raw());
  writer.put_u32(static_cast<std::uint32_t>(evaluation.zones().size()));
  for (const ZoneThermalState& zone : evaluation.zones()) {
    writer.put_u32(static_cast<std::uint32_t>(zone.zone.raw()));
    writer.put_u64(zone.zone_generation.raw());
    writer.put_u32(zone.derating_after.published_level);
    writer.put_u32(zone.derating_after.published_steps);
    writer.put_u32(zone.derating_after.hold_count);
  }
  return fingerprint_bytes(writer);
}

// Verifies that the durable store assigned exactly the commit sequence the
// in-memory receipt was built against. A divergence would mean the store and
// the engine disagree about the commit lineage, which is never acceptable.
Status verify_commit(const CommitOutcome& outcome, CommitSequence expected) {
  if (outcome.commit != expected) {
    return Error(ErrorCode::Internal,
                 "the store published a commit sequence the engine did not predict")
        .with("expected", expected.raw())
        .with("actual", outcome.commit.raw());
  }
  return Status();
}

// The heat figure a zone contributes to its neighbours.
struct HeatFigure {
  bool known = false;
  MilliWatts value;
  HeatSource source = HeatSource::None;
};

struct CouplingField {
  std::vector<std::int64_t> incoming_heat;
  std::vector<bool> known;
  std::vector<std::size_t> unknown_contributors;
  std::uint32_t hops_used = 0;
  bool refinement_refused = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// Engine implementation
// ---------------------------------------------------------------------------

struct ThermalZoneEngine::Impl {
  EngineOptions options;
  const Clock* clock = nullptr;
  std::unique_ptr<DurableStore> store;
  mutable std::shared_mutex state_mutex;
  mutable std::mutex cache_mutex;
  WorldState world;
  std::deque<EvaluationResult> recent;
  RecoveryReport recovery;
  ControllerIncarnation incarnation;
  std::atomic<bool> closed{false};
  bool durable = false;

  // Fast pre-check, taken without the state lock so that a closed or read-only
  // engine is refused cheaply.
  Status require_open_and_writable() const {
    if (closed.load()) {
      return Error(ErrorCode::StoreClosed, "the engine has been closed");
    }
    if (durable && store == nullptr) {
      return Error(ErrorCode::StoreClosed, "the engine no longer holds its store");
    }
    if (durable && store->access() != StoreAccess::ReadWrite) {
      return Error(ErrorCode::ReadOnlyStore,
                   "the engine was opened without writer authority for this store");
    }
    return Status();
  }

  // The same decision, re-taken with the state lock held. close() also takes
  // that lock and clears the store handle, so a mutation that raced past the
  // pre-check must re-check here before it dereferences the store.
  Status require_open_locked() const {
    if (closed.load()) {
      return Error(ErrorCode::StoreClosed, "the engine has been closed");
    }
    if (durable && store == nullptr) {
      return Error(ErrorCode::StoreClosed, "the engine no longer holds its store");
    }
    return Status();
  }

  // The durable commit sequence the next successful commit will carry. The
  // store assigns exactly the successor of the sequence it holds, and the
  // store's sequence mirrors the engine's, so a receipt can record the commit
  // it belongs to before the payload is written. The equality is verified
  // after the commit rather than assumed.
  CommitSequence next_commit() const { return world.fencing.commit.next(); }

  // Publishes a prospective world. The in-memory world is untouched: the
  // caller only adopts next_world after this returns Ok. The commit sequence
  // the store assigns is verified against the value the receipts were built
  // with, so a receipt can never name the wrong commit.
  Status publish(WorldState& next_world, std::span<const std::byte> payload) {
    if (!durable) {
      return Status();
    }
    if (store == nullptr) {
      // Unreachable while every caller re-checks under the state lock; kept so
      // that a future caller cannot dereference a released store.
      return Error(ErrorCode::StoreClosed, "the engine no longer holds its store");
    }
    const CommitSequence expected = next_world.fencing.commit;
    CommitRequest commit;
    commit.payload = payload;
    commit.epoch = next_world.fencing.epoch;
    commit.incarnation = incarnation;
    commit.revision = next_world.fencing.revision;
    commit.configuration_generation = next_world.fencing.configuration_generation;
    commit.evidence_generation = next_world.fencing.evidence_generation;
    commit.policy_generation = next_world.fencing.policy_generation;
    Result<CommitOutcome> outcome = store->commit(commit);
    if (!outcome.ok()) {
      return outcome.error();
    }
    return verify_commit(outcome.value(), expected);
  }

  // Reads a zone's contribution to coupling. Observed heat wins over the
  // declaration, but only when the observation is usable evidence; a declared
  // heat load is configuration and never goes stale.
  HeatFigure heat_figure(const ZoneConfiguration& zone, const ZoneEvidence& evidence,
                         bool evidence_usable) const {
    HeatFigure figure;
    if (evidence_usable && evidence.present && evidence.observation.heat.has_value()) {
      figure.known = true;
      figure.value = *evidence.observation.heat;
      figure.source = HeatSource::Observed;
      return figure;
    }
    if (zone.declared_heat.has_value()) {
      figure.known = true;
      figure.value = *zone.declared_heat;
      figure.source = HeatSource::Declared;
      return figure;
    }
    return figure;
  }

  ZoneEvidence evidence_for_zone(ZoneId zone) const {
    ZoneEvidence evidence;
    const PersistedZoneState* persisted = world.find(zone);
    if (persisted != nullptr && persisted->has_observation) {
      evidence.present = true;
      evidence.observation = persisted->observation;
      evidence.recovered = persisted->recovered;
    }
    return evidence;
  }

  DeratingState derating_for_zone(ZoneId zone) const {
    const PersistedZoneState* persisted = world.find(zone);
    if (persisted == nullptr) {
      return DeratingState{};
    }
    return persisted->derating;
  }

  // Bounded, deterministic, cycle-safe coupling evaluation. The hop count is
  // fixed before the first pass, so propagation always terminates in exactly
  // that many passes; a graph whose incoming coupling is not strictly
  // contractive is refused a refinement and evaluated at the first order
  // instead, which is recorded on the result.
  Result<CouplingField> propagate(const Configuration& configuration,
                                  const std::vector<HeatFigure>& figures) const {
    const std::vector<ZoneConfiguration>& zones = configuration.zones();
    const std::size_t count = zones.size();
    CouplingField field;
    field.incoming_heat.assign(count, 0);
    field.known.assign(count, true);
    field.unknown_contributors.assign(count, 0);

    const ThermalPolicy& policy = configuration.policy();
    std::uint32_t hops = policy.coupling_hops;
    if (hops > 1 && !configuration.coupling().is_contractive()) {
      field.refinement_refused = true;
      hops = 1;
    }
    field.hops_used = hops;

    std::vector<std::int64_t> frontier(count, 0);
    std::vector<bool> frontier_known(count, true);
    for (std::size_t index = 0; index < count; ++index) {
      if (figures[index].known) {
        frontier[index] = figures[index].value.value;
      } else {
        frontier_known[index] = false;
      }
    }

    std::vector<std::int64_t> next(count, 0);
    std::vector<bool> next_known(count, true);
    std::vector<bool> frontier_used(count, false);

    for (std::uint32_t hop = 0; hop < hops; ++hop) {
      bool any_carrier = false;
      for (std::size_t sink = 0; sink < count; ++sink) {
        std::int64_t sum = 0;
        bool known = true;
        for (const CouplingEdge& edge : configuration.coupling().incoming(zones[sink].id)) {
          if (edge.coefficient.value == 0) {
            continue;
          }
          const auto source = static_cast<std::size_t>(
              std::distance(zones.begin(),
                            std::lower_bound(zones.begin(), zones.end(), edge.source,
                                             [](const ZoneConfiguration& candidate, ZoneId value) {
                                               return candidate.id < value;
                                             })));
          if (source >= count || !(zones[source].id == edge.source)) {
            return Error(ErrorCode::Internal,
                         "a coupling edge named a zone that is not in the configuration");
          }
          if (!frontier_known[source]) {
            known = false;
            continue;
          }
          const std::optional<std::int64_t> contribution =
              mul_div_ceil(frontier[source], static_cast<std::int64_t>(edge.coefficient.value),
                           static_cast<std::int64_t>(kPpmScale));
          if (!contribution.has_value()) {
            return Error(ErrorCode::ArithmeticOverflow,
                         "the coupled heat contribution overflowed")
                .with("zone", zones[sink].id.raw());
          }
          const std::optional<std::int64_t> total = add_checked(sum, contribution.value());
          if (!total.has_value()) {
            return Error(ErrorCode::ArithmeticOverflow,
                         "the coupled heat total overflowed")
                .with("zone", zones[sink].id.raw());
          }
          sum = total.value();
          frontier_used[source] = true;
        }
        next[sink] = sum;
        next_known[sink] = known;
        if (known && sum != 0) {
          any_carrier = true;
        }
        if (known) {
          const std::optional<std::int64_t> accumulated =
              add_checked(field.incoming_heat[sink], sum);
          if (!accumulated.has_value()) {
            return Error(ErrorCode::ArithmeticOverflow,
                         "the accumulated coupled heat overflowed")
                .with("zone", zones[sink].id.raw());
          }
          field.incoming_heat[sink] = accumulated.value();
        } else {
          field.known[sink] = false;
        }
      }
      frontier.swap(next);
      frontier_known.swap(next_known);
      if (!any_carrier) {
        break;
      }
    }

    // Distinct declared neighbours whose heat load is unknown and whose
    // coupling coefficient into this zone is non-zero. This is the count a
    // consumer sees; the boolean above already accounts for what the
    // multi-hop series could not resolve.
    for (std::size_t sink = 0; sink < count; ++sink) {
      for (const CouplingEdge& edge : configuration.coupling().incoming(zones[sink].id)) {
        if (edge.coefficient.value == 0) {
          continue;
        }
        const auto source = static_cast<std::size_t>(
            std::distance(zones.begin(),
                          std::lower_bound(zones.begin(), zones.end(), edge.source,
                                           [](const ZoneConfiguration& candidate, ZoneId value) {
                                             return candidate.id < value;
                                           })));
        if (source < count && !figures[source].known) {
          field.unknown_contributors[sink] += 1;
        }
      }
    }
    return field;
  }
};

// ---------------------------------------------------------------------------
// Open and lifetime
// ---------------------------------------------------------------------------

ThermalZoneEngine::ThermalZoneEngine() : impl_(std::make_unique<Impl>()) {}

ThermalZoneEngine::~ThermalZoneEngine() = default;

Result<std::unique_ptr<ThermalZoneEngine>> ThermalZoneEngine::open(const EngineOptions& options) {
  auto engine = std::unique_ptr<ThermalZoneEngine>(new ThermalZoneEngine());
  Impl& impl = *engine->impl_;
  impl.options = options;
  impl.clock = options.clock != nullptr ? options.clock : &system_clock();
  impl.incarnation =
      options.incarnation.is_zero() ? make_incarnation() : options.incarnation;

  if (options.root.empty()) {
    // Ephemeral engine: no durable state and no cross-process exclusion.
    impl.durable = false;
    impl.world = WorldState{};
    impl.recovery = RecoveryReport{};
    impl.recovery.store_existed = false;
    impl.recovery.store_created = true;
    return engine;
  }

  StoreOptions store_options;
  store_options.root = options.root;
  store_options.access = options.access;
  store_options.create_if_missing = options.create_if_missing;
  store_options.lock_wait = options.lock_wait;
  store_options.verify_after_write = options.verify_after_write;
  store_options.slot_capacity = options.slot_capacity;
  Result<std::unique_ptr<DurableStore>> store = DurableStore::open(store_options);
  if (!store.ok()) {
    return store.error();
  }
  impl.store = std::move(store.value());
  impl.durable = true;
  impl.recovery = impl.store->recovery();

  const std::span<const std::byte> payload = impl.store->payload();
  if (!payload.empty()) {
    ByteReader reader(payload);
    Result<WorldState> decoded = detail::decode_world(reader);
    if (!decoded.ok()) {
      return decoded.error();
    }
    const Status end = reader.require_end();
    if (!end.ok()) {
      return end.error();
    }
    impl.world = std::move(decoded.value());

    // The slot header is the commit marker, so it is authoritative for the
    // fencing state. The copy inside the payload must agree with it.
    const FencingState& slot = impl.store->fencing();
    const FencingState& embedded = impl.world.fencing;
    if (!(slot == embedded)) {
      return Error(ErrorCode::StoreCorrupt,
                   "the slot header and the payload disagree about the fencing state")
          .with("slot_commit", slot.commit.raw())
          .with("payload_commit", embedded.commit.raw());
    }
  } else {
    impl.world = WorldState{};
  }

  // Recovered durable state is not fresh physical evidence. Every observation
  // that came back from disk is marked recovered until a strictly newer
  // observation from a live source replaces it.
  std::size_t recovered_observations = 0;
  for (PersistedZoneState& zone : impl.world.zones) {
    if (zone.has_observation) {
      zone.recovered = true;
      ++recovered_observations;
    }
  }
  impl.recovery.zone_count = impl.world.configuration.zone_count();
  impl.recovery.observation_count = recovered_observations;
  return engine;
}

bool ThermalZoneEngine::durable() const noexcept { return impl_->durable; }

bool ThermalZoneEngine::holds_writer() const noexcept {
  return impl_->store != nullptr && impl_->store->holds_writer();
}

bool ThermalZoneEngine::closed() const noexcept { return impl_->closed.load(); }

const Clock& ThermalZoneEngine::clock() const noexcept { return *impl_->clock; }

ControllerIncarnation ThermalZoneEngine::incarnation() const { return impl_->incarnation; }

Configuration ThermalZoneEngine::configuration() const {
  const std::shared_lock<std::shared_mutex> guard(impl_->state_mutex);
  return impl_->world.configuration;
}

FencingState ThermalZoneEngine::fencing() const {
  const std::shared_lock<std::shared_mutex> guard(impl_->state_mutex);
  return impl_->world.fencing;
}

RecoveryReport ThermalZoneEngine::recovery_report() const {
  const std::shared_lock<std::shared_mutex> guard(impl_->state_mutex);
  return impl_->recovery;
}

ZoneEvidence ThermalZoneEngine::evidence_for(ZoneId zone) const {
  const std::shared_lock<std::shared_mutex> guard(impl_->state_mutex);
  ZoneEvidence evidence = impl_->evidence_for_zone(zone);
  evidence.freshness = classify_freshness(evidence, impl_->clock->now(),
                                          impl_->world.configuration.evidence_generation(),
                                          impl_->world.configuration.policy().future_tolerance);
  return evidence;
}

DeratingState ThermalZoneEngine::derating_for(ZoneId zone) const {
  const std::shared_lock<std::shared_mutex> guard(impl_->state_mutex);
  return impl_->derating_for_zone(zone);
}

std::string ThermalZoneEngine::describe() const {
  const std::shared_lock<std::shared_mutex> guard(impl_->state_mutex);
  std::string text;
  text += std::string("mode              : ") + (impl_->durable ? "durable" : "ephemeral") + "\n";
  text += "incarnation       : " + to_string(impl_->incarnation) + "\n";
  if (impl_->store != nullptr) {
    text += impl_->store->describe();
  }
  text += "store revision    : " + to_string(impl_->world.fencing.revision) + "\n";
  text += "configuration gen : " + to_string(impl_->world.configuration.generation()) + "\n";
  text += "evidence gen      : " + to_string(impl_->world.configuration.evidence_generation()) + "\n";
  text += "policy gen        : " + to_string(impl_->world.configuration.policy().generation) + "\n";
  text += "zones             : " +
          std::to_string(static_cast<unsigned long long>(impl_->world.configuration.zone_count())) +
          "\n";
  text += "coupling edges    : " +
          std::to_string(static_cast<unsigned long long>(
              impl_->world.configuration.coupling().edge_count())) +
          "\n";
  text += "retained receipts : " +
          std::to_string(static_cast<unsigned long long>(impl_->world.receipts.size())) + "\n";
  return text;
}

Status ThermalZoneEngine::close() {
  {
    const std::unique_lock<std::shared_mutex> guard(impl_->state_mutex);
    impl_->closed.store(true);
  }
  std::unique_ptr<DurableStore> released;
  {
    const std::unique_lock<std::shared_mutex> guard(impl_->state_mutex);
    released = std::move(impl_->store);
  }
  const std::lock_guard<std::mutex> cache_guard(impl_->cache_mutex);
  impl_->recent.clear();
  return Status();
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

namespace {

Status check_epoch(const FencingState& fencing, ControlPlaneEpoch epoch) {
  if (epoch.is_zero()) {
    return Error(ErrorCode::MissingEpoch,
                 "the request does not carry a control-plane epoch; establish one first");
  }
  if (epoch != fencing.epoch) {
    return Error(ErrorCode::EpochMismatch,
                 "the request was planned under a different control-plane epoch")
        .with("request_epoch", epoch.raw())
        .with("current_epoch", fencing.epoch.raw());
  }
  return Status();
}

const IdempotencyRecord* find_receipt(const WorldState& world, CommandId command) {
  const auto found = std::lower_bound(
      world.receipts.begin(), world.receipts.end(), command,
      [](const IdempotencyRecord& record, CommandId value) { return record.command < value; });
  if (found == world.receipts.end() || !(found->command == command)) {
    return nullptr;
  }
  return &(*found);
}

void retain_receipt(WorldState& world, IdempotencyRecord record) {
  const auto found = std::lower_bound(
      world.receipts.begin(), world.receipts.end(), record.command,
      [](const IdempotencyRecord& existing, CommandId value) {
        return existing.command < value;
      });
  world.receipts.insert(found, record);
  while (world.receipts.size() > kMaxRetainedReceipts) {
    world.receipts.erase(world.receipts.begin());
  }
}

}  // namespace

Result<ConfigurationReceipt> ThermalZoneEngine::apply_configuration(
    const ConfigurationRequest& request) {
  Impl& impl = *impl_;
  {
    const Status allowed = impl.require_open_and_writable();
    if (!allowed.ok()) {
      return allowed.error();
    }
  }
  if (request.command.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "a mutation must carry a command identifier");
  }

  const std::uint64_t fingerprint = configuration_request_fingerprint(request);
  const std::unique_lock<std::shared_mutex> guard(impl.state_mutex);
  {
    const Status allowed = impl.require_open_locked();
    if (!allowed.ok()) {
      return allowed.error();
    }
  }

  // A command that has already been applied replays its original outcome
  // before any precondition is re-evaluated. That is what makes a lost-response
  // retry safe: the caller gets the answer to the request it already made,
  // and nothing is actuated a second time.
  if (const IdempotencyRecord* replay = find_receipt(impl.world, request.command)) {
    if (replay->fingerprint != fingerprint || replay->kind != CommandKind::ApplyConfiguration) {
      return Error(ErrorCode::IdempotencyConflict,
                   "this command identifier was already used for a different request")
          .with("command", request.command.raw());
    }
    ConfigurationReceipt receipt;
    receipt.generation = ConfigurationGeneration::from_value(replay->result_value);
    receipt.evidence_generation = EvidenceGeneration::from_value(replay->result_aux);
    receipt.revision = replay->applied_revision;
    receipt.commit = replay->applied_commit;
    receipt.zone_count = static_cast<std::size_t>(replay->result_extra >> 32);
    receipt.coupling_edge_count = static_cast<std::size_t>(replay->result_extra & 0xFFFFFFFFULL);
    receipt.fingerprint = fingerprint;
    receipt.replayed = true;
    receipt.durable = impl.durable;
    return receipt;
  }

  const Status epoch_status = check_epoch(impl.world.fencing, request.epoch);
  if (!epoch_status.ok()) {
    return epoch_status.error();
  }
  const ConfigurationGeneration current = impl.world.configuration.generation();
  if (request.expected_generation < current) {
    return Error(ErrorCode::StaleConfigurationGeneration,
                 "the request was planned against a superseded configuration")
        .with("expected", request.expected_generation.raw())
        .with("current", current.raw());
  }
  if (request.expected_generation > current) {
    return Error(ErrorCode::FutureConfigurationGeneration,
                 "the request names a configuration generation that does not exist yet")
        .with("expected", request.expected_generation.raw())
        .with("current", current.raw());
  }

  // Assign generations. An unchanged declaration keeps its zone generation; a
  // new or edited zone gets a new one, which drops its held evidence.
  const ConfigurationGeneration next_generation = current.next();
  const EvidenceGeneration next_evidence =
      impl.world.configuration.evidence_generation().next();
  ThermalPolicy policy = request.policy;
  if (same_policy(policy, impl.world.configuration.policy())) {
    policy.generation = impl.world.configuration.policy().generation;
    if (policy.generation.is_zero()) {
      policy.generation = PolicyGeneration::first();
    }
  } else {
    policy.generation = impl.world.configuration.policy().generation.is_zero()
                            ? PolicyGeneration::first()
                            : impl.world.configuration.policy().generation.next();
  }

  Configuration::ZoneList zones = canonicalise(request).zones;
  for (ZoneConfiguration& zone : zones) {
    const ZoneConfiguration* existing = impl.world.configuration.find(zone.id);
    if (existing == nullptr || !same_declaration(*existing, zone)) {
      zone.generation = existing == nullptr ? ZoneGeneration::first()
                                            : existing->generation.next();
    } else {
      zone.generation = existing->generation;
    }
  }

  std::vector<CouplingEdge> edges = request.coupling;
  std::sort(edges.begin(), edges.end(), [](const CouplingEdge& a, const CouplingEdge& b) {
    if (a.source != b.source) {
      return a.source < b.source;
    }
    return a.sink < b.sink;
  });
  std::vector<ZoneId> ids;
  ids.reserve(zones.size());
  for (const ZoneConfiguration& zone : zones) {
    ids.push_back(zone.id);
  }
  Result<CouplingGraph> coupling = CouplingGraph::create(std::move(edges), ids);
  if (!coupling.ok()) {
    return coupling.error();
  }
  Result<Configuration> built = Configuration::create(std::move(zones), std::move(coupling.value()),
                                                      policy, next_generation, next_evidence);
  if (!built.ok()) {
    return built.error();
  }

  WorldState next_world;
  next_world.configuration = std::move(built.value());
  next_world.fencing = impl.world.fencing;
  next_world.fencing.configuration_generation = next_generation;
  next_world.fencing.evidence_generation = next_evidence;
  next_world.fencing.policy_generation = policy.generation;
  next_world.fencing.revision = impl.world.fencing.revision.next();
  next_world.fencing.incarnation = impl.incarnation;
  next_world.fencing.commit = impl.world.fencing.commit.next();
  next_world.receipts = impl.world.receipts;

  for (const ZoneConfiguration& zone : next_world.configuration.zones()) {
    PersistedZoneState state;
    state.zone = zone.id;
    state.generation = zone.generation;
    const PersistedZoneState* existing = impl.world.find(zone.id);
    if (existing != nullptr && existing->generation == zone.generation) {
      state.has_observation = existing->has_observation;
      state.observation = existing->observation;
      state.recovered = existing->recovered;
    }
    state.derating = clamp_derating(existing != nullptr ? existing->derating : DeratingState{},
                                    zone.derate_steps);
    next_world.zones.push_back(state);
  }

  IdempotencyRecord record;
  record.command = request.command;
  record.kind = CommandKind::ApplyConfiguration;
  record.fingerprint = fingerprint;
  record.result_value = next_generation.raw();
  record.result_aux = next_evidence.raw();
  record.result_extra =
      (static_cast<std::uint64_t>(next_world.configuration.zone_count()) << 32) |
      static_cast<std::uint64_t>(next_world.configuration.coupling().edge_count());
  record.applied_revision = next_world.fencing.revision;
  record.applied_commit = next_world.fencing.commit;
  retain_receipt(next_world, record);

  ByteWriter writer;
  detail::encode_world(writer, next_world);
  if (writer.overflowed()) {
    return Error(ErrorCode::PayloadTooLarge, "the configuration does not fit in a store slot");
  }

  {
    const Status published = impl.publish(next_world, writer.bytes());
    if (!published.ok()) {
      return published.error();
    }
  }

  impl.world = std::move(next_world);
  impl.recovery.zone_count = impl.world.configuration.zone_count();
  impl.recovery.revision = impl.world.fencing.revision;
  impl.recovery.commit = impl.world.fencing.commit;

  ConfigurationReceipt receipt;
  receipt.generation = next_generation;
  receipt.evidence_generation = next_evidence;
  receipt.revision = impl.world.fencing.revision;
  receipt.commit = impl.world.fencing.commit;
  receipt.zone_count = impl.world.configuration.zone_count();
  receipt.coupling_edge_count = impl.world.configuration.coupling().edge_count();
  receipt.fingerprint = fingerprint;
  receipt.replayed = false;
  receipt.durable = impl.durable;
  return receipt;
}

Result<EpochReceipt> ThermalZoneEngine::advance_epoch(const EpochRequest& request) {
  Impl& impl = *impl_;
  {
    const Status allowed = impl.require_open_and_writable();
    if (!allowed.ok()) {
      return allowed.error();
    }
  }
  if (request.command.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "a mutation must carry a command identifier");
  }

  const std::uint64_t fingerprint = epoch_request_fingerprint(request);
  const std::unique_lock<std::shared_mutex> guard(impl.state_mutex);
  {
    const Status allowed = impl.require_open_locked();
    if (!allowed.ok()) {
      return allowed.error();
    }
  }

  if (const IdempotencyRecord* replay = find_receipt(impl.world, request.command)) {
    if (replay->fingerprint != fingerprint || replay->kind != CommandKind::AdvanceEpoch) {
      return Error(ErrorCode::IdempotencyConflict,
                   "this command identifier was already used for a different request")
          .with("command", request.command.raw());
    }
    EpochReceipt receipt;
    receipt.previous = ControlPlaneEpoch::from_value(replay->result_value);
    receipt.current = ControlPlaneEpoch::from_value(replay->result_aux);
    receipt.revision = replay->applied_revision;
    receipt.commit = replay->applied_commit;
    receipt.fingerprint = fingerprint;
    receipt.replayed = true;
    receipt.durable = impl.durable;
    return receipt;
  }

  if (request.expected_current != impl.world.fencing.epoch) {
    return Error(ErrorCode::EpochMismatch,
                 "the epoch advance was planned against a different control-plane epoch")
        .with("expected", request.expected_current.raw())
        .with("current", impl.world.fencing.epoch.raw());
  }
  if (request.target <= request.expected_current) {
    return Error(ErrorCode::EpochRegression,
                 "a control-plane epoch may only move forward")
        .with("current", request.expected_current.raw())
        .with("target", request.target.raw());
  }

  WorldState next_world = impl.world;
  next_world.fencing.epoch = request.target;
  next_world.fencing.revision = impl.world.fencing.revision.next();
  next_world.fencing.incarnation = impl.incarnation;
  next_world.fencing.commit = impl.world.fencing.commit.next();

  IdempotencyRecord record;
  record.command = request.command;
  record.kind = CommandKind::AdvanceEpoch;
  record.fingerprint = fingerprint;
  record.result_value = request.expected_current.raw();
  record.result_aux = request.target.raw();
  record.applied_revision = next_world.fencing.revision;
  record.applied_commit = next_world.fencing.commit;
  retain_receipt(next_world, record);

  ByteWriter writer;
  detail::encode_world(writer, next_world);
  if (writer.overflowed()) {
    return Error(ErrorCode::PayloadTooLarge, "the state does not fit in a store slot");
  }

  {
    const Status published = impl.publish(next_world, writer.bytes());
    if (!published.ok()) {
      return published.error();
    }
  }

  impl.world = std::move(next_world);

  EpochReceipt receipt;
  receipt.previous = request.expected_current;
  receipt.current = request.target;
  receipt.revision = impl.world.fencing.revision;
  receipt.commit = impl.world.fencing.commit;
  receipt.fingerprint = fingerprint;
  receipt.replayed = false;
  receipt.durable = impl.durable;
  return receipt;
}

namespace {

bool same_observation(const TemperatureObservation& a, const TemperatureObservation& b) {
  return a.zone == b.zone && a.zone_generation == b.zone_generation &&
         a.evidence_generation == b.evidence_generation && a.sequence == b.sequence &&
         a.temperature == b.temperature && a.heat == b.heat && a.source == b.source &&
         a.provenance == b.provenance && a.observed_at == b.observed_at &&
         a.validity == b.validity && a.publisher_epoch == b.publisher_epoch &&
         a.publisher_incarnation == b.publisher_incarnation;
}

}  // namespace

Result<ObservationReceipt> ThermalZoneEngine::ingest_observation(
    const TemperatureObservation& observation) {
  Impl& impl = *impl_;
  {
    const Status allowed = impl.require_open_and_writable();
    if (!allowed.ok()) {
      return allowed.error();
    }
  }
  const Status valid = observation.validate();
  if (!valid.ok()) {
    return valid.error();
  }

  const std::unique_lock<std::shared_mutex> guard(impl.state_mutex);
  {
    const Status allowed = impl.require_open_locked();
    if (!allowed.ok()) {
      return allowed.error();
    }
  }

  if (observation.publisher_epoch.is_zero()) {
    return Error(ErrorCode::MissingEpoch,
                 "an observation must carry the epoch it was published under");
  }
  if (observation.publisher_epoch != impl.world.fencing.epoch) {
    return Error(ErrorCode::EpochMismatch,
                 "the observation was published under a different control-plane epoch")
        .with("publisher_epoch", observation.publisher_epoch.raw())
        .with("current_epoch", impl.world.fencing.epoch.raw());
  }

  const ZoneConfiguration* zone = impl.world.configuration.find(observation.zone);
  if (zone == nullptr) {
    return Error(ErrorCode::UnknownZone, "the observation names a zone that is not declared")
        .with("zone", observation.zone.raw());
  }
  if (observation.zone_generation < zone->generation) {
    return Error(ErrorCode::StaleZoneGeneration,
                 "the observation was taken against a superseded zone definition")
        .with("zone", observation.zone.raw())
        .with("observed_generation", observation.zone_generation.raw())
        .with("current_generation", zone->generation.raw());
  }
  if (observation.zone_generation > zone->generation) {
    return Error(ErrorCode::FutureZoneGeneration,
                 "the observation names a zone generation that does not exist yet")
        .with("zone", observation.zone.raw())
        .with("observed_generation", observation.zone_generation.raw())
        .with("current_generation", zone->generation.raw());
  }
  const EvidenceGeneration current_evidence = impl.world.configuration.evidence_generation();
  if (observation.evidence_generation < current_evidence) {
    return Error(ErrorCode::StaleEvidenceGeneration,
                 "the observation was taken against a superseded evidence generation")
        .with("observed_generation", observation.evidence_generation.raw())
        .with("current_generation", current_evidence.raw());
  }
  if (observation.evidence_generation > current_evidence) {
    return Error(ErrorCode::FutureEvidenceGeneration,
                 "the observation names an evidence generation that does not exist yet")
        .with("observed_generation", observation.evidence_generation.raw())
        .with("current_generation", current_evidence.raw());
  }

  const PersistedZoneState* existing = impl.world.find(observation.zone);
  bool replay = false;
  if (existing != nullptr && existing->has_observation) {
    if (observation.sequence < existing->observation.sequence) {
      return Error(ErrorCode::StaleSequence,
                   "the observation reuses a sequence that was already superseded")
          .with("zone", observation.zone.raw())
          .with("sequence", observation.sequence.raw())
          .with("accepted", existing->observation.sequence.raw());
    }
    if (observation.sequence == existing->observation.sequence) {
      if (!same_observation(observation, existing->observation)) {
        return Error(ErrorCode::ConflictingObservation,
                     "an observation sequence was reused with a different payload")
            .with("zone", observation.zone.raw())
            .with("sequence", observation.sequence.raw());
      }
      // An identical redelivery replays the original outcome. It does not
      // refresh the lifecycle of the held value: only a strictly newer
      // sequence clears the recovered marker.
      replay = true;
    }
  }

  const std::uint64_t fingerprint = [&observation]() {
    ByteWriter writer;
    writer.put_u8(static_cast<std::uint8_t>(CommandKind::IngestObservation));
    writer.put_u32(static_cast<std::uint32_t>(observation.zone.raw()));
    writer.put_u64(observation.zone_generation.raw());
    writer.put_u64(observation.evidence_generation.raw());
    writer.put_u64(observation.sequence.raw());
    writer.put_i64(observation.temperature.value);
    writer.put_optional_i64(observation.heat.has_value(),
                            observation.heat.has_value() ? observation.heat->value : 0);
    writer.put_text(observation.source.str());
    writer.put_u8(static_cast<std::uint8_t>(observation.provenance));
    writer.put_i64(observation.observed_at.value);
    writer.put_i64(observation.validity.value);
    writer.put_u64(observation.publisher_epoch.raw());
    writer.put_u64(observation.publisher_incarnation.raw());
    return fingerprint_bytes(writer);
  }();

  if (replay) {
    ObservationReceipt receipt;
    receipt.zone = observation.zone;
    receipt.sequence = existing->observation.sequence;
    receipt.revision = impl.world.fencing.revision;
    receipt.commit = impl.world.fencing.commit;
    receipt.fingerprint = fingerprint;
    receipt.replayed = true;
    receipt.durable = impl.durable;
    return receipt;
  }

  WorldState next_world = impl.world;
  PersistedZoneState* target = next_world.find(observation.zone);
  if (target == nullptr) {
    PersistedZoneState fresh;
    fresh.zone = observation.zone;
    fresh.generation = zone->generation;
    next_world.zones.push_back(fresh);
    std::sort(next_world.zones.begin(), next_world.zones.end(),
              [](const PersistedZoneState& a, const PersistedZoneState& b) {
                return a.zone < b.zone;
              });
    target = next_world.find(observation.zone);
  }
  target->generation = zone->generation;
  target->has_observation = true;
  target->observation = observation;
  target->recovered = false;

  next_world.fencing.revision = impl.world.fencing.revision.next();
  next_world.fencing.incarnation = impl.incarnation;
  next_world.fencing.commit = impl.world.fencing.commit.next();

  ByteWriter writer;
  detail::encode_world(writer, next_world);
  if (writer.overflowed()) {
    return Error(ErrorCode::PayloadTooLarge, "the state does not fit in a store slot");
  }

  {
    const Status published = impl.publish(next_world, writer.bytes());
    if (!published.ok()) {
      return published.error();
    }
  }

  impl.world = std::move(next_world);
  impl.recovery.observation_count = 0;
  for (const PersistedZoneState& state : impl.world.zones) {
    if (state.has_observation) {
      ++impl.recovery.observation_count;
    }
  }
  impl.recovery.revision = impl.world.fencing.revision;
  impl.recovery.commit = impl.world.fencing.commit;

  ObservationReceipt receipt;
  receipt.zone = observation.zone;
  receipt.sequence = observation.sequence;
  receipt.revision = impl.world.fencing.revision;
  receipt.commit = impl.world.fencing.commit;
  receipt.fingerprint = fingerprint;
  receipt.replayed = false;
  receipt.durable = impl.durable;
  return receipt;
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

Result<EvaluationResult> ThermalZoneEngine::evaluate(const EvaluationRequest& request) {
  Impl& impl = *impl_;
  if (impl.closed.load()) {
    return Error(ErrorCode::StoreClosed, "the engine has been closed");
  }
  if (request.epoch.is_zero()) {
    return Error(ErrorCode::MissingEpoch, "an evaluation must carry a control-plane epoch");
  }

  EvaluationResult result;
  {
    const std::shared_lock<std::shared_mutex> guard(impl.state_mutex);
    if (request.epoch != impl.world.fencing.epoch) {
      return Error(ErrorCode::EpochMismatch,
                   "the evaluation was planned under a different control-plane epoch")
          .with("request_epoch", request.epoch.raw())
          .with("current_epoch", impl.world.fencing.epoch.raw());
    }
    const ConfigurationGeneration current_config = impl.world.configuration.generation();
    if (request.configuration_generation < current_config) {
      return Error(ErrorCode::StaleConfigurationGeneration,
                   "the evaluation was planned against a superseded configuration")
          .with("expected", request.configuration_generation.raw())
          .with("current", current_config.raw());
    }
    if (request.configuration_generation > current_config) {
      return Error(ErrorCode::FutureConfigurationGeneration,
                   "the evaluation names a configuration generation that does not exist yet")
          .with("expected", request.configuration_generation.raw())
          .with("current", current_config.raw());
    }
    const EvidenceGeneration current_evidence = impl.world.configuration.evidence_generation();
    if (request.evidence_generation < current_evidence) {
      return Error(ErrorCode::StaleEvidenceGeneration,
                   "the evaluation was planned against a superseded evidence generation")
          .with("expected", request.evidence_generation.raw())
          .with("current", current_evidence.raw());
    }
    if (request.evidence_generation > current_evidence) {
      return Error(ErrorCode::FutureEvidenceGeneration,
                   "the evaluation names an evidence generation that does not exist yet")
          .with("expected", request.evidence_generation.raw())
          .with("current", current_evidence.raw());
    }
    if (request.basis_revision.has_value() &&
        *request.basis_revision != impl.world.fencing.revision) {
      return Error(ErrorCode::StaleStateRevision,
                   "the evaluation was planned against a superseded store revision")
          .with("expected", request.basis_revision->raw())
          .with("current", impl.world.fencing.revision.raw());
    }

    std::vector<ZoneId> selected;
    if (request.zones.empty()) {
      selected = impl.world.configuration.zone_ids();
    } else {
      selected = request.zones;
      std::sort(selected.begin(), selected.end());
      selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
      for (const ZoneId zone : selected) {
        if (impl.world.configuration.find(zone) == nullptr) {
          return Error(ErrorCode::UnknownZone, "the evaluation names a zone that is not declared")
              .with("zone", zone.raw());
        }
      }
    }
    if (selected.size() > kMaxZonesPerEvaluation) {
      return Error(ErrorCode::TooManyZones, "the evaluation exceeds its zone ceiling")
          .with("zones", static_cast<std::uint64_t>(selected.size()));
    }

    const Configuration& configuration = impl.world.configuration;
    const Timestamp now = impl.clock->now();

    // Heat figures and freshness are computed once per declared zone so that
    // the coupling pass is a pure function of the whole world, independent of
    // which subset was requested.
    std::vector<HeatFigure> figures(configuration.zones().size());
    std::vector<EvidenceFreshness> freshness(configuration.zones().size());
    for (std::size_t index = 0; index < configuration.zones().size(); ++index) {
      const ZoneConfiguration& zone = configuration.zones()[index];
      const ZoneEvidence evidence = impl.evidence_for_zone(zone.id);
      const EvidenceFreshness verdict =
          classify_freshness(evidence, now, configuration.evidence_generation(),
                             configuration.policy().future_tolerance);
      freshness[index] = verdict;
      figures[index] = impl.heat_figure(zone, evidence, is_usable(verdict));
    }

    Result<CouplingField> field = impl.propagate(configuration, figures);
    if (!field.ok()) {
      return field.error();
    }

    for (const ZoneId zone_id : selected) {
      const ZoneConfiguration* zone = configuration.find(zone_id);
      if (zone == nullptr) {
        return Error(ErrorCode::UnknownZone, "the evaluation names a zone that is not declared")
            .with("zone", zone_id.raw());
      }
      const auto position = static_cast<std::size_t>(std::distance(
          configuration.zones().begin(),
          std::lower_bound(configuration.zones().begin(), configuration.zones().end(), zone_id,
                           [](const ZoneConfiguration& candidate, ZoneId value) {
                             return candidate.id < value;
                           })));

      const ZoneEvidence evidence = impl.evidence_for_zone(zone_id);
      const EvidenceFreshness verdict = freshness[position];
      const bool usable = is_usable(verdict);

      ZoneThermalState state;
      state.zone = zone->id;
      state.name = zone->name;
      state.zone_generation = zone->generation;
      state.observation_present = evidence.present;
      state.evidence_freshness = verdict;
      if (evidence.present) {
        state.observed_temperature = evidence.observation.temperature;
        state.observed_at = evidence.observation.observed_at;
        state.source = evidence.observation.source;
        state.provenance = evidence.observation.provenance;
      }
      state.observed_temperature_known = usable;
      state.heat_known = figures[position].known;
      state.heat_source = figures[position].source;
      state.heat = figures[position].value;

      state.coupled_heat = MilliWatts{field.value().incoming_heat[position]};
      state.coupled_heat_known = field.value().known[position];
      state.unknown_neighbour_count = field.value().unknown_contributors[position];
      state.coupling_hops_used = field.value().hops_used;
      state.coupling_refinement_refused = field.value().refinement_refused;

      const std::optional<std::int64_t> rise = mul_div_ceil(
          state.coupled_heat.value, zone->thermal_resistance.value, 1000000);
      if (!rise.has_value()) {
        return Error(ErrorCode::ArithmeticOverflow, "the coupled temperature rise overflowed")
            .with("zone", zone_id.raw());
      }
      state.coupled_rise = MilliCelsius{rise.value()};

      std::optional<MilliCelsius> effective;
      if (usable) {
        if (!state.coupled_heat_known) {
          state.effective_temperature_known = false;
        } else {
          const std::optional<std::int64_t> sum =
              add_checked(state.observed_temperature.value, state.coupled_rise.value);
          if (!sum.has_value()) {
            return Error(ErrorCode::ArithmeticOverflow,
                         "the effective temperature overflowed")
                .with("zone", zone_id.raw());
          }
          effective = MilliCelsius{sum.value()};
          state.effective_temperature = *effective;
          state.effective_temperature_known = true;
        }
      }

      if (state.effective_temperature_known) {
        if (!is_plausible(state.effective_temperature)) {
          return Error(ErrorCode::InvalidTemperature,
                       "the effective temperature is not physically plausible")
              .with("zone", zone_id.raw())
              .with("millicelsius", state.effective_temperature.value);
        }
        state.band = classify(zone->envelope, state.effective_temperature);
        state.band_known = true;
      }

      // -- headroom ---------------------------------------------------------
      if (!usable) {
        state.headroom.status = HeadroomStatus::Unknown;
      } else if (!state.effective_temperature_known) {
        state.headroom.status = HeadroomStatus::Indeterminate;
      } else {
        state.headroom.status = HeadroomStatus::Known;
        const std::optional<std::int64_t> margin =
            sub_checked(zone->envelope.ceiling_temp.value, state.effective_temperature.value);
        if (!margin.has_value()) {
          return Error(ErrorCode::ArithmeticOverflow, "the temperature margin overflowed")
              .with("zone", zone_id.raw());
        }
        state.headroom.temperature_margin = MilliCelsius{margin.value()};
        const std::optional<std::int64_t> power =
            mul_div_floor(margin.value(), 1000000, zone->thermal_resistance.value);
        if (!power.has_value()) {
          return Error(ErrorCode::ArithmeticOverflow, "the power margin overflowed")
              .with("zone", zone_id.raw());
        }
        state.headroom.power_margin = MilliWatts{power.value()};
      }

      // -- derating with hysteresis -----------------------------------------
      const DeratingState before = impl.derating_for_zone(zone_id);
      const DeratingOutcome outcome =
          advance_derating(before, zone->envelope, zone->derate_steps, zone->recovery_margin,
                           configuration.policy().release_hold_observations, effective);
      state.derating_before = before;
      state.derating_after = outcome.state;
      state.instant_derate_level = outcome.instant_level;
      state.derate_fraction = outcome.fraction;
      state.derating_escalated = outcome.escalated;
      state.derating_released = outcome.released;
      state.derating_held_for_evidence = outcome.held_for_evidence;

      // -- emitted allowance ------------------------------------------------
      if (state.headroom.status != HeadroomStatus::Known) {
        state.allowance_known = false;
        state.placement_allowance = MilliWatts{0};
        state.constraint_kind = PlacementConstraintKind::Indeterminate;
      } else if (state.headroom.temperature_margin.value <= 0 ||
                 state.headroom.power_margin.value <= 0) {
        state.allowance_known = true;
        state.placement_allowance = MilliWatts{0};
        state.constraint_kind = PlacementConstraintKind::Prohibited;
      } else {
        const std::int64_t remaining =
            static_cast<std::int64_t>(kPpmScale) - state.derate_fraction.value;
        const std::optional<std::int64_t> allowance = mul_div_floor(
            state.headroom.power_margin.value, remaining, static_cast<std::int64_t>(kPpmScale));
        if (!allowance.has_value()) {
          return Error(ErrorCode::ArithmeticOverflow, "the placement allowance overflowed")
              .with("zone", zone_id.raw());
        }
        state.allowance_known = true;
        state.placement_allowance = MilliWatts{allowance.value()};
        state.constraint_kind = allowance.value() > 0 ? PlacementConstraintKind::Bounded
                                                      : PlacementConstraintKind::Prohibited;
      }

      // -- reasons ----------------------------------------------------------
      switch (verdict) {
        case EvidenceFreshness::Fresh:
          push_reason(state.reasons, ConstraintReason::EvidenceFresh);
          break;
        case EvidenceFreshness::Missing:
        case EvidenceFreshness::Unsupported:
          push_reason(state.reasons, ConstraintReason::EvidenceMissing);
          break;
        case EvidenceFreshness::Future:
          push_reason(state.reasons, ConstraintReason::EvidenceFuture);
          break;
        case EvidenceFreshness::Recovered:
          push_reason(state.reasons, ConstraintReason::EvidenceRecovered);
          break;
        case EvidenceFreshness::Stale:
          push_reason(state.reasons,
                      evidence.present && evidence.observation.evidence_generation !=
                                               configuration.evidence_generation()
                          ? ConstraintReason::EvidenceSuperseded
                          : ConstraintReason::EvidenceStale);
          break;
      }
      if (state.unknown_neighbour_count > 0) {
        push_reason(state.reasons, ConstraintReason::NeighbourHeatUnknown);
      }
      if (state.coupled_heat.value > 0) {
        push_reason(state.reasons, ConstraintReason::CouplingApplied);
      }
      if (state.coupling_refinement_refused) {
        push_reason(state.reasons, ConstraintReason::CouplingNotContractive);
      }
      if (state.band_known) {
        switch (state.band) {
          case ThermalBand::BelowFloor:
            push_reason(state.reasons, ConstraintReason::BelowFloor);
            break;
          case ThermalBand::Nominal:
            break;
          case ThermalBand::Derating:
            push_reason(state.reasons, ConstraintReason::DeratingActive);
            break;
          case ThermalBand::AtLimit:
            push_reason(state.reasons, ConstraintReason::AtCeiling);
            break;
          case ThermalBand::Critical:
            push_reason(state.reasons, ConstraintReason::AtCeiling);
            push_reason(state.reasons, ConstraintReason::AtCritical);
            break;
        }
        if (state.effective_temperature.value > zone->envelope.ceiling_temp.value) {
          push_reason(state.reasons, ConstraintReason::AboveCeiling);
        }
      }
      if (state.derate_fraction.value > 0) {
        push_reason(state.reasons, ConstraintReason::DeratingActive);
      }
      if (state.derate_fraction.value >= kPpmScale) {
        push_reason(state.reasons, ConstraintReason::DeratingFull);
      }
      if (state.headroom.status == HeadroomStatus::Known &&
          state.derate_fraction.value > 0) {
        push_reason(state.reasons, ConstraintReason::AllowanceReducedByDerating);
      }
      if (state.allowance_known && state.placement_allowance.value == 0) {
        push_reason(state.reasons, ConstraintReason::AllowanceExhausted);
      }

      result.zones_.push_back(state);

      PlacementConstraint constraint;
      constraint.zone = state.zone;
      constraint.zone_generation = state.zone_generation;
      constraint.configuration_generation = configuration.generation();
      constraint.evidence_generation = configuration.evidence_generation();
      constraint.policy_generation = configuration.policy().generation;
      constraint.evaluation = request.id;
      constraint.basis_revision = impl.world.fencing.revision;
      constraint.kind = state.constraint_kind;
      constraint.allowance_known = state.allowance_known;
      constraint.max_additional_heat = state.placement_allowance;
      constraint.band = state.band;
      constraint.band_known = state.band_known;
      constraint.derate_fraction = state.derate_fraction;
      constraint.reasons = state.reasons;
      result.constraints_.push_back(std::move(constraint));
    }

    result.id_ = request.id;
    result.evaluated_at_ = now;
    result.epoch_ = impl.world.fencing.epoch;
    result.configuration_generation_ = configuration.generation();
    result.evidence_generation_ = configuration.evidence_generation();
    result.policy_generation_ = configuration.policy().generation;
    result.basis_revision_ = impl.world.fencing.revision;
  }

  result.seal();

  {
    const std::lock_guard<std::mutex> cache_guard(impl.cache_mutex);
    impl.recent.push_back(result);
    while (impl.recent.size() > kRecentEvaluationCapacity) {
      impl.recent.pop_front();
    }
  }
  return result;
}

Result<EvaluationCommitReceipt> ThermalZoneEngine::commit_evaluation(
    const EvaluationCommitRequest& request) {
  Impl& impl = *impl_;
  {
    const Status allowed = impl.require_open_and_writable();
    if (!allowed.ok()) {
      return allowed.error();
    }
  }
  if (request.command.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "a mutation must carry a command identifier");
  }

  // The evaluation is recovered from this engine's own bounded cache, so a
  // caller can never hand in a fabricated derating outcome.
  EvaluationResult evaluation;
  {
    const std::lock_guard<std::mutex> cache_guard(impl.cache_mutex);
    const auto found = std::find_if(impl.recent.begin(), impl.recent.end(),
                                    [&request](const EvaluationResult& candidate) {
                                      return candidate.id() == request.evaluation;
                                    });
    if (found == impl.recent.end()) {
      return Error(ErrorCode::UnknownCommand,
                   "the evaluation identifier was not produced by this engine")
          .with("evaluation", request.evaluation.raw());
    }
    evaluation = *found;
  }

  const std::uint64_t fingerprint = evaluation_commit_fingerprint(evaluation, request);
  const std::unique_lock<std::shared_mutex> guard(impl.state_mutex);
  {
    const Status allowed = impl.require_open_locked();
    if (!allowed.ok()) {
      return allowed.error();
    }
  }

  if (const IdempotencyRecord* replay = find_receipt(impl.world, request.command)) {
    if (replay->fingerprint != fingerprint || replay->kind != CommandKind::CommitEvaluation) {
      return Error(ErrorCode::IdempotencyConflict,
                   "this command identifier was already used for a different request")
          .with("command", request.command.raw());
    }
    EvaluationCommitReceipt receipt;
    receipt.evaluation = request.evaluation;
    receipt.revision = replay->applied_revision;
    receipt.commit = replay->applied_commit;
    receipt.zones_updated = static_cast<std::size_t>(replay->result_value);
    receipt.fingerprint = fingerprint;
    receipt.replayed = true;
    receipt.durable = impl.durable;
    return receipt;
  }

  const Status epoch_status = check_epoch(impl.world.fencing, request.epoch);
  if (!epoch_status.ok()) {
    return epoch_status.error();
  }
  if (request.configuration_generation < impl.world.configuration.generation() ||
      request.evidence_generation < impl.world.configuration.evidence_generation() ||
      request.basis_revision < impl.world.fencing.revision) {
    return Error(ErrorCode::StaleEvaluation,
                 "the state moved on after the evaluation was computed")
        .with("evaluation_revision", request.basis_revision.raw())
        .with("current_revision", impl.world.fencing.revision.raw());
  }
  if (request.configuration_generation > impl.world.configuration.generation() ||
      request.evidence_generation > impl.world.configuration.evidence_generation() ||
      request.basis_revision > impl.world.fencing.revision) {
    return Error(ErrorCode::StaleEvaluation,
                 "the evaluation names a state that does not exist yet")
        .with("evaluation_revision", request.basis_revision.raw())
        .with("current_revision", impl.world.fencing.revision.raw());
  }

  WorldState next_world = impl.world;
  std::size_t updated = 0;
  for (const ZoneThermalState& state : evaluation.zones()) {
    PersistedZoneState* target = next_world.find(state.zone);
    if (target == nullptr || !(target->generation == state.zone_generation)) {
      return Error(ErrorCode::StaleEvaluation,
                   "a zone changed after the evaluation was computed")
          .with("zone", state.zone.raw());
    }
    target->derating = state.derating_after;
    ++updated;
  }

  next_world.fencing.revision = impl.world.fencing.revision.next();
  next_world.fencing.incarnation = impl.incarnation;
  next_world.fencing.commit = impl.world.fencing.commit.next();

  IdempotencyRecord record;
  record.command = request.command;
  record.kind = CommandKind::CommitEvaluation;
  record.fingerprint = fingerprint;
  record.result_value = static_cast<std::uint64_t>(updated);
  record.applied_revision = next_world.fencing.revision;
  record.applied_commit = next_world.fencing.commit;
  retain_receipt(next_world, record);

  ByteWriter writer;
  detail::encode_world(writer, next_world);
  if (writer.overflowed()) {
    return Error(ErrorCode::PayloadTooLarge, "the state does not fit in a store slot");
  }

  {
    const Status published = impl.publish(next_world, writer.bytes());
    if (!published.ok()) {
      return published.error();
    }
  }

  impl.world = std::move(next_world);

  EvaluationCommitReceipt receipt;
  receipt.evaluation = request.evaluation;
  receipt.revision = impl.world.fencing.revision;
  receipt.commit = impl.world.fencing.commit;
  receipt.zones_updated = updated;
  receipt.fingerprint = fingerprint;
  receipt.replayed = false;
  receipt.durable = impl.durable;
  return receipt;
}

// ---------------------------------------------------------------------------
// Human-readable rendering
// ---------------------------------------------------------------------------

std::string render(const Configuration& configuration) {
  std::string text = "configuration generation " + to_string(configuration.generation()) +
                     ", evidence generation " + to_string(configuration.evidence_generation()) +
                     ", policy generation " + to_string(configuration.policy().generation) + "\n";
  text += "  policy: coupling hops " +
          std::to_string(static_cast<unsigned long long>(configuration.policy().coupling_hops)) +
          ", release hold " +
          std::to_string(
              static_cast<unsigned long long>(configuration.policy().release_hold_observations)) +
          ", future tolerance " +
          std::to_string(configuration.policy().future_tolerance.value) + " ns\n";
  for (const ZoneConfiguration& zone : configuration.zones()) {
    text += "  zone " + to_string(zone.id) + " (" + zone.name.str() + ") generation " +
            to_string(zone.generation) + "\n";
    text += "    envelope      : floor " + format_milli(zone.envelope.floor_temp.value) +
            " C, derate onset " + format_milli(zone.envelope.derate_onset.value) +
            " C, ceiling " + format_milli(zone.envelope.ceiling_temp.value) +
            " C, critical " + format_milli(zone.envelope.critical_temp.value) + " C\n";
    text += "    resistance    : " + std::to_string(zone.thermal_resistance.value) + " uK/W\n";
    text += "    declared heat : ";
    if (zone.declared_heat.has_value()) {
      text += format_milli(zone.declared_heat->value) + " W";
    } else {
      text += "not declared";
    }
    text += "\n";
    text += "    derate ladder : " +
            std::to_string(static_cast<unsigned long long>(zone.derate_steps)) +
            " step(s), release margin " + format_milli(zone.recovery_margin.value) + " C\n";
  }
  for (const CouplingEdge& edge : configuration.coupling().edges()) {
    text += "  coupling " + to_string(edge.source) + " -> " + to_string(edge.sink) + " at " +
            std::to_string(static_cast<long long>(edge.coefficient.value)) + " ppm\n";
  }
  return text;
}

std::string render(const FencingState& fencing) {
  std::string text = "epoch " + to_string(fencing.epoch) + ", incarnation " +
                     to_string(fencing.incarnation) + ", revision " +
                     to_string(fencing.revision) + ", commit " + to_string(fencing.commit) + "\n";
  text += "  configuration generation " + to_string(fencing.configuration_generation) +
          ", evidence generation " + to_string(fencing.evidence_generation) +
          ", policy generation " + to_string(fencing.policy_generation) + "\n";
  return text;
}

std::string render(const RecoveryReport& report) {
  std::string text;
  text += std::string("store existed     : ") + (report.store_existed ? "yes" : "no") + "\n";
  text += std::string("store created     : ") + (report.store_created ? "yes" : "no") + "\n";
  text += std::string("generation found  : ") + (report.recovered ? "yes" : "no") + "\n";
  text += "slot              : " + std::to_string(report.slot_index) + "\n";
  text += "commit sequence   : " + to_string(report.commit) + "\n";
  text += "store revision    : " + to_string(report.revision) + "\n";
  text += "zones             : " +
          std::to_string(static_cast<unsigned long long>(report.zone_count)) + "\n";
  text += "observations      : " +
          std::to_string(static_cast<unsigned long long>(report.observation_count)) +
          " (all marked recovered)\n";
  if (report.abandoned_torn_slot) {
    text += "abandoned slot    : torn (" + report.abandoned_reason + ")\n";
  } else if (report.abandoned_older_slot) {
    text += "abandoned slot    : older valid generation\n";
  }
  return text;
}

std::string render(const ConfigurationReceipt& receipt) {
  std::string text = "configuration generation " + to_string(receipt.generation) +
                     ", evidence generation " + to_string(receipt.evidence_generation) + "\n";
  text += "  store revision " + to_string(receipt.revision) + ", commit " +
          to_string(receipt.commit) + "\n";
  text += "  zones " + std::to_string(static_cast<unsigned long long>(receipt.zone_count)) +
          ", coupling edges " +
          std::to_string(static_cast<unsigned long long>(receipt.coupling_edge_count)) + "\n";
  text += std::string("  replayed ") + (receipt.replayed ? "yes" : "no") + ", durable " +
          (receipt.durable ? "yes" : "no") + "\n";
  return text;
}

std::string render(const ObservationReceipt& receipt) {
  std::string text = "zone " + to_string(receipt.zone) + " sequence " +
                     to_string(receipt.sequence) + "\n";
  text += "  store revision " + to_string(receipt.revision) + ", commit " +
          to_string(receipt.commit) + "\n";
  text += std::string("  replayed ") + (receipt.replayed ? "yes" : "no") + ", durable " +
          (receipt.durable ? "yes" : "no") + "\n";
  return text;
}

std::string render(const EvaluationCommitReceipt& receipt) {
  std::string text = "evaluation " + to_string(receipt.evaluation) + ", zones updated " +
                     std::to_string(static_cast<unsigned long long>(receipt.zones_updated)) + "\n";
  text += "  store revision " + to_string(receipt.revision) + ", commit " +
          to_string(receipt.commit) + "\n";
  text += std::string("  replayed ") + (receipt.replayed ? "yes" : "no") + ", durable " +
          (receipt.durable ? "yes" : "no") + "\n";
  return text;
}

}  // namespace thermal_zone_manager
