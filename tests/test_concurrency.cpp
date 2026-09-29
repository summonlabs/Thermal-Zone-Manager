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


#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "fixtures.hpp"
#include "test_harness.hpp"
#include "thermal_zone_manager/thermal_zone_manager.hpp"

using namespace thermal_zone_manager;
using tzm_test::Facility;
using tzm_test::ZoneSpec;

namespace {

std::vector<ZoneSpec> grid(std::size_t count) {
  std::vector<ZoneSpec> zones;
  for (std::size_t index = 0; index < count; ++index) {
    ZoneSpec spec;
    spec.id = static_cast<std::uint32_t>(index + 1);
    spec.name = "zone-" + std::to_string(spec.id);
    zones.push_back(spec);
  }
  return zones;
}

}  // namespace

TZM_TEST(concurrency, readers_and_a_writer_do_not_interfere) {
  tzm_test::TempDir directory("concurrency-read-write");
  Result<std::unique_ptr<tzm_test::Facility>> facility =
      tzm_test::Facility::open(directory.path(), grid(24), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  ThermalZoneEngine& engine = facility.value()->engine();
  tzm_test::ManualClock& clock = facility.value()->clock();

  std::atomic<bool> stop{false};
  std::atomic<int> read_failures{0};
  std::atomic<long long> reads{0};

  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&engine, &stop, &read_failures, &reads]() {
      while (!stop.load(std::memory_order_relaxed)) {
        const Configuration configuration = engine.configuration();
        if (configuration.zone_count() == 0) {
          read_failures.fetch_add(1);
        }
        const FencingState fencing = engine.fencing();
        (void)fencing;
        const std::string text = engine.describe();
        if (text.empty()) {
          read_failures.fetch_add(1);
        }
        reads.fetch_add(1);
      }
    });
  }

  const Configuration configuration = engine.configuration();
  std::atomic<int> write_failures{0};
  for (std::uint64_t sequence = 1; sequence <= 400; ++sequence) {
    const std::uint32_t zone = static_cast<std::uint32_t>((sequence % 24) + 1);
    const ZoneConfiguration* declaration = configuration.find(ZoneId::from_value(zone));
    if (declaration == nullptr) {
      write_failures.fetch_add(1);
      break;
    }
    TemperatureObservation observation;
    observation.zone = ZoneId::from_value(zone);
    observation.zone_generation = declaration->generation;
    observation.evidence_generation = configuration.evidence_generation();
    observation.sequence = ObservationSequence::from_value(sequence);
    observation.temperature = MilliCelsius{60000 + static_cast<std::int64_t>(sequence % 1000)};
    observation.source = SourceId::literal("concurrency-harness");
    observation.provenance = ProvenanceKind::SyntheticHarness;
    observation.observed_at = clock.now();
    observation.validity = Nanoseconds{60000000000LL};
    observation.publisher_epoch = engine.fencing().epoch;
    observation.publisher_incarnation = engine.incarnation();
    Result<ObservationReceipt> ingested = engine.ingest_observation(observation);
    if (!ingested.ok()) {
      write_failures.fetch_add(1);
      break;
    }
  }
  stop.store(true);
  for (std::thread& thread : readers) {
    thread.join();
  }
  TZM_CHECK_EQ(write_failures.load(), 0);
  TZM_CHECK_EQ(read_failures.load(), 0);
  TZM_CHECK(reads.load() > 0);
  TZM_CHECK_EQ(engine.configuration().zone_count(), static_cast<std::size_t>(24));
}

TZM_TEST(concurrency, concurrent_evaluations_agree) {
  tzm_test::TempDir directory("concurrency-evaluate");
  Result<std::unique_ptr<tzm_test::Facility>> facility =
      tzm_test::Facility::open(directory.path(), grid(8), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  for (std::uint32_t zone = 1; zone <= 8; ++zone) {
    TZM_REQUIRE_OK(harness.observe(zone, 62000));
  }

  std::atomic<int> mismatches{0};
  std::vector<std::thread> workers;
  for (int index = 0; index < 4; ++index) {
    workers.emplace_back([&harness, &mismatches]() {
      for (int attempt = 0; attempt < 20; ++attempt) {
        EvaluationRequest request;
        request.id = EvaluationId::from_value(
            static_cast<std::uint64_t>(1000 + attempt));
        request.epoch = harness.engine().fencing().epoch;
        request.configuration_generation = harness.configuration().generation();
        request.evidence_generation = harness.configuration().evidence_generation();
        Result<EvaluationResult> result = harness.engine().evaluate(request);
        if (!result.ok() || result.value().zones().size() != 8) {
          mismatches.fetch_add(1);
          continue;
        }
        for (const ZoneThermalState& zone : result.value().zones()) {
          if (zone.headroom.status != HeadroomStatus::Known) {
            mismatches.fetch_add(1);
          }
        }
      }
    });
  }
  for (std::thread& thread : workers) {
    thread.join();
  }
  TZM_CHECK_EQ(mismatches.load(), 0);
}

TZM_TEST(concurrency, closing_while_readers_run_is_safe) {
  tzm_test::TempDir directory("concurrency-close");
  Result<std::unique_ptr<tzm_test::Facility>> facility =
      tzm_test::Facility::open(directory.path(), grid(32), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  ThermalZoneEngine& engine = harness.engine();
  for (std::uint32_t zone = 1; zone <= 32; ++zone) {
    TZM_REQUIRE_OK(harness.observe(zone, 60000));
  }

  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&engine, &stop, &failures]() {
      while (!stop.load(std::memory_order_relaxed)) {
        const Configuration configuration = engine.configuration();
        if (configuration.zone_count() == 0) {
          failures.fetch_add(1);
        }
        const std::uint64_t revision = engine.fencing().revision.raw();
        (void)revision;
      }
    });
  }
  // Closing while readers are active must not race with them: close takes the
  // same exclusive lock the readers take in shared mode.
  TZM_CHECK_OK(engine.close());
  stop.store(true);
  for (std::thread& thread : readers) {
    thread.join();
  }
  TZM_CHECK_EQ(failures.load(), 0);
  TZM_CHECK(engine.closed());
  TZM_CHECK_ERR(harness.observe(1, 61000), ErrorCode::StoreClosed);
}
TZM_TEST(concurrency, mutation_racing_close_never_dereferences_a_released_store) {
  tzm_test::TempDir directory("concurrency-close-race");
  Result<std::unique_ptr<tzm_test::Facility>> facility =
      tzm_test::Facility::open(directory.path(), grid(4), {}, 1, 3);
  TZM_REQUIRE_OK(facility);
  Facility& harness = *facility.value();
  ThermalZoneEngine& engine = harness.engine();
  TZM_REQUIRE_OK(harness.observe(1, 60000));

  std::atomic<bool> stop{false};
  std::atomic<int> unexpected{0};
  std::atomic<long long> accepted{0};
  std::atomic<long long> refused{0};
  std::vector<std::thread> writers;
  for (int index = 0; index < 3; ++index) {
    writers.emplace_back([&engine, &stop, &unexpected, &accepted, &refused]() {
      std::uint64_t sequence = 500000000;
      while (!stop.load(std::memory_order_relaxed)) {
        ++sequence;
        TemperatureObservation observation;
        observation.zone = ZoneId::from_value(1);
        observation.zone_generation = ZoneGeneration::first();
        observation.evidence_generation = engine.fencing().evidence_generation;
        observation.sequence = ObservationSequence::from_value(sequence);
        observation.temperature = MilliCelsius{60000};
        observation.source = SourceId::literal("race-harness");
        observation.provenance = ProvenanceKind::SyntheticHarness;
        observation.observed_at = engine.clock().now();
        observation.validity = Nanoseconds{60000000000LL};
        observation.publisher_epoch = engine.fencing().epoch;
        observation.publisher_incarnation = engine.incarnation();
        Result<ObservationReceipt> receipt = engine.ingest_observation(observation);
        if (receipt.ok()) {
          accepted.fetch_add(1);
        } else if (receipt.error().code() == ErrorCode::StoreClosed ||
                   receipt.error().code() == ErrorCode::ReadOnlyStore ||
                   receipt.error().code() == ErrorCode::StaleZoneGeneration ||
                   receipt.error().code() == ErrorCode::StaleEvidenceGeneration ||
                   receipt.error().code() == ErrorCode::EpochMismatch) {
          refused.fetch_add(1);
        } else {
          unexpected.fetch_add(1);
        }
      }
    });
  }
  // Close while writers are mid-flight. Every outcome must be a success or a
  // named refusal; a released store handle must never be dereferenced.
  TZM_CHECK_OK(engine.close());
  stop.store(true);
  for (std::thread& thread : writers) {
    thread.join();
  }
  TZM_CHECK_EQ(unexpected.load(), 0);
  TZM_CHECK(accepted.load() >= 0);
  TZM_CHECK(refused.load() >= 0);
  TZM_CHECK(engine.closed());
}

TZM_TEST(concurrency, concurrent_read_only_engines_over_one_store) {
  tzm_test::TempDir directory("concurrency-readonly");
  {
    Result<std::unique_ptr<tzm_test::Facility>> facility =
        tzm_test::Facility::open(directory.path(), grid(4), {}, 1, 3);
    TZM_REQUIRE_OK(facility);
    TZM_REQUIRE_OK(facility.value()->observe(1, 60000));
  }
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  for (int index = 0; index < 4; ++index) {
    workers.emplace_back([&directory, &failures]() {
      EngineOptions options;
      options.root = directory.path();
      options.access = StoreAccess::ReadOnly;
      options.create_if_missing = false;
      Result<std::unique_ptr<ThermalZoneEngine>> engine = ThermalZoneEngine::open(options);
      if (!engine.ok() || engine.value()->configuration().zone_count() != 4) {
        failures.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : workers) {
    thread.join();
  }
  TZM_CHECK_EQ(failures.load(), 0);
}
