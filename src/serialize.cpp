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

#include "serialize.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "thermal_zone_manager/digest.hpp"
#include "thermal_zone_manager/limits.hpp"
#include "thermal_zone_manager/version.hpp"

namespace thermal_zone_manager {
namespace detail {
namespace {

Error reject_enum(const char* field, std::uint64_t value) {
  return Error(ErrorCode::StoreCorrupt, "a stored enumeration holds an impossible value")
      .with("field", field)
      .with("value", value);
}

Result<std::uint8_t> decode_u8_enum(ByteReader& reader, const char* field, std::uint8_t limit) {
  Result<std::uint8_t> raw = reader.u8();
  if (!raw.ok()) {
    return raw.error();
  }
  if (raw.value() >= limit) {
    return reject_enum(field, raw.value());
  }
  return raw.value();
}

void encode_u64_pair(ByteWriter& writer, std::uint64_t first, std::uint64_t second) {
  writer.put_u64(first);
  writer.put_u64(second);
}

void encode_envelope(ByteWriter& writer, const TemperatureEnvelope& envelope) {
  writer.put_i64(envelope.floor_temp.value);
  writer.put_i64(envelope.derate_onset.value);
  writer.put_i64(envelope.ceiling_temp.value);
  writer.put_i64(envelope.critical_temp.value);
}

Result<TemperatureEnvelope> decode_envelope(ByteReader& reader) {
  Result<std::int64_t> floor_temp = reader.i64();
  if (!floor_temp.ok()) {
    return floor_temp.error();
  }
  Result<std::int64_t> onset = reader.i64();
  if (!onset.ok()) {
    return onset.error();
  }
  Result<std::int64_t> ceiling = reader.i64();
  if (!ceiling.ok()) {
    return ceiling.error();
  }
  Result<std::int64_t> critical = reader.i64();
  if (!critical.ok()) {
    return critical.error();
  }
  TemperatureEnvelope envelope;
  envelope.floor_temp = MilliCelsius{floor_temp.value()};
  envelope.derate_onset = MilliCelsius{onset.value()};
  envelope.ceiling_temp = MilliCelsius{ceiling.value()};
  envelope.critical_temp = MilliCelsius{critical.value()};
  return envelope;
}

void encode_observation(ByteWriter& writer, const TemperatureObservation& observation) {
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
}

Result<TemperatureObservation> decode_observation(ByteReader& reader) {
  TemperatureObservation observation;

  Result<std::uint32_t> zone = reader.u32();
  if (!zone.ok()) {
    return zone.error();
  }
  observation.zone = ZoneId::from_value(zone.value());

  Result<std::uint64_t> zone_generation = reader.u64();
  if (!zone_generation.ok()) {
    return zone_generation.error();
  }
  observation.zone_generation = ZoneGeneration::from_value(zone_generation.value());

  Result<std::uint64_t> evidence_generation = reader.u64();
  if (!evidence_generation.ok()) {
    return evidence_generation.error();
  }
  observation.evidence_generation = EvidenceGeneration::from_value(evidence_generation.value());

  Result<std::uint64_t> sequence = reader.u64();
  if (!sequence.ok()) {
    return sequence.error();
  }
  observation.sequence = ObservationSequence::from_value(sequence.value());

  Result<std::int64_t> temperature = reader.i64();
  if (!temperature.ok()) {
    return temperature.error();
  }
  observation.temperature = MilliCelsius{temperature.value()};

  Result<bool> heat_present = reader.optional_marker();
  if (!heat_present.ok()) {
    return heat_present.error();
  }
  if (heat_present.value()) {
    Result<std::int64_t> heat = reader.i64();
    if (!heat.ok()) {
      return heat.error();
    }
    observation.heat = MilliWatts{heat.value()};
  }

  Result<std::string> source = reader.text(kMaxSourceLength);
  if (!source.ok()) {
    return source.error();
  }
  Result<SourceId> source_id = SourceId::create(source.value());
  if (!source_id.ok()) {
    return source_id.error();
  }
  observation.source = source_id.value();

  Result<std::uint8_t> provenance = decode_u8_enum(reader, "provenance", 5);
  if (!provenance.ok()) {
    return provenance.error();
  }
  observation.provenance = static_cast<ProvenanceKind>(provenance.value());

  Result<std::int64_t> observed_at = reader.i64();
  if (!observed_at.ok()) {
    return observed_at.error();
  }
  observation.observed_at = Timestamp{observed_at.value()};

  Result<std::int64_t> validity = reader.i64();
  if (!validity.ok()) {
    return validity.error();
  }
  observation.validity = Nanoseconds{validity.value()};

  Result<std::uint64_t> epoch = reader.u64();
  if (!epoch.ok()) {
    return epoch.error();
  }
  observation.publisher_epoch = ControlPlaneEpoch::from_value(epoch.value());

  Result<std::uint64_t> incarnation = reader.u64();
  if (!incarnation.ok()) {
    return incarnation.error();
  }
  observation.publisher_incarnation = ControllerIncarnation::from_value(incarnation.value());

  const Status valid = observation.validate();
  if (!valid.ok()) {
    return valid.error();
  }
  return observation;
}

void encode_derating(ByteWriter& writer, const DeratingState& derating) {
  writer.put_u32(derating.published_level);
  writer.put_u32(derating.published_steps);
  writer.put_u32(derating.hold_count);
}

Result<DeratingState> decode_derating(ByteReader& reader) {
  DeratingState derating;
  Result<std::uint32_t> level = reader.u32();
  if (!level.ok()) {
    return level.error();
  }
  Result<std::uint32_t> steps = reader.u32();
  if (!steps.ok()) {
    return steps.error();
  }
  Result<std::uint32_t> hold = reader.u32();
  if (!hold.ok()) {
    return hold.error();
  }
  if (level.value() > kMaxDerateSteps || steps.value() > kMaxDerateSteps ||
      hold.value() > kMaxReleaseHoldObservations) {
    return Error(ErrorCode::StoreCorrupt, "a stored derating state is outside its bounds")
        .with("published_level", static_cast<std::uint64_t>(level.value()))
        .with("published_steps", static_cast<std::uint64_t>(steps.value()))
        .with("hold_count", static_cast<std::uint64_t>(hold.value()));
  }
  derating.published_level = level.value();
  derating.published_steps = steps.value();
  derating.hold_count = hold.value();
  return derating;
}

}  // namespace

void encode_configuration(ByteWriter& writer, const Configuration& configuration) {
  writer.put_u64(configuration.generation().raw());
  writer.put_u64(configuration.evidence_generation().raw());
  writer.put_u64(configuration.policy().generation.raw());
  writer.put_u32(configuration.policy().coupling_hops);
  writer.put_u32(configuration.policy().release_hold_observations);
  writer.put_i64(configuration.policy().future_tolerance.value);

  writer.put_u32(static_cast<std::uint32_t>(configuration.zones().size()));
  for (const ZoneConfiguration& zone : configuration.zones()) {
    writer.put_u32(static_cast<std::uint32_t>(zone.id.raw()));
    writer.put_text(zone.name.str());
    writer.put_u64(zone.generation.raw());
    encode_envelope(writer, zone.envelope);
    writer.put_optional_i64(zone.declared_heat.has_value(),
                            zone.declared_heat.has_value() ? zone.declared_heat->value : 0);
    writer.put_i64(zone.thermal_resistance.value);
    writer.put_u32(zone.derate_steps);
    writer.put_i64(zone.recovery_margin.value);
  }

  writer.put_u32(static_cast<std::uint32_t>(configuration.coupling().edge_count()));
  for (const CouplingEdge& edge : configuration.coupling().edges()) {
    writer.put_u32(static_cast<std::uint32_t>(edge.source.raw()));
    writer.put_u32(static_cast<std::uint32_t>(edge.sink.raw()));
    writer.put_u32(static_cast<std::uint32_t>(edge.coefficient.value));
  }
}

Result<Configuration> decode_configuration(ByteReader& reader) {
  Result<std::uint64_t> generation = reader.u64();
  if (!generation.ok()) {
    return generation.error();
  }
  Result<std::uint64_t> evidence_generation = reader.u64();
  if (!evidence_generation.ok()) {
    return evidence_generation.error();
  }
  ThermalPolicy policy;
  Result<std::uint64_t> policy_generation = reader.u64();
  if (!policy_generation.ok()) {
    return policy_generation.error();
  }
  policy.generation = PolicyGeneration::from_value(policy_generation.value());
  Result<std::uint32_t> hops = reader.u32();
  if (!hops.ok()) {
    return hops.error();
  }
  policy.coupling_hops = hops.value();
  Result<std::uint32_t> hold = reader.u32();
  if (!hold.ok()) {
    return hold.error();
  }
  policy.release_hold_observations = hold.value();
  Result<std::int64_t> tolerance = reader.i64();
  if (!tolerance.ok()) {
    return tolerance.error();
  }
  policy.future_tolerance = Nanoseconds{tolerance.value()};

  Result<std::uint32_t> zone_count = reader.u32();
  if (!zone_count.ok()) {
    return zone_count.error();
  }
  if (static_cast<std::size_t>(zone_count.value()) > kMaxZones) {
    return Error(ErrorCode::PayloadTooLarge, "a stored zone count exceeds its ceiling")
        .with("zones", static_cast<std::uint64_t>(zone_count.value()))
        .with("limit", static_cast<std::uint64_t>(kMaxZones));
  }

  Configuration::ZoneList zones;
  zones.reserve(zone_count.value());
  for (std::uint32_t index = 0; index < zone_count.value(); ++index) {
    ZoneConfiguration zone;
    Result<std::uint32_t> id = reader.u32();
    if (!id.ok()) {
      return id.error();
    }
    zone.id = ZoneId::from_value(id.value());
    Result<std::string> name = reader.text(kMaxZoneNameLength);
    if (!name.ok()) {
      return name.error();
    }
    Result<ZoneName> zone_name = ZoneName::create(name.value());
    if (!zone_name.ok()) {
      return zone_name.error();
    }
    zone.name = zone_name.value();
    Result<std::uint64_t> zone_generation = reader.u64();
    if (!zone_generation.ok()) {
      return zone_generation.error();
    }
    zone.generation = ZoneGeneration::from_value(zone_generation.value());
    Result<TemperatureEnvelope> envelope = decode_envelope(reader);
    if (!envelope.ok()) {
      return envelope.error();
    }
    zone.envelope = envelope.value();
    Result<bool> heat_present = reader.optional_marker();
    if (!heat_present.ok()) {
      return heat_present.error();
    }
    if (heat_present.value()) {
      Result<std::int64_t> heat = reader.i64();
      if (!heat.ok()) {
        return heat.error();
      }
      zone.declared_heat = MilliWatts{heat.value()};
    }
    Result<std::int64_t> resistance = reader.i64();
    if (!resistance.ok()) {
      return resistance.error();
    }
    zone.thermal_resistance = MicroKelvinPerWatt{resistance.value()};
    Result<std::uint32_t> steps = reader.u32();
    if (!steps.ok()) {
      return steps.error();
    }
    zone.derate_steps = steps.value();
    Result<std::int64_t> margin = reader.i64();
    if (!margin.ok()) {
      return margin.error();
    }
    zone.recovery_margin = MilliCelsius{margin.value()};
    zones.push_back(zone);
  }

  Result<std::uint32_t> edge_count = reader.u32();
  if (!edge_count.ok()) {
    return edge_count.error();
  }
  if (static_cast<std::size_t>(edge_count.value()) > kMaxCouplingEdges) {
    return Error(ErrorCode::PayloadTooLarge, "a stored coupling edge count exceeds its ceiling")
        .with("edges", static_cast<std::uint64_t>(edge_count.value()))
        .with("limit", static_cast<std::uint64_t>(kMaxCouplingEdges));
  }
  std::vector<CouplingEdge> edges;
  edges.reserve(edge_count.value());
  for (std::uint32_t index = 0; index < edge_count.value(); ++index) {
    CouplingEdge edge;
    Result<std::uint32_t> source = reader.u32();
    if (!source.ok()) {
      return source.error();
    }
    Result<std::uint32_t> sink = reader.u32();
    if (!sink.ok()) {
      return sink.error();
    }
    Result<std::uint32_t> coefficient = reader.u32();
    if (!coefficient.ok()) {
      return coefficient.error();
    }
    edge.source = ZoneId::from_value(source.value());
    edge.sink = ZoneId::from_value(sink.value());
    edge.coefficient = PartsPerMillion{static_cast<std::int32_t>(coefficient.value())};
    edges.push_back(edge);
  }

  // Generation zero with no zones and no edges is the empty world, which is
  // what a store holds before anything has been declared. It is not a
  // configuration with no zones, which create() refuses.
  if (zones.empty() && edges.empty() && generation.value() == 0 &&
      evidence_generation.value() == 0) {
    return Configuration();
  }

  std::vector<ZoneId> ids;
  ids.reserve(zones.size());
  for (const ZoneConfiguration& zone : zones) {
    ids.push_back(zone.id);
  }
  Result<CouplingGraph> coupling = CouplingGraph::create(std::move(edges), ids);
  if (!coupling.ok()) {
    return coupling.error();
  }
  return Configuration::create(std::move(zones), std::move(coupling.value()), policy,
                               ConfigurationGeneration::from_value(generation.value()),
                               EvidenceGeneration::from_value(evidence_generation.value()));
}

std::uint64_t configuration_digest(const Configuration& configuration) {
  ByteWriter writer;
  encode_configuration(writer, configuration);
  if (writer.overflowed()) {
    return 0;
  }
  return fnv1a64(writer.bytes());
}

void encode_world(ByteWriter& writer, const WorldState& world) {
  encode_u64_pair(writer, world.fencing.epoch.raw(), world.fencing.incarnation.raw());
  encode_u64_pair(writer, world.fencing.revision.raw(), world.fencing.commit.raw());
  encode_u64_pair(writer, world.fencing.configuration_generation.raw(),
                  world.fencing.evidence_generation.raw());
  writer.put_u64(world.fencing.policy_generation.raw());

  encode_configuration(writer, world.configuration);

  writer.put_u32(static_cast<std::uint32_t>(world.zones.size()));
  for (const PersistedZoneState& zone : world.zones) {
    writer.put_u32(static_cast<std::uint32_t>(zone.zone.raw()));
    writer.put_u64(zone.generation.raw());
    writer.put_u8(zone.has_observation ? 1U : 0U);
    if (zone.has_observation) {
      encode_observation(writer, zone.observation);
    }
    writer.put_u8(zone.recovered ? 1U : 0U);
    encode_derating(writer, zone.derating);
  }

  writer.put_u32(static_cast<std::uint32_t>(world.receipts.size()));
  for (const IdempotencyRecord& receipt : world.receipts) {
    writer.put_u64(receipt.command.raw());
    writer.put_u8(static_cast<std::uint8_t>(receipt.kind));
    writer.put_u64(receipt.fingerprint);
    writer.put_u64(receipt.result_value);
    writer.put_u64(receipt.result_aux);
    writer.put_u64(receipt.result_extra);
    writer.put_u64(receipt.applied_revision.raw());
    writer.put_u64(receipt.applied_commit.raw());
  }
}

Result<WorldState> decode_world(ByteReader& reader) {
  WorldState world;

  Result<std::uint64_t> epoch = reader.u64();
  if (!epoch.ok()) {
    return epoch.error();
  }
  Result<std::uint64_t> incarnation = reader.u64();
  if (!incarnation.ok()) {
    return incarnation.error();
  }
  Result<std::uint64_t> revision = reader.u64();
  if (!revision.ok()) {
    return revision.error();
  }
  Result<std::uint64_t> commit = reader.u64();
  if (!commit.ok()) {
    return commit.error();
  }
  Result<std::uint64_t> config_generation = reader.u64();
  if (!config_generation.ok()) {
    return config_generation.error();
  }
  Result<std::uint64_t> evidence_generation = reader.u64();
  if (!evidence_generation.ok()) {
    return evidence_generation.error();
  }
  Result<std::uint64_t> policy_generation = reader.u64();
  if (!policy_generation.ok()) {
    return policy_generation.error();
  }
  world.fencing.epoch = ControlPlaneEpoch::from_value(epoch.value());
  world.fencing.incarnation = ControllerIncarnation::from_value(incarnation.value());
  world.fencing.revision = StoreRevision::from_value(revision.value());
  world.fencing.commit = CommitSequence::from_value(commit.value());
  world.fencing.configuration_generation = ConfigurationGeneration::from_value(config_generation.value());
  world.fencing.evidence_generation = EvidenceGeneration::from_value(evidence_generation.value());
  world.fencing.policy_generation = PolicyGeneration::from_value(policy_generation.value());

  Result<Configuration> configuration = decode_configuration(reader);
  if (!configuration.ok()) {
    return configuration.error();
  }
  world.configuration = std::move(configuration.value());

  Result<std::uint32_t> zone_count = reader.u32();
  if (!zone_count.ok()) {
    return zone_count.error();
  }
  if (static_cast<std::size_t>(zone_count.value()) > kMaxZones) {
    return Error(ErrorCode::PayloadTooLarge, "a stored zone state count exceeds its ceiling")
        .with("zones", static_cast<std::uint64_t>(zone_count.value()))
        .with("limit", static_cast<std::uint64_t>(kMaxZones));
  }
  world.zones.reserve(zone_count.value());
  for (std::uint32_t index = 0; index < zone_count.value(); ++index) {
    PersistedZoneState zone;
    Result<std::uint32_t> id = reader.u32();
    if (!id.ok()) {
      return id.error();
    }
    zone.zone = ZoneId::from_value(id.value());
    Result<std::uint64_t> zone_generation = reader.u64();
    if (!zone_generation.ok()) {
      return zone_generation.error();
    }
    zone.generation = ZoneGeneration::from_value(zone_generation.value());
    Result<bool> has_observation = reader.optional_marker();
    if (!has_observation.ok()) {
      return has_observation.error();
    }
    zone.has_observation = has_observation.value();
    if (zone.has_observation) {
      Result<TemperatureObservation> observation = decode_observation(reader);
      if (!observation.ok()) {
        return observation.error();
      }
      zone.observation = observation.value();
    }
    Result<bool> recovered = reader.optional_marker();
    if (!recovered.ok()) {
      return recovered.error();
    }
    zone.recovered = recovered.value();
    Result<DeratingState> derating = decode_derating(reader);
    if (!derating.ok()) {
      return derating.error();
    }
    zone.derating = derating.value();
    world.zones.push_back(zone);
  }
  for (std::size_t index = 1; index < world.zones.size(); ++index) {
    if (!(world.zones[index - 1].zone < world.zones[index].zone)) {
      return Error(ErrorCode::StoreCorrupt,
                   "the stored zone states are not in ascending handle order")
          .with("index", static_cast<std::uint64_t>(index));
    }
  }

  Result<std::uint32_t> receipt_count = reader.u32();
  if (!receipt_count.ok()) {
    return receipt_count.error();
  }
  if (static_cast<std::size_t>(receipt_count.value()) > kMaxRetainedReceipts) {
    return Error(ErrorCode::PayloadTooLarge, "a stored receipt count exceeds its ceiling")
        .with("receipts", static_cast<std::uint64_t>(receipt_count.value()))
        .with("limit", static_cast<std::uint64_t>(kMaxRetainedReceipts));
  }
  world.receipts.reserve(receipt_count.value());
  for (std::uint32_t index = 0; index < receipt_count.value(); ++index) {
    IdempotencyRecord receipt;
    Result<std::uint64_t> command = reader.u64();
    if (!command.ok()) {
      return command.error();
    }
    receipt.command = CommandId::from_value(command.value());
    Result<std::uint8_t> kind = decode_u8_enum(reader, "command_kind", 4);
    if (!kind.ok()) {
      return kind.error();
    }
    receipt.kind = static_cast<CommandKind>(kind.value());
    Result<std::uint64_t> fingerprint = reader.u64();
    if (!fingerprint.ok()) {
      return fingerprint.error();
    }
    receipt.fingerprint = fingerprint.value();
    Result<std::uint64_t> result_value = reader.u64();
    if (!result_value.ok()) {
      return result_value.error();
    }
    receipt.result_value = result_value.value();
    Result<std::uint64_t> result_aux = reader.u64();
    if (!result_aux.ok()) {
      return result_aux.error();
    }
    receipt.result_aux = result_aux.value();
    Result<std::uint64_t> result_extra = reader.u64();
    if (!result_extra.ok()) {
      return result_extra.error();
    }
    receipt.result_extra = result_extra.value();
    Result<std::uint64_t> applied_revision = reader.u64();
    if (!applied_revision.ok()) {
      return applied_revision.error();
    }
    receipt.applied_revision = StoreRevision::from_value(applied_revision.value());
    Result<std::uint64_t> applied_commit = reader.u64();
    if (!applied_commit.ok()) {
      return applied_commit.error();
    }
    receipt.applied_commit = CommitSequence::from_value(applied_commit.value());
    world.receipts.push_back(receipt);
  }
  for (std::size_t index = 1; index < world.receipts.size(); ++index) {
    if (!(world.receipts[index - 1].command < world.receipts[index].command)) {
      return Error(ErrorCode::StoreCorrupt,
                   "the stored command receipts are not in ascending order")
          .with("index", static_cast<std::uint64_t>(index));
    }
  }

  return world;
}

void encode_evaluation(ByteWriter& writer, const EvaluationResult& result) {
  writer.put_u64(result.id().raw());
  writer.put_i64(result.evaluated_at().value);
  writer.put_u64(result.epoch().raw());
  writer.put_u64(result.configuration_generation().raw());
  writer.put_u64(result.evidence_generation().raw());
  writer.put_u64(result.policy_generation().raw());
  writer.put_u64(result.basis_revision().raw());

  writer.put_u32(static_cast<std::uint32_t>(result.zones().size()));
  for (const ZoneThermalState& zone : result.zones()) {
    writer.put_u32(static_cast<std::uint32_t>(zone.zone.raw()));
    writer.put_text(zone.name.str());
    writer.put_u64(zone.zone_generation.raw());
    writer.put_u8(zone.observation_present ? 1U : 0U);
    writer.put_u8(static_cast<std::uint8_t>(zone.evidence_freshness));
    writer.put_u8(zone.observed_temperature_known ? 1U : 0U);
    writer.put_i64(zone.observed_temperature.value);
    writer.put_i64(zone.observed_at.value);
    writer.put_text(zone.source.str());
    writer.put_u8(static_cast<std::uint8_t>(zone.provenance));
    writer.put_u8(zone.heat_known ? 1U : 0U);
    writer.put_u8(static_cast<std::uint8_t>(zone.heat_source));
    writer.put_i64(zone.heat.value);
    writer.put_u8(zone.coupled_heat_known ? 1U : 0U);
    writer.put_i64(zone.coupled_heat.value);
    writer.put_i64(zone.coupled_rise.value);
    writer.put_u32(zone.coupling_hops_used);
    writer.put_u64(static_cast<std::uint64_t>(zone.unknown_neighbour_count));
    writer.put_u8(zone.coupling_refinement_refused ? 1U : 0U);
    writer.put_u8(zone.effective_temperature_known ? 1U : 0U);
    writer.put_i64(zone.effective_temperature.value);
    writer.put_u8(zone.band_known ? 1U : 0U);
    writer.put_u8(static_cast<std::uint8_t>(zone.band));
    writer.put_u8(static_cast<std::uint8_t>(zone.headroom.status));
    writer.put_i64(zone.headroom.temperature_margin.value);
    writer.put_i64(zone.headroom.power_margin.value);
    writer.put_u32(zone.instant_derate_level);
    encode_derating(writer, zone.derating_before);
    encode_derating(writer, zone.derating_after);
    writer.put_u32(static_cast<std::uint32_t>(zone.derate_fraction.value));
    writer.put_u8(zone.derating_escalated ? 1U : 0U);
    writer.put_u8(zone.derating_released ? 1U : 0U);
    writer.put_u8(zone.derating_held_for_evidence ? 1U : 0U);
    writer.put_u8(zone.allowance_known ? 1U : 0U);
    writer.put_i64(zone.placement_allowance.value);
    writer.put_u8(static_cast<std::uint8_t>(zone.constraint_kind));
    writer.put_u32(static_cast<std::uint32_t>(zone.reasons.size()));
    for (const ConstraintReason reason : zone.reasons) {
      writer.put_u8(static_cast<std::uint8_t>(reason));
    }
  }

  writer.put_u32(static_cast<std::uint32_t>(result.constraints().size()));
  for (const PlacementConstraint& constraint : result.constraints()) {
    writer.put_u32(static_cast<std::uint32_t>(constraint.zone.raw()));
    writer.put_u64(constraint.zone_generation.raw());
    writer.put_u64(constraint.configuration_generation.raw());
    writer.put_u64(constraint.evidence_generation.raw());
    writer.put_u64(constraint.policy_generation.raw());
    writer.put_u64(constraint.evaluation.raw());
    writer.put_u64(constraint.basis_revision.raw());
    writer.put_u8(static_cast<std::uint8_t>(constraint.kind));
    writer.put_u8(constraint.allowance_known ? 1U : 0U);
    writer.put_i64(constraint.max_additional_heat.value);
    writer.put_u8(static_cast<std::uint8_t>(constraint.band));
    writer.put_u8(constraint.band_known ? 1U : 0U);
    writer.put_u32(static_cast<std::uint32_t>(constraint.derate_fraction.value));
    writer.put_u32(static_cast<std::uint32_t>(constraint.reasons.size()));
    for (const ConstraintReason reason : constraint.reasons) {
      writer.put_u8(static_cast<std::uint8_t>(reason));
    }
  }

  const EvaluationSummary& summary = result.summary();
  writer.put_u64(summary.zones_total);
  writer.put_u64(summary.zones_known);
  writer.put_u64(summary.zones_unknown);
  writer.put_u64(summary.zones_indeterminate);
  writer.put_u64(summary.constraints_bounded);
  writer.put_u64(summary.constraints_prohibited);
  writer.put_u64(summary.constraints_indeterminate);
  writer.put_u64(summary.zones_with_usable_evidence);
  writer.put_u64(summary.zones_with_unknown_neighbour);
}

std::uint64_t evaluation_digest(const EvaluationResult& result) {
  ByteWriter writer;
  encode_evaluation(writer, result);
  if (writer.overflowed()) {
    return 0;
  }
  return fnv1a64(writer.bytes());
}

const PersistedZoneState* WorldState::find(ZoneId zone) const {
  for (const PersistedZoneState& entry : zones) {
    if (entry.zone == zone) {
      return &entry;
    }
  }
  return nullptr;
}

PersistedZoneState* WorldState::find(ZoneId zone) {
  for (PersistedZoneState& entry : zones) {
    if (entry.zone == zone) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace detail
}  // namespace thermal_zone_manager
