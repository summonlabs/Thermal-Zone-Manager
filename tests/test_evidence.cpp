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


#include <cstdint>
#include <optional>
#include <string>

#include "test_harness.hpp"
#include "thermal_zone_manager/thermal_zone_manager.hpp"

using namespace thermal_zone_manager;

namespace {

constexpr std::int64_t kSecond = 1000000000LL;

TemperatureObservation make_observation() {
  TemperatureObservation observation;
  observation.zone = ZoneId::from_value(1);
  observation.zone_generation = ZoneGeneration::first();
  observation.evidence_generation = EvidenceGeneration::first();
  observation.sequence = ObservationSequence::first();
  observation.temperature = MilliCelsius{70000};
  observation.source = SourceId::literal("bms-1");
  observation.provenance = ProvenanceKind::ExternalSensor;
  observation.observed_at = Timestamp{1000 * kSecond};
  observation.validity = Nanoseconds{60 * kSecond};
  observation.publisher_epoch = ControlPlaneEpoch::first();
  observation.publisher_incarnation = ControllerIncarnation::from_value(9);
  return observation;
}

ZoneEvidence held(const TemperatureObservation& observation, bool recovered) {
  ZoneEvidence evidence;
  evidence.present = true;
  evidence.observation = observation;
  evidence.recovered = recovered;
  return evidence;
}

}  // namespace

TZM_TEST(evidence, a_complete_observation_validates) {
  TZM_CHECK_OK(make_observation().validate());
}

TZM_TEST(evidence, every_required_field_is_enforced) {
  TemperatureObservation observation = make_observation();
  observation.zone = ZoneId::zero();
  TZM_CHECK_ERR(observation.validate(), ErrorCode::UnknownZone);

  observation = make_observation();
  observation.sequence = ObservationSequence::zero();
  TZM_CHECK_ERR(observation.validate(), ErrorCode::StaleSequence);

  observation = make_observation();
  observation.temperature = MilliCelsius{kMaxPlausibleMilliCelsius + 1};
  TZM_CHECK_ERR(observation.validate(), ErrorCode::InvalidTemperature);

  observation = make_observation();
  observation.heat = MilliWatts{-1};
  TZM_CHECK_ERR(observation.validate(), ErrorCode::InvalidArgument);

  observation = make_observation();
  observation.validity = Nanoseconds{0};
  TZM_CHECK_ERR(observation.validate(), ErrorCode::InvalidFreshnessWindow);

  observation = make_observation();
  observation.source = SourceId();
  TZM_CHECK_ERR(observation.validate(), ErrorCode::InvalidArgument);

  observation = make_observation();
  observation.publisher_epoch = ControlPlaneEpoch::zero();
  TZM_CHECK_ERR(observation.validate(), ErrorCode::MissingEpoch);

  observation = make_observation();
  observation.publisher_incarnation = ControllerIncarnation::zero();
  TZM_CHECK_ERR(observation.validate(), ErrorCode::InvalidArgument);

  observation = make_observation();
  observation.observed_at = Timestamp{0};
  TZM_CHECK_ERR(observation.validate(), ErrorCode::InvalidArgument);
}

TZM_TEST(evidence, freshness_decision_order_is_total) {
  const TemperatureObservation observation = make_observation();
  const Timestamp now{1000 * kSecond + 10 * kSecond};
  const EvidenceGeneration current = EvidenceGeneration::first();

  ZoneEvidence missing;
  TZM_CHECK(classify_freshness(missing, now, current, Nanoseconds{0}) ==
            EvidenceFreshness::Missing);

  // A superseded evidence generation is stale even though the timestamp is
  // inside the validity window and the value was not recovered.
  ZoneEvidence superseded = held(observation, false);
  superseded.observation.evidence_generation = EvidenceGeneration::from_value(2);
  TZM_CHECK(classify_freshness(superseded, now, current, Nanoseconds{0}) ==
            EvidenceFreshness::Stale);

  // Recovered dominates the timestamp: a value read back from disk is never
  // fresh even when its window has not expired.
  ZoneEvidence recovered = held(observation, true);
  TZM_CHECK(classify_freshness(recovered, now, current, Nanoseconds{0}) ==
            EvidenceFreshness::Recovered);

  ZoneEvidence future = held(observation, false);
  future.observation.observed_at = Timestamp{now.value + 2 * kSecond};
  TZM_CHECK(classify_freshness(future, now, current, Nanoseconds{0}) ==
            EvidenceFreshness::Future);
  TZM_CHECK(classify_freshness(future, now, current, Nanoseconds{kSecond}) ==
            EvidenceFreshness::Future);
  // Inside the declared skew tolerance the observation is treated as taken now
  // and stays fresh for as long as its validity window allows.
  TZM_CHECK(classify_freshness(future, now, current, Nanoseconds{3 * kSecond}) ==
            EvidenceFreshness::Fresh);

  ZoneEvidence expired = held(observation, false);
  expired.observation.observed_at = Timestamp{now.value - 60 * kSecond - 1};
  TZM_CHECK(classify_freshness(expired, now, current, Nanoseconds{0}) ==
            EvidenceFreshness::Stale);

  ZoneEvidence boundary = held(observation, false);
  boundary.observation.observed_at = Timestamp{now.value - 60 * kSecond};
  TZM_CHECK(classify_freshness(boundary, now, current, Nanoseconds{0}) ==
            EvidenceFreshness::Fresh);

  ZoneEvidence fresh = held(observation, false);
  TZM_CHECK(classify_freshness(fresh, now, current, Nanoseconds{0}) ==
            EvidenceFreshness::Fresh);
}

TZM_TEST(evidence, only_fresh_is_usable) {
  TZM_CHECK(is_usable(EvidenceFreshness::Fresh));
  TZM_CHECK(!is_usable(EvidenceFreshness::Stale));
  TZM_CHECK(!is_usable(EvidenceFreshness::Future));
  TZM_CHECK(!is_usable(EvidenceFreshness::Recovered));
  TZM_CHECK(!is_usable(EvidenceFreshness::Missing));
  TZM_CHECK(!is_usable(EvidenceFreshness::Unsupported));
  TZM_CHECK_EQ(std::string(to_string(EvidenceFreshness::Recovered)), std::string("recovered"));
  TZM_CHECK_EQ(std::string(to_string(ProvenanceKind::SyntheticHarness)),
               std::string("synthetic_harness"));
}
