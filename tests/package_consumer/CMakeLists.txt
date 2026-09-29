# Copyright 2026 Summon Software Labs
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# An out-of-tree consumer of the installed package. It knows nothing about the
# source tree: everything it needs must come from find_package().

cmake_minimum_required(VERSION 3.20)

project(TzmPackageConsumer CXX)

find_package(ThermalZoneManager 1.0 REQUIRED)

add_executable(tzm_package_consumer consumer.cpp)

set_target_properties(
  tzm_package_consumer
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(tzm_package_consumer PRIVATE ThermalZoneManager::thermal_zone_manager)
