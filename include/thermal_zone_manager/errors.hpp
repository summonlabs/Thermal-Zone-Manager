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

#ifndef THERMAL_ZONE_MANAGER_ERRORS_HPP
#define THERMAL_ZONE_MANAGER_ERRORS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "thermal_zone_manager/export.hpp"

namespace thermal_zone_manager {

// Machine-readable outcome codes. These values are part of the public
// contract: they are stable across releases and are what a caller branches on.
// Free-form text is never the contract.
//
// The numeric values are grouped by validation phase. Validation follows a
// fixed precedence: the phases are evaluated in the documented order below and
// the first violated rule decides the primary code. Within one phase the order
// is the order of the enumerators. See validation_precedence() for the
// authoritative machine-readable list.
enum class ErrorCode : std::uint32_t {
  // No error. Never stored inside an Error instance.
  Ok = 0,

  // -- phase 1: store and process availability -----------------------------
  StoreUnavailable = 100,
  StoreClosed = 101,
  ReadOnlyStore = 102,
  StoreBusy = 103,
  WriterAuthorityLost = 104,
  ProcessFailure = 105,

  // -- phase 2: structural argument validity and resource bounds -----------
  InvalidArgument = 200,
  EmptyConfiguration = 201,
  TooManyZones = 202,
  TooManyCouplingEdges = 203,
  NameTooLong = 204,
  TextNotValidUtf8 = 205,
  PayloadTooLarge = 206,

  // -- phase 3: control-plane epoch ----------------------------------------
  MissingEpoch = 300,
  EpochMismatch = 301,
  EpochRegression = 302,

  // -- phase 4: generations and authority binding --------------------------
  StaleConfigurationGeneration = 400,
  FutureConfigurationGeneration = 401,
  StaleZoneGeneration = 402,
  FutureZoneGeneration = 403,
  StaleEvidenceGeneration = 404,
  FutureEvidenceGeneration = 405,
  StaleEvaluation = 406,
  StaleStateRevision = 407,
  CrossGenerationAuthor = 408,

  // -- phase 5: numeric domain and declared limits -------------------------
  InvalidEnvelope = 500,
  InvalidTemperature = 501,
  InvalidThermalResistance = 502,
  InvalidCoefficient = 503,
  CouplingBudgetExceeded = 504,
  InvalidPolicy = 505,
  InvalidDerateLadder = 506,
  InvalidFreshnessWindow = 507,
  ContradictoryLimits = 508,

  // -- phase 6: identity ---------------------------------------------------
  DuplicateZoneId = 600,
  DuplicateZoneName = 601,
  UnknownZone = 602,

  // -- phase 7: coupling graph ---------------------------------------------
  UnknownCouplingEndpoint = 700,
  SelfCoupling = 701,
  DuplicateCouplingEdge = 702,
  CouplingNotContractive = 703,
  HopBudgetExceeded = 704,

  // -- phase 8: observation sequencing --------------------------------------
  StaleSequence = 800,
  ConflictingObservation = 801,
  DuplicateObservation = 802,
  ObservationForGenerationMismatch = 803,

  // -- phase 9: idempotency and command identity ---------------------------
  IdempotencyConflict = 900,
  UnknownCommand = 901,
  CommandAlreadyApplied = 902,

  // -- phase 10: arithmetic -------------------------------------------------
  ArithmeticOverflow = 1000,
  DivisionByZero = 1001,

  // -- phase 11: durable store integrity and I/O ---------------------------
  StoreCorrupt = 1100,
  StoreFormatMismatch = 1101,
  StorePathInvalid = 1102,
  StoreLocked = 1103,
  TruncatedRecord = 1104,
  TrailingBytes = 1105,
  UnexpectedEndOfInput = 1106,
  IoFailure = 1107,

  // -- phase 12: capabilities and internal invariants -----------------------
  Unsupported = 1200,
  Internal = 1201,
};

// One machine-readable validation phase, in precedence order.
struct ValidationPhase {
  std::uint32_t index;
  std::string_view name;
  ErrorCode first;
  ErrorCode last;
};

// The authoritative, ordered validation precedence. A request that violates
// rules in several phases reports the code of the earliest phase listed here.
TZM_API const std::vector<ValidationPhase>& validation_precedence();

// Stable identifier of a code, e.g. "stale_configuration_generation".
TZM_API std::string_view to_string(ErrorCode code);

// The index of the validation phase a code belongs to, or nullopt for Ok.
TZM_API std::optional<std::uint32_t> validation_phase(ErrorCode code);

// True when the code describes a condition a caller may retry unchanged after
// the transient condition clears (for example StoreBusy). A refusal caused by
// stale or missing authority is never transient.
TZM_API bool is_transient(ErrorCode code);

// A refusal. Carries the primary machine-readable code, a human-readable
// explanation, and ordered key/value context that never replaces the code.
class Error {
 public:
  Error() = default;
  Error(ErrorCode code, std::string message);

  Error& with(std::string key, std::string value);
  Error& with(std::string key, std::int64_t value);
  Error& with(std::string key, std::uint64_t value);

  ErrorCode code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }
  const std::vector<std::pair<std::string, std::string>>& context() const noexcept {
    return context_;
  }

  // "stale_configuration_generation: ...; expected=3; actual=4"
  std::string to_string() const;

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string message_;
  std::vector<std::pair<std::string, std::string>> context_;
};

// Outcome of an operation that yields no value.
class ResultVoid;

// Outcome of an operation that yields a T. A Result is one of a value or an
// Error, never both and never neither.
//
// value() and error() are lvalue-qualified on purpose. Reading a value out of
// a temporary Result would produce a reference into an object that is about to
// die, so that form does not compile: hold the Result in a named variable
// first. ok() and value_or() are safe on a temporary and are not qualified.
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  Result(const Result&) = default;
  Result(Result&&) noexcept = default;
  Result& operator=(const Result&) = default;
  Result& operator=(Result&&) noexcept = default;
  ~Result() = default;

  bool ok() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }

  T& value() & noexcept { return *value_; }
  const T& value() const& noexcept { return *value_; }

  Error& error() & noexcept { return error_; }
  const Error& error() const& noexcept { return error_; }

  // The value when present, otherwise the supplied fallback. Never invents a
  // value from an error.
  T value_or(T fallback) const { return value_.has_value() ? *value_ : std::move(fallback); }

 private:
  std::optional<T> value_;
  Error error_;
};

// Outcome of an operation that yields nothing on success.
class ResultVoid {
 public:
  ResultVoid() = default;
  ResultVoid(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  bool ok() const noexcept { return error_.code() == ErrorCode::Ok; }
  explicit operator bool() const noexcept { return ok(); }

  Error& error() & noexcept { return error_; }
  const Error& error() const& noexcept { return error_; }

 private:
  Error error_;
};

using Status = ResultVoid;

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_ERRORS_HPP
