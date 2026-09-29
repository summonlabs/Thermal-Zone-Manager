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

#include "thermal_zone_manager/headroom.hpp"

#include <string_view>

namespace thermal_zone_manager {

std::string_view to_string(HeadroomStatus status) {
  switch (status) {
    case HeadroomStatus::Known:
      return "known";
    case HeadroomStatus::Unknown:
      return "unknown";
    case HeadroomStatus::Indeterminate:
      return "indeterminate";
  }
  return "unknown_headroom_status";
}

bool is_proven(HeadroomStatus status) { return status == HeadroomStatus::Known; }

}  // namespace thermal_zone_manager
