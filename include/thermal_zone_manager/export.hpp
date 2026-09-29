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

#ifndef THERMAL_ZONE_MANAGER_EXPORT_HPP
#define THERMAL_ZONE_MANAGER_EXPORT_HPP

// Shared-library visibility decoration. A static build defines neither symbol
// and the macro expands to nothing, so the same headers serve both.
#if defined(_WIN32) || defined(_WIN64)
#if defined(TZM_SHARED_BUILD)
#define TZM_API __declspec(dllexport)
#elif defined(TZM_SHARED_USE)
#define TZM_API __declspec(dllimport)
#else
#define TZM_API
#endif
#else
#if defined(TZM_SHARED_BUILD)
#define TZM_API __attribute__((visibility("default")))
#else
#define TZM_API
#endif
#endif

#endif  // THERMAL_ZONE_MANAGER_EXPORT_HPP
