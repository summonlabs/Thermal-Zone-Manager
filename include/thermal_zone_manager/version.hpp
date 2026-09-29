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

#ifndef THERMAL_ZONE_MANAGER_VERSION_HPP
#define THERMAL_ZONE_MANAGER_VERSION_HPP

#include <cstdint>
#include <string>

#include "thermal_zone_manager/export.hpp"

namespace thermal_zone_manager {

// Semantic version of the runtime.
inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

// Version of the durable store format. This is deliberately independent of the
// library version: the format only changes when the on-disk layout changes, and
// a reader refuses any other value rather than guessing.
inline constexpr std::uint16_t kStoreFormatVersion = 1;

// Returns "1.0.0".
TZM_API std::string version_string();

// Returns a multi-line description of the build: library version, store format
// version, compiler identification and the pointer width of the build.
TZM_API std::string build_information();

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_VERSION_HPP
