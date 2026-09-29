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

#include "thermal_zone_manager/evidence.hpp"

#include <string_view>

#include "thermal_zone_manager/limits.hpp"

namespace thermal_zone_manager {

std::string_view to_string(ProvenanceKind kind) {
  switch (kind) {
    case ProvenanceKind::ExternalSensor:
      return "external_sensor";
    case ProvenanceKind::FacilityModel:
      return "facility_model";
    case ProvenanceKind::OperatorDeclaration:
      return "operator_declaration";
    case ProvenanceKind::SyntheticHarness:
      return "synthetic_harness";
    case ProvenanceKind::RecoveredStore:
      return "recovered_store";
  }
  return "unknown_provenance";
}

std::string_view to_string(EvidenceFreshness freshness) {
  switch (freshness) {
    case EvidenceFreshness::Fresh:
      return "fresh";
    case EvidenceFreshness::Stale:
      return "stale";
    case EvidenceFreshness::Future:
      return "future";
    case EvidenceFreshness::Recovered:
      return "recovered";
    case EvidenceFreshness::Missing:
      return "missing";
    case EvidenceFreshness::Unsupported:
      return "unsupported";
  }
  return "unknown_freshness";
}

bool is_usable(EvidenceFreshness freshness) { return freshness == EvidenceFreshness::Fresh; }

Status TemperatureObservation::validate() const {
  if (zone.is_zero()) {
    return Error(ErrorCode::UnknownZone, "an observation must name a zone");
  }
  if (sequence.is_zero()) {
    return Error(ErrorCode::StaleSequence,
                 "an observation sequence starts at one and may never be zero");
  }
  if (!is_plausible(temperature)) {
    return Error(ErrorCode::InvalidTemperature,
                 "an observed temperature is not physically plausible")
        .with("millicelsius", temperature.value);
  }
  if (heat.has_value()) {
    if (heat->value < 0 || heat->value > kMaxPlausibleMilliWatts) {
      return Error(ErrorCode::InvalidArgument, "an observed heat load is outside its range")
          .with("milliwatts", heat->value)
          .with("max", kMaxPlausibleMilliWatts);
    }
  }
  if (validity.value < kMinFreshnessWindowNs || validity.value > kMaxFreshnessWindowNs) {
    return Error(ErrorCode::InvalidFreshnessWindow,
                 "an observation validity window is outside its supported range")
        .with("validity_ns", validity.value)
        .with("min", kMinFreshnessWindowNs)
        .with("max", kMaxFreshnessWindowNs);
  }
  if (source.empty()) {
    return Error(ErrorCode::InvalidArgument, "an observation must name its source");
  }
  if (publisher_epoch.is_zero()) {
    return Error(ErrorCode::MissingEpoch, "an observation must carry its publisher epoch");
  }
  if (publisher_incarnation.is_zero()) {
    return Error(ErrorCode::InvalidArgument,
                 "an observation must carry its publisher incarnation");
  }
  if (observed_at.value <= 0) {
    return Error(ErrorCode::InvalidArgument,
                 "an observation timestamp must be a positive Unix epoch instant")
        .with("observed_at_ns", observed_at.value);
  }
  return Status();
}

EvidenceFreshness classify_freshness(const ZoneEvidence& evidence, Timestamp now,
                                     EvidenceGeneration current_generation,
                                     Nanoseconds future_tolerance) {
  if (!evidence.present) {
    return EvidenceFreshness::Missing;
  }
  if (evidence.observation.evidence_generation != current_generation) {
    return EvidenceFreshness::Stale;
  }
  if (evidence.recovered) {
    return EvidenceFreshness::Recovered;
  }
  const std::optional<std::int64_t> latest =
      add_checked(now.value, future_tolerance.value > 0 ? future_tolerance.value : 0);
  if (!latest.has_value()) {
    return EvidenceFreshness::Future;
  }
  if (evidence.observation.observed_at.value > latest.value()) {
    return EvidenceFreshness::Future;
  }
  const std::optional<std::int64_t> age =
      sub_checked(now.value, evidence.observation.observed_at.value);
  if (!age.has_value()) {
    return EvidenceFreshness::Future;
  }
  // An observation inside the declared skew tolerance is treated as taken now,
  // so it is fresh for exactly as long as its validity window allows.
  const std::int64_t elapsed = age.value() < 0 ? 0 : age.value();
  if (elapsed > evidence.observation.validity.value) {
    return EvidenceFreshness::Stale;
  }
  return EvidenceFreshness::Fresh;
}

}  // namespace thermal_zone_manager
