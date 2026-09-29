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

#ifndef THERMAL_ZONE_MANAGER_DIGEST_HPP
#define THERMAL_ZONE_MANAGER_DIGEST_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "thermal_zone_manager/export.hpp"

namespace thermal_zone_manager {

// FNV-1a, 64 bit. Used as a cheap deterministic identity digest over canonical
// bytes and as the idempotency fingerprint. It is an identity function, not a
// security primitive: it detects accidental divergence, not a deliberate
// forgery.
inline constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

TZM_API std::uint64_t fnv1a64(std::span<const std::byte> data,
                              std::uint64_t seed = kFnvOffsetBasis) noexcept;

// CRC-32C (Castagnoli, reflected, polynomial 0x1EDC6F41, initial value all
// ones, final inversion). Used as the integrity check of every durable record.
TZM_API std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t seed = 0) noexcept;

// Lower-case hexadecimal, zero padded to the natural width of the input.
TZM_API std::string to_hex(std::uint32_t value);
TZM_API std::string to_hex(std::uint64_t value);

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_DIGEST_HPP
