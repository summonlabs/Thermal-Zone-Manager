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

#include "thermal_zone_manager/limits.hpp"

namespace thermal_zone_manager {

bool is_within_zone_limit(std::size_t count) { return count <= kMaxZones; }

bool is_within_edge_limit(std::size_t count) { return count <= kMaxCouplingEdges; }

}  // namespace thermal_zone_manager
