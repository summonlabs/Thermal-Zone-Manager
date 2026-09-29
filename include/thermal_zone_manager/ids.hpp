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

#ifndef THERMAL_ZONE_MANAGER_IDS_HPP
#define THERMAL_ZONE_MANAGER_IDS_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "thermal_zone_manager/errors.hpp"
#include "thermal_zone_manager/export.hpp"

// Every identity in this runtime is a distinct type. A zone handle, a zone
// generation, a configuration generation, an evidence generation, a store
// revision, a control-plane epoch, an observation sequence, an evaluation
// identifier, a command identifier and a commit sequence all mean different
// things and are never interchangeable.

namespace thermal_zone_manager {

// Maximum length in bytes of any text identity stored by this runtime.
inline constexpr std::size_t kMaxTextLength = 128;

namespace detail {

// Common implementation for the monotone counters. Distinct Tag types produce
// distinct, non-convertible strong types.
template <class Tag>
struct CounterValue {
  using rep = std::uint64_t;

  rep value = 0;

  static constexpr CounterValue first() { return CounterValue{1}; }
  static constexpr CounterValue zero() { return CounterValue{0}; }
  static constexpr CounterValue from_value(rep raw) { return CounterValue{raw}; }

  constexpr rep raw() const { return value; }
  constexpr bool is_zero() const { return value == 0; }
  constexpr bool is_set() const { return value != 0; }
  constexpr CounterValue next() const { return CounterValue{value + 1}; }

  friend constexpr bool operator==(CounterValue a, CounterValue b) { return a.value == b.value; }
  friend constexpr bool operator!=(CounterValue a, CounterValue b) { return a.value != b.value; }
  friend constexpr bool operator<(CounterValue a, CounterValue b) { return a.value < b.value; }
  friend constexpr bool operator<=(CounterValue a, CounterValue b) { return a.value <= b.value; }
  friend constexpr bool operator>(CounterValue a, CounterValue b) { return a.value > b.value; }
  friend constexpr bool operator>=(CounterValue a, CounterValue b) { return a.value >= b.value; }
};

}  // namespace detail

struct ZoneIdTag;
struct ZoneGenerationTag;
struct ConfigurationGenerationTag;
struct EvidenceGenerationTag;
struct StoreRevisionTag;
struct ControlPlaneEpochTag;
struct CommitSequenceTag;
struct ObservationSequenceTag;
struct EvaluationIdTag;
struct CommandIdTag;
struct AttemptIdTag;
struct ControllerIncarnationTag;
struct PolicyGenerationTag;

// Operator-assigned zone handle. Zone 0 is reserved and never valid.
using ZoneId = detail::CounterValue<ZoneIdTag>;
// Bumped whenever a single zone's declared configuration is replaced.
using ZoneGeneration = detail::CounterValue<ZoneGenerationTag>;
// Bumped whenever the whole declared configuration is replaced.
using ConfigurationGeneration = detail::CounterValue<ConfigurationGenerationTag>;
// Bumped whenever the coupling graph or the evidence topology changes.
using EvidenceGeneration = detail::CounterValue<EvidenceGenerationTag>;
// Bumped on every durable mutation.
using StoreRevision = detail::CounterValue<StoreRevisionTag>;
// Operator-declared control-plane epoch. Advancing it fences all prior authority.
using ControlPlaneEpoch = detail::CounterValue<ControlPlaneEpochTag>;
// Monotone counter of successful durable commits in one store lineage.
using CommitSequence = detail::CounterValue<CommitSequenceTag>;
// Monotone per-zone counter of accepted observations.
using ObservationSequence = detail::CounterValue<ObservationSequenceTag>;
// Identity of one evaluation pass.
using EvaluationId = detail::CounterValue<EvaluationIdTag>;
// Identity of one requested mutation, used for idempotent retry.
using CommandId = detail::CounterValue<CommandIdTag>;
// Identity of one delivery attempt of a command.
using AttemptId = detail::CounterValue<AttemptIdTag>;
// Identity of one controller process incarnation.
using ControllerIncarnation = detail::CounterValue<ControllerIncarnationTag>;
// Bumped whenever the declared thermal policy is replaced.
using PolicyGeneration = detail::CounterValue<PolicyGenerationTag>;

TZM_API std::string to_string(ZoneId value);
TZM_API std::string to_string(ZoneGeneration value);
TZM_API std::string to_string(ConfigurationGeneration value);
TZM_API std::string to_string(EvidenceGeneration value);
TZM_API std::string to_string(StoreRevision value);
TZM_API std::string to_string(ControlPlaneEpoch value);
TZM_API std::string to_string(CommitSequence value);
TZM_API std::string to_string(ObservationSequence value);
TZM_API std::string to_string(EvaluationId value);
TZM_API std::string to_string(CommandId value);
TZM_API std::string to_string(AttemptId value);
TZM_API std::string to_string(ControllerIncarnation value);
TZM_API std::string to_string(PolicyGeneration value);

// Validates and holds a bounded UTF-8 text identity. Absent (empty) and
// present are distinct; a present value is non-empty, at most max_length
// bytes, valid UTF-8, free of control characters, and free of leading or
// trailing whitespace.
class BoundedText {
 public:
  BoundedText() = default;

  // Returns TextNotValidUtf8, NameTooLong or InvalidArgument for a rejected
  // value. The reason is never silently repaired.
  static Result<BoundedText> create(std::string_view text, std::size_t max_length);

  // Builds a value known to be valid at the call site. Truncation is not
  // performed: an over-long literal yields an empty value so a mistake shows
  // up as a missing identity rather than a wrong one.
  static BoundedText literal(std::string_view text);

  bool empty() const noexcept { return text_.empty(); }
  const std::string& str() const noexcept { return text_; }
  std::size_t size() const noexcept { return text_.size(); }

  friend bool operator==(const BoundedText& a, const BoundedText& b) { return a.text_ == b.text_; }
  friend bool operator!=(const BoundedText& a, const BoundedText& b) { return a.text_ != b.text_; }
  friend bool operator<(const BoundedText& a, const BoundedText& b) { return a.text_ < b.text_; }

 private:
  std::string text_;
};

// The operator or subsystem that requested a mutation.
class ActorId {
 public:
  ActorId() = default;
  static Result<ActorId> create(std::string_view text);
  static ActorId literal(std::string_view text) { return ActorId(BoundedText::literal(text)); }

  bool empty() const noexcept { return text_.empty(); }
  const std::string& str() const noexcept { return text_.str(); }
  friend bool operator==(const ActorId& a, const ActorId& b) { return a.text_ == b.text_; }

 private:
  explicit ActorId(BoundedText text) : text_(std::move(text)) {}
  BoundedText text_;
};

// The external system that produced an observation. Provenance is recorded,
// never inferred.
class SourceId {
 public:
  SourceId() = default;
  static Result<SourceId> create(std::string_view text);
  static SourceId literal(std::string_view text) { return SourceId(BoundedText::literal(text)); }

  bool empty() const noexcept { return text_.empty(); }
  const std::string& str() const noexcept { return text_.str(); }
  friend bool operator==(const SourceId& a, const SourceId& b) { return a.text_ == b.text_; }
  friend bool operator<(const SourceId& a, const SourceId& b) { return a.text_ < b.text_; }

 private:
  explicit SourceId(BoundedText text) : text_(std::move(text)) {}
  BoundedText text_;
};

// Stable operator-facing zone name, unique within a configuration.
class ZoneName {
 public:
  ZoneName() = default;
  static Result<ZoneName> create(std::string_view text);
  static ZoneName literal(std::string_view text) { return ZoneName(BoundedText::literal(text)); }

  bool empty() const noexcept { return text_.empty(); }
  const std::string& str() const noexcept { return text_.str(); }
  friend bool operator==(const ZoneName& a, const ZoneName& b) { return a.text_ == b.text_; }
  friend bool operator<(const ZoneName& a, const ZoneName& b) { return a.text_ < b.text_; }

 private:
  explicit ZoneName(BoundedText text) : text_(std::move(text)) {}
  BoundedText text_;
};

// True when text is well-formed UTF-8 with no control characters.
TZM_API bool is_valid_identifier_text(std::string_view text);

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_IDS_HPP
