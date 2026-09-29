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

#ifndef THERMAL_ZONE_MANAGER_EVIDENCE_HPP
#define THERMAL_ZONE_MANAGER_EVIDENCE_HPP

#include <cstdint>
#include <optional>
#include <string_view>

#include "thermal_zone_manager/errors.hpp"
#include "thermal_zone_manager/export.hpp"
#include "thermal_zone_manager/ids.hpp"
#include "thermal_zone_manager/units.hpp"

namespace thermal_zone_manager {

// Where an observation came from. Provenance is recorded as supplied; this
// runtime never infers it and never upgrades it.
//
// ExternalSensor and FacilityModel describe real plant evidence that this
// runtime does not own. SyntheticHarness describes evidence produced by a test
// or example harness. RecoveredStore marks a value that was read back from the
// durable store and has not been revalidated by a current source.
enum class ProvenanceKind : std::uint8_t {
  ExternalSensor = 0,
  FacilityModel = 1,
  OperatorDeclaration = 2,
  SyntheticHarness = 3,
  RecoveredStore = 4,
};

TZM_API std::string_view to_string(ProvenanceKind kind);

// Why a zone's temperature evidence can or cannot be used. Fresh is the only
// state that can produce a known headroom. Missing and Recovered are distinct:
// "no sensor ever reported" is not the same as "a value was restored from
// disk".
enum class EvidenceFreshness : std::uint8_t {
  Fresh = 0,
  Stale = 1,
  Future = 2,
  Recovered = 3,
  Missing = 4,
  Unsupported = 5,
};

TZM_API std::string_view to_string(EvidenceFreshness freshness);

// True only for Fresh. Every other state must be treated as "not proven".
TZM_API bool is_usable(EvidenceFreshness freshness);

// One temperature observation supplied by an external source.
//
// The observation is stamped with the zone, configuration and evidence
// generations it was produced against, the control-plane epoch of its
// publisher, and the publisher's process incarnation. All four are checked at
// ingest; a mismatch is a refusal, not a downgrade.
struct TemperatureObservation {
  ZoneId zone;
  ZoneGeneration zone_generation;
  EvidenceGeneration evidence_generation;
  ObservationSequence sequence;
  MilliCelsius temperature;
  // Optional concurrent heat load attributable to the zone. When absent the
  // zone's declared heat load is used for coupling and the absence is recorded.
  std::optional<MilliWatts> heat;
  SourceId source;
  ProvenanceKind provenance = ProvenanceKind::ExternalSensor;
  Timestamp observed_at;
  // How long this observation remains usable after observed_at.
  Nanoseconds validity;
  ControlPlaneEpoch publisher_epoch;
  ControllerIncarnation publisher_incarnation;

  Status validate() const;
};

// The observation currently held for one zone, together with the freshness
// verdict of the last evaluation.
struct ZoneEvidence {
  bool present = false;
  TemperatureObservation observation;
  EvidenceFreshness freshness = EvidenceFreshness::Missing;
  // True while the held observation came from the durable store and has not
  // been replaced by a strictly newer observation from a live source.
  bool recovered = false;
};

// Freshness of a held observation at a given instant. The decision order is
// fixed and total:
//
//   1. no observation at all                      -> Missing
//   2. evidence generation is not the current one -> Stale (superseded)
//   3. the value came back from the durable store -> Recovered
//   4. observed_at is later than now + tolerance  -> Future
//   5. observed_at is older than the validity     -> Stale (expired)
//   6. otherwise                                  -> Fresh
//
// Recovered is checked before the timestamp, so state read back from disk is
// never fresh physical evidence even when its timestamp would still be inside
// its validity window.
TZM_API EvidenceFreshness classify_freshness(const ZoneEvidence& evidence, Timestamp now,
                                             EvidenceGeneration current_generation,
                                             Nanoseconds future_tolerance);

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_EVIDENCE_HPP
