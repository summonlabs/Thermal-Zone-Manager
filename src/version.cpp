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

#include "thermal_zone_manager/version.hpp"

#include <string>

namespace thermal_zone_manager {

std::string version_string() {
  return std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) + "." +
         std::to_string(kVersionPatch);
}

namespace {

std::string compiler_identification() {
#if defined(_MSC_VER)
  return "msvc " + std::to_string(_MSC_VER);
#elif defined(__clang__)
  return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("gcc ") + __VERSION__;
#else
  return "unknown";
#endif
}

}  // namespace

std::string build_information() {
  std::string text;
  text += "thermal_zone_manager " + version_string() + "\n";
  text += "store format version: " + std::to_string(kStoreFormatVersion) + "\n";
  text += "compiler: " + compiler_identification() + "\n";
  text += "c++ standard: " + std::to_string(__cplusplus) + "\n";
  text += std::string("pointer width: ") +
          std::to_string(static_cast<unsigned>(sizeof(void*) * 8U)) + " bits\n";
  return text;
}

}  // namespace thermal_zone_manager
