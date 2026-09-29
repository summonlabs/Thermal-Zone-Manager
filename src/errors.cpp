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

#include "thermal_zone_manager/errors.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace thermal_zone_manager {
namespace {

struct CodeName {
  ErrorCode code;
  std::string_view name;
};

// The single table that fixes both the identifier of every code and the order
// of the phases. Validation walks the phases in this order and reports the
// first violated rule, so the same invalid request always produces the same
// primary code.
constexpr CodeName kNames[] = {
    {ErrorCode::Ok, "ok"},
    {ErrorCode::StoreUnavailable, "store_unavailable"},
    {ErrorCode::StoreClosed, "store_closed"},
    {ErrorCode::ReadOnlyStore, "read_only_store"},
    {ErrorCode::StoreBusy, "store_busy"},
    {ErrorCode::WriterAuthorityLost, "writer_authority_lost"},
    {ErrorCode::ProcessFailure, "process_failure"},
    {ErrorCode::InvalidArgument, "invalid_argument"},
    {ErrorCode::EmptyConfiguration, "empty_configuration"},
    {ErrorCode::TooManyZones, "too_many_zones"},
    {ErrorCode::TooManyCouplingEdges, "too_many_coupling_edges"},
    {ErrorCode::NameTooLong, "name_too_long"},
    {ErrorCode::TextNotValidUtf8, "text_not_valid_utf8"},
    {ErrorCode::PayloadTooLarge, "payload_too_large"},
    {ErrorCode::MissingEpoch, "missing_epoch"},
    {ErrorCode::EpochMismatch, "epoch_mismatch"},
    {ErrorCode::EpochRegression, "epoch_regression"},
    {ErrorCode::StaleConfigurationGeneration, "stale_configuration_generation"},
    {ErrorCode::FutureConfigurationGeneration, "future_configuration_generation"},
    {ErrorCode::StaleZoneGeneration, "stale_zone_generation"},
    {ErrorCode::FutureZoneGeneration, "future_zone_generation"},
    {ErrorCode::StaleEvidenceGeneration, "stale_evidence_generation"},
    {ErrorCode::FutureEvidenceGeneration, "future_evidence_generation"},
    {ErrorCode::StaleEvaluation, "stale_evaluation"},
    {ErrorCode::StaleStateRevision, "stale_state_revision"},
    {ErrorCode::CrossGenerationAuthor, "cross_generation_author"},
    {ErrorCode::InvalidEnvelope, "invalid_envelope"},
    {ErrorCode::InvalidTemperature, "invalid_temperature"},
    {ErrorCode::InvalidThermalResistance, "invalid_thermal_resistance"},
    {ErrorCode::InvalidCoefficient, "invalid_coefficient"},
    {ErrorCode::CouplingBudgetExceeded, "coupling_budget_exceeded"},
    {ErrorCode::InvalidPolicy, "invalid_policy"},
    {ErrorCode::InvalidDerateLadder, "invalid_derate_ladder"},
    {ErrorCode::InvalidFreshnessWindow, "invalid_freshness_window"},
    {ErrorCode::ContradictoryLimits, "contradictory_limits"},
    {ErrorCode::DuplicateZoneId, "duplicate_zone_id"},
    {ErrorCode::DuplicateZoneName, "duplicate_zone_name"},
    {ErrorCode::UnknownZone, "unknown_zone"},
    {ErrorCode::UnknownCouplingEndpoint, "unknown_coupling_endpoint"},
    {ErrorCode::SelfCoupling, "self_coupling"},
    {ErrorCode::DuplicateCouplingEdge, "duplicate_coupling_edge"},
    {ErrorCode::CouplingNotContractive, "coupling_not_contractive"},
    {ErrorCode::HopBudgetExceeded, "hop_budget_exceeded"},
    {ErrorCode::StaleSequence, "stale_sequence"},
    {ErrorCode::ConflictingObservation, "conflicting_observation"},
    {ErrorCode::DuplicateObservation, "duplicate_observation"},
    {ErrorCode::ObservationForGenerationMismatch, "observation_for_generation_mismatch"},
    {ErrorCode::IdempotencyConflict, "idempotency_conflict"},
    {ErrorCode::UnknownCommand, "unknown_command"},
    {ErrorCode::CommandAlreadyApplied, "command_already_applied"},
    {ErrorCode::ArithmeticOverflow, "arithmetic_overflow"},
    {ErrorCode::DivisionByZero, "division_by_zero"},
    {ErrorCode::StoreCorrupt, "store_corrupt"},
    {ErrorCode::StoreFormatMismatch, "store_format_mismatch"},
    {ErrorCode::StorePathInvalid, "store_path_invalid"},
    {ErrorCode::StoreLocked, "store_locked"},
    {ErrorCode::TruncatedRecord, "truncated_record"},
    {ErrorCode::TrailingBytes, "trailing_bytes"},
    {ErrorCode::UnexpectedEndOfInput, "unexpected_end_of_input"},
    {ErrorCode::IoFailure, "io_failure"},
    {ErrorCode::Unsupported, "unsupported"},
    {ErrorCode::Internal, "internal"},
};

const std::vector<ValidationPhase>& phase_table() {
  static const std::vector<ValidationPhase> table = {
      {1, "store_availability", ErrorCode::StoreUnavailable, ErrorCode::ProcessFailure},
      {2, "structural_validity", ErrorCode::InvalidArgument, ErrorCode::PayloadTooLarge},
      {3, "control_plane_epoch", ErrorCode::MissingEpoch, ErrorCode::EpochRegression},
      {4, "generation_binding", ErrorCode::StaleConfigurationGeneration,
       ErrorCode::CrossGenerationAuthor},
      {5, "numeric_domain", ErrorCode::InvalidEnvelope, ErrorCode::ContradictoryLimits},
      {6, "identity", ErrorCode::DuplicateZoneId, ErrorCode::UnknownZone},
      {7, "coupling_graph", ErrorCode::UnknownCouplingEndpoint, ErrorCode::HopBudgetExceeded},
      {8, "observation_sequence", ErrorCode::StaleSequence,
       ErrorCode::ObservationForGenerationMismatch},
      {9, "command_identity", ErrorCode::IdempotencyConflict, ErrorCode::CommandAlreadyApplied},
      {10, "arithmetic", ErrorCode::ArithmeticOverflow, ErrorCode::DivisionByZero},
      {11, "store_integrity", ErrorCode::StoreCorrupt, ErrorCode::IoFailure},
      {12, "capability", ErrorCode::Unsupported, ErrorCode::Internal},
  };
  return table;
}

}  // namespace

const std::vector<ValidationPhase>& validation_precedence() { return phase_table(); }

std::string_view to_string(ErrorCode code) {
  for (const CodeName& entry : kNames) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return "unknown_error_code";
}

std::optional<std::uint32_t> validation_phase(ErrorCode code) {
  if (code == ErrorCode::Ok) {
    return std::nullopt;
  }
  for (const ValidationPhase& phase : phase_table()) {
    if (static_cast<std::uint32_t>(code) >= static_cast<std::uint32_t>(phase.first) &&
        static_cast<std::uint32_t>(code) <= static_cast<std::uint32_t>(phase.last)) {
      return phase.index;
    }
  }
  return std::nullopt;
}

bool is_transient(ErrorCode code) {
  return code == ErrorCode::StoreBusy || code == ErrorCode::StoreLocked ||
         code == ErrorCode::IoFailure;
}

Error::Error(ErrorCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

Error& Error::with(std::string key, std::string value) {
  context_.emplace_back(std::move(key), std::move(value));
  return *this;
}

Error& Error::with(std::string key, std::int64_t value) {
  context_.emplace_back(std::move(key), std::to_string(value));
  return *this;
}

Error& Error::with(std::string key, std::uint64_t value) {
  context_.emplace_back(std::move(key), std::to_string(value));
  return *this;
}

std::string Error::to_string() const {
  std::string text(thermal_zone_manager::to_string(code_));
  text += ": ";
  text += message_;
  for (const auto& entry : context_) {
    text += "; ";
    text += entry.first;
    text += "=";
    text += entry.second;
  }
  return text;
}

}  // namespace thermal_zone_manager
