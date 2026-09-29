# Thermal Zone Manager - build, test, install and packaging rules.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Everything here is target based: no directory-global flags are set, so the
# library, the tool, the examples, the benchmark and the tests each carry
# exactly the options they need and nothing this project uses for itself can
# leak into the installed package.

cmake_minimum_required(VERSION 3.20)

project(ThermalZoneManager
        VERSION 1.0.0
        DESCRIPTION "Generation-bound thermal zone state and headroom for the Data Center Control Plane"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(BUILD_SHARED_LIBS "Build thermal_zone_manager as a shared library instead of a static one" OFF)

option(TZM_BUILD_TESTS "Build the Thermal Zone Manager test suite" ON)
option(TZM_BUILD_TOOLS "Build the tzm administration and inspection tool" ON)
option(TZM_BUILD_EXAMPLES "Build the Thermal Zone Manager example programs" ON)
option(TZM_BUILD_BENCHMARKS "Build the Thermal Zone Manager benchmark" ON)
option(TZM_WARNINGS_AS_ERRORS "Treat every compiler warning as an error" ON)
option(TZM_SANITIZERS "Build with AddressSanitizer, and UndefinedBehaviorSanitizer on GCC and Clang" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckCXXCompilerFlag)
include(CTest)

set(TZM_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/ThermalZoneManager"
    CACHE STRING "Install destination of the Thermal Zone Manager CMake package files")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function(tzm_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    if(TZM_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/RTC1>)
    target_compile_options(${target} PRIVATE
      $<$<CONFIG:Release>:/O2>
      $<$<CONFIG:Release>:/Gy>
      $<$<CONFIG:Release>:/Gw>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(TZM_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(TZM_SANITIZERS)
    target_compile_options(${target} PRIVATE ${TZM_SANITIZER_COMPILE_OPTIONS})
    target_link_options(${target} PRIVATE ${TZM_SANITIZER_LINK_OPTIONS})
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Sanitizers
# ---------------------------------------------------------------------------

if(TZM_SANITIZERS)
  if(MSVC)
    check_cxx_compiler_flag("/fsanitize=address" TZM_COMPILER_HAS_ASAN)
    if(NOT TZM_COMPILER_HAS_ASAN)
      message(FATAL_ERROR
        "TZM_SANITIZERS=ON cannot be used with ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}: a program built with /fsanitize=address does not "
        "compile and link here. Either the toolset predates AddressSanitizer support "
        "(Visual Studio 2019 16.9) or the optional C++ AddressSanitizer component is not "
        "installed. Reconfigure with -DTZM_SANITIZERS=OFF.")
    endif()
    set(TZM_SANITIZER_COMPILE_OPTIONS /fsanitize=address /Zi)
    set(TZM_SANITIZER_LINK_OPTIONS /fsanitize=address)
    message(STATUS "ThermalZoneManager: AddressSanitizer enabled (MSVC)")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    check_cxx_compiler_flag("-fsanitize=address,undefined" TZM_COMPILER_HAS_ASAN_UBSAN)
    if(NOT TZM_COMPILER_HAS_ASAN_UBSAN)
      message(FATAL_ERROR
        "TZM_SANITIZERS=ON cannot be used with ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}: the sanitizer runtime is unavailable. "
        "Reconfigure with -DTZM_SANITIZERS=OFF.")
    endif()
    set(TZM_SANITIZER_COMPILE_OPTIONS -fsanitize=address,undefined -fno-omit-frame-pointer)
    set(TZM_SANITIZER_LINK_OPTIONS -fsanitize=address,undefined)
    message(STATUS "ThermalZoneManager: AddressSanitizer and UndefinedBehaviorSanitizer enabled")
  else()
    message(FATAL_ERROR
      "TZM_SANITIZERS=ON is supported with MSVC (/fsanitize=address) and with GCC or Clang "
      "(-fsanitize=address,undefined); the configured compiler is "
      "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}. "
      "Reconfigure with -DTZM_SANITIZERS=OFF.")
  endif()
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

set(TZM_PUBLIC_HEADERS
  include/thermal_zone_manager/canonical.hpp
  include/thermal_zone_manager/clock.hpp
  include/thermal_zone_manager/configuration.hpp
  include/thermal_zone_manager/constraints.hpp
  include/thermal_zone_manager/coupling.hpp
  include/thermal_zone_manager/derating.hpp
  include/thermal_zone_manager/digest.hpp
  include/thermal_zone_manager/engine.hpp
  include/thermal_zone_manager/envelope.hpp
  include/thermal_zone_manager/errors.hpp
  include/thermal_zone_manager/evaluation.hpp
  include/thermal_zone_manager/evidence.hpp
  include/thermal_zone_manager/export.hpp
  include/thermal_zone_manager/headroom.hpp
  include/thermal_zone_manager/ids.hpp
  include/thermal_zone_manager/limits.hpp
  include/thermal_zone_manager/policy.hpp
  include/thermal_zone_manager/store.hpp
  include/thermal_zone_manager/thermal_zone_manager.hpp
  include/thermal_zone_manager/units.hpp
  include/thermal_zone_manager/version.hpp
  include/thermal_zone_manager/zone.hpp)

set(TZM_SOURCES
  src/canonical.cpp
  src/clock.cpp
  src/configuration.cpp
  src/constraints.cpp
  src/coupling.cpp
  src/derating.cpp
  src/digest.cpp
  src/engine.cpp
  src/envelope.cpp
  src/errors.cpp
  src/evaluation.cpp
  src/evidence.cpp
  src/headroom.cpp
  src/ids.cpp
  src/limits.cpp
  src/platform.cpp
  src/policy.cpp
  src/serialize.cpp
  src/store.cpp
  src/units.cpp
  src/version.cpp
  src/zone.cpp)

add_library(thermal_zone_manager ${TZM_SOURCES})
add_library(ThermalZoneManager::thermal_zone_manager ALIAS thermal_zone_manager)

target_include_directories(thermal_zone_manager
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(thermal_zone_manager PUBLIC cxx_std_20)
set_target_properties(thermal_zone_manager PROPERTIES
  OUTPUT_NAME thermal_zone_manager
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

if(BUILD_SHARED_LIBS)
  target_compile_definitions(thermal_zone_manager
    PUBLIC TZM_SHARED_USE
    PRIVATE TZM_SHARED_BUILD)
endif()

tzm_configure_target(thermal_zone_manager)

# ---------------------------------------------------------------------------
# Tool, examples and benchmark
# ---------------------------------------------------------------------------

function(tzm_add_console_target target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE ThermalZoneManager::thermal_zone_manager)
  tzm_configure_target(${target})
endfunction()

if(TZM_BUILD_TOOLS)
  tzm_add_console_target(tzm tools/tzm_cli.cpp)
endif()

set(TZM_EXAMPLE_SOURCES
  examples/01_zone_state.cpp
  examples/02_coupling_and_constraints.cpp
  examples/03_unknown_is_not_zero.cpp
  examples/04_derating_hysteresis.cpp
  examples/05_generation_fencing.cpp
  examples/06_persistence_and_recovery.cpp
  examples/07_idempotent_retry.cpp
  examples/08_facility_wide_survey.cpp)

if(TZM_BUILD_EXAMPLES)
  foreach(source IN LISTS TZM_EXAMPLE_SOURCES)
    string(SUBSTRING "${source}" 9 2 TZM_EXAMPLE_NUMBER)
    tzm_add_console_target(tzm_example_${TZM_EXAMPLE_NUMBER} ${source})
  endforeach()
endif()

if(TZM_BUILD_BENCHMARKS)
  tzm_add_console_target(tzm_benchmark benchmarks/benchmark_evaluation.cpp)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

set(TZM_TEST_SOURCES
  tests/test_adversarial.cpp
  tests/test_canonical.cpp
  tests/test_concurrency.cpp
  tests/test_corruption.cpp
  tests/test_coupling.cpp
  tests/test_crash.cpp
  tests/test_derating.cpp
  tests/test_determinism.cpp
  tests/test_envelope.cpp
  tests/test_errors.cpp
  tests/test_evidence.cpp
  tests/test_fencing.cpp
  tests/test_headroom.cpp
  tests/test_persistence.cpp
  tests/test_paths.cpp
  tests/test_property.cpp
  tests/test_restart.cpp
  tests/test_units.cpp
  tests/test_writer_lock.cpp)

if(TZM_BUILD_TESTS)
  add_executable(tzm_tests
    ${TZM_TEST_SOURCES}
    tests/support/test_harness.cpp
    tests/support/test_main.cpp
    tests/support/test_process.cpp
    tests/support/reference_model.cpp)
  target_link_libraries(tzm_tests PRIVATE ThermalZoneManager::thermal_zone_manager)
  target_include_directories(tzm_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  tzm_configure_target(tzm_tests)

  # The test binary re-executes itself to obtain real independent operating
  # system processes for the writer-lock, restart and process-death tests.
  add_test(NAME tzm.tests COMMAND tzm_tests)

  if(TZM_BUILD_EXAMPLES)
    foreach(source IN LISTS TZM_EXAMPLE_SOURCES)
      string(SUBSTRING "${source}" 9 2 TZM_EXAMPLE_NUMBER)
      add_test(NAME tzm.example.${TZM_EXAMPLE_NUMBER}
               COMMAND tzm_example_${TZM_EXAMPLE_NUMBER}
               ${CMAKE_CURRENT_BINARY_DIR}/example-${TZM_EXAMPLE_NUMBER}-store)
    endforeach()
  endif()

  if(TZM_BUILD_TOOLS)
    add_test(NAME tzm.cli.self-check COMMAND tzm self-check)
    add_test(NAME tzm.cli.scenario
             COMMAND tzm scenario --store ${CMAKE_CURRENT_BINARY_DIR}/cli-scenario-store)
    add_test(NAME tzm.cli.inspect
             COMMAND tzm inspect --store ${CMAKE_CURRENT_BINARY_DIR}/cli-scenario-store)
  endif()

  if(TZM_BUILD_BENCHMARKS)
    add_test(NAME tzm.benchmark.smoke
             COMMAND tzm_benchmark --quick
                     --store ${CMAKE_CURRENT_BINARY_DIR}/benchmark-store)
  endif()
endif()

# ---------------------------------------------------------------------------
# Install and package
# ---------------------------------------------------------------------------

install(TARGETS thermal_zone_manager
  EXPORT ThermalZoneManagerTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES ${TZM_PUBLIC_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/thermal_zone_manager)

if(TZM_BUILD_TOOLS)
  install(TARGETS tzm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT ThermalZoneManagerTargets
  FILE ThermalZoneManagerTargets.cmake
  NAMESPACE ThermalZoneManager::
  DESTINATION ${TZM_INSTALL_CMAKEDIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThermalZoneManagerConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ThermalZoneManagerConfig.cmake
  INSTALL_DESTINATION ${TZM_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ThermalZoneManagerConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/ThermalZoneManagerConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/ThermalZoneManagerConfigVersion.cmake
  DESTINATION ${TZM_INSTALL_CMAKEDIR})

install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/ThermalZoneManager)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "ThermalZoneManager ${PROJECT_VERSION}")
message(STATUS "  compiler            : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type          : ${CMAKE_BUILD_TYPE}")
message(STATUS "  warnings as errors  : ${TZM_WARNINGS_AS_ERRORS}")
message(STATUS "  sanitizers          : ${TZM_SANITIZERS}")
message(STATUS "  tests/tools/examples: ${TZM_BUILD_TESTS}/${TZM_BUILD_TOOLS}/${TZM_BUILD_EXAMPLES}")
