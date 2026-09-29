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

#ifndef THERMAL_ZONE_MANAGER_SRC_SERIALIZE_HPP
#define THERMAL_ZONE_MANAGER_SRC_SERIALIZE_HPP

#include <cstdint>

#include "thermal_zone_manager/canonical.hpp"
#include "thermal_zone_manager/errors.hpp"
#include "thermal_zone_manager/evaluation.hpp"
#include "world.hpp"

namespace thermal_zone_manager {
namespace detail {

void encode_configuration(ByteWriter& writer, const Configuration& configuration);
Result<Configuration> decode_configuration(ByteReader& reader);
std::uint64_t configuration_digest(const Configuration& configuration);

void encode_world(ByteWriter& writer, const WorldState& world);
Result<WorldState> decode_world(ByteReader& reader);

void encode_evaluation(ByteWriter& writer, const EvaluationResult& result);
std::uint64_t evaluation_digest(const EvaluationResult& result);

}  // namespace detail
}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_SRC_SERIALIZE_HPP
