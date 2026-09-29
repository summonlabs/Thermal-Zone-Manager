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

#ifndef THERMAL_ZONE_MANAGER_LIMITS_HPP
#define THERMAL_ZONE_MANAGER_LIMITS_HPP

#include <cstddef>
#include <cstdint>

#include "thermal_zone_manager/export.hpp"

// Fixed resource bounds. Every collection this runtime accepts from a caller is
// bounded here, and every declared length is checked against these bounds
// before anything is allocated. Nothing scales with input without a ceiling.

namespace thermal_zone_manager {

// Declared configuration.
inline constexpr std::size_t kMaxZones = 4096;
inline constexpr std::size_t kMaxCouplingEdges = 65536;

// Coupling budget, in parts per million. The total outgoing coupling of one
// zone may not exceed unity: a zone cannot hand more than all of its heat to
// its neighbours. The incoming budget is larger because several neighbours may
// each contribute a meaningful share, but it is still bounded so that no
// coefficient set can amplify a heat load without limit.
inline constexpr std::int64_t kMaxOutgoingCouplingPpm = 1000000;
inline constexpr std::int64_t kMaxIncomingCouplingPpm = 4000000;

// Derating ladder resolution.
inline constexpr std::uint32_t kMinDerateSteps = 1;
inline constexpr std::uint32_t kMaxDerateSteps = 64;

// Bounded coupling propagation. The hop count is fixed before evaluation
// begins, so propagation always terminates in a bounded number of passes.
inline constexpr std::uint32_t kMinCouplingHops = 1;
inline constexpr std::uint32_t kMaxCouplingHops = 8;

// Hysteresis.
inline constexpr std::uint32_t kMinReleaseHoldObservations = 1;
inline constexpr std::uint32_t kMaxReleaseHoldObservations = 64;

// Freshness windows, in nanoseconds.
inline constexpr std::int64_t kMinFreshnessWindowNs = 1;
inline constexpr std::int64_t kMaxFreshnessWindowNs = 86400000000000LL;  // 24 hours

// Evaluation batch size for a single request. Bounded even though it may not
// exceed the number of declared zones.
inline constexpr std::size_t kMaxZonesPerEvaluation = kMaxZones;

// Retained idempotency receipts. The oldest receipt is evicted first, so a
// long-lived store cannot grow without limit.
inline constexpr std::size_t kMaxRetainedReceipts = 4096;

// Durable store. A slot holds a fixed header followed by the canonical payload
// of one generation. The capacity is fixed when the store is created so that
// the two slots always have known offsets.
inline constexpr std::size_t kStoreHeaderBytes = 64;
inline constexpr std::size_t kStoreSlotHeaderBytes = 96;
inline constexpr std::size_t kMinStoreSlotPayload = 1u * 1024u * 1024u;      // 1 MiB
inline constexpr std::size_t kDefaultStoreSlotPayload = 8u * 1024u * 1024u;  // 8 MiB
inline constexpr std::size_t kMaxStoreSlotBytes = 64u * 1024u * 1024u;       // 64 MiB
inline constexpr std::size_t kMaxStoreFileBytes =
    kStoreHeaderBytes + 2 * (kStoreSlotHeaderBytes + kMaxStoreSlotBytes);

// Bounded text fields.
inline constexpr std::size_t kMaxActorLength = 128;
inline constexpr std::size_t kMaxSourceLength = 128;
inline constexpr std::size_t kMaxZoneNameLength = 128;
inline constexpr std::size_t kMaxNotesLength = 256;

// Canonical encoding ceilings, checked before any length is used to resize.
inline constexpr std::size_t kMaxCanonicalBytes = kMaxStoreSlotBytes;

// Rejects a declared count before it is used to allocate.
TZM_API bool is_within_zone_limit(std::size_t count);
TZM_API bool is_within_edge_limit(std::size_t count);

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_LIMITS_HPP
