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

#include "test_process.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "thermal_zone_manager/thermal_zone_manager.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace tzm_test {

namespace {

using thermal_zone_manager::ActorId;
using thermal_zone_manager::Configuration;
using thermal_zone_manager::ConfigurationGeneration;
using thermal_zone_manager::ConfigurationReceipt;
using thermal_zone_manager::ConfigurationRequest;
using thermal_zone_manager::ControlPlaneEpoch;
using thermal_zone_manager::CommandId;
using thermal_zone_manager::CouplingEdge;
using thermal_zone_manager::CouplingGraph;
using thermal_zone_manager::EngineOptions;
using thermal_zone_manager::Error;
using thermal_zone_manager::ErrorCode;
using thermal_zone_manager::EvaluationCommitRequest;
using thermal_zone_manager::EvaluationId;
using thermal_zone_manager::EvaluationRequest;
using thermal_zone_manager::EvidenceGeneration;
using thermal_zone_manager::MicroKelvinPerWatt;
using thermal_zone_manager::MilliCelsius;
using thermal_zone_manager::MilliWatts;
using thermal_zone_manager::Nanoseconds;
using thermal_zone_manager::ObservationReceipt;
using thermal_zone_manager::ObservationSequence;
using thermal_zone_manager::PartsPerMillion;
using thermal_zone_manager::PolicyGeneration;
using thermal_zone_manager::ProvenanceKind;
using thermal_zone_manager::Result;
using thermal_zone_manager::SourceId;
using thermal_zone_manager::Status;
using thermal_zone_manager::StoreAccess;
using thermal_zone_manager::TemperatureEnvelope;
using thermal_zone_manager::TemperatureObservation;
using thermal_zone_manager::ThermalPolicy;
using thermal_zone_manager::ThermalZoneEngine;
using thermal_zone_manager::ZoneConfiguration;
using thermal_zone_manager::ZoneGeneration;
using thermal_zone_manager::ZoneId;
using thermal_zone_manager::ZoneName;

void sleep_ms(unsigned millis) { std::this_thread::sleep_for(std::chrono::milliseconds(millis)); }

#if defined(_WIN32)

std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), needed);
  return wide;
}

std::string narrow(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed,
                      nullptr, nullptr);
  return out;
}

std::wstring quoted(const std::string& argument) {
  std::wstring out = L"\"";
  for (const char raw : argument) {
    if (raw == '"') {
      out.push_back(L'\\');
    }
    out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(raw)));
  }
  out.push_back(L'"');
  return out;
}

#endif

}  // namespace

std::string executable_path() {
#if defined(_WIN32)
  std::wstring buffer(4096, L'\0');
  const DWORD written =
      GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (written == 0) {
    return std::string();
  }
  buffer.resize(written);
  return narrow(buffer);
#else
  std::vector<char> buffer(4096, '\0');
  const ssize_t written = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (written <= 0) {
    return std::string();
  }
  buffer[static_cast<std::size_t>(written)] = '\0';
  return std::string(buffer.data());
#endif
}

Result<ChildProcess> spawn_child(const std::vector<std::string>& arguments) {
  const std::string exe = executable_path();
  if (exe.empty()) {
    return Error(ErrorCode::ProcessFailure, "the test binary path could not be determined");
  }
#if defined(_WIN32)
  std::wstring command_line = quoted(exe);
  for (const std::string& argument : arguments) {
    command_line.push_back(L' ');
    command_line.append(quoted(argument));
  }
  std::wstring mutable_command = command_line;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information{};
  // Handles are not inherited and no pipes are created, so a child can never
  // block on a stream that the parent is not draining.
  if (CreateProcessW(widen(exe).c_str(), mutable_command.data(), nullptr, nullptr, FALSE, 0,
                     nullptr, nullptr, &startup, &information) == 0) {
    return Error(ErrorCode::ProcessFailure, "the child process could not be started")
        .with("path", exe)
        .with("system_error", static_cast<std::uint64_t>(GetLastError()));
  }
  CloseHandle(information.hThread);
  ChildProcess child;
  child.handle = information.hProcess;
  child.pid = information.dwProcessId;
  child.running = true;
  return child;
#else
  std::vector<std::string> storage;
  storage.push_back(exe);
  for (const std::string& argument : arguments) {
    storage.push_back(argument);
  }
  std::vector<char*> argv;
  for (std::string& item : storage) {
    argv.push_back(item.data());
  }
  argv.push_back(nullptr);
  pid_t pid = 0;
  if (::posix_spawn(&pid, storage.front().c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
    return Error(ErrorCode::ProcessFailure, "the child process could not be started");
  }
  ChildProcess child;
  child.pid = static_cast<unsigned long>(pid);
  child.handle = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
  child.running = true;
  return child;
#endif
}

Result<unsigned long> wait_child(ChildProcess& child) {
  if (!child.running) {
    return Error(ErrorCode::ProcessFailure, "the child process is not running");
  }
#if defined(_WIN32)
  const DWORD outcome = WaitForSingleObject(static_cast<HANDLE>(child.handle), INFINITE);
  if (outcome != WAIT_OBJECT_0) {
    return Error(ErrorCode::ProcessFailure, "waiting for the child process failed")
        .with("system_error", static_cast<std::uint64_t>(GetLastError()));
  }
  DWORD code = 0;
  if (GetExitCodeProcess(static_cast<HANDLE>(child.handle), &code) == 0) {
    return Error(ErrorCode::ProcessFailure, "the child exit code could not be read");
  }
  CloseHandle(static_cast<HANDLE>(child.handle));
  child.handle = nullptr;
  child.running = false;
  return static_cast<unsigned long>(code);
#else
  int status = 0;
  if (::waitpid(static_cast<pid_t>(child.pid), &status, 0) < 0) {
    return Error(ErrorCode::ProcessFailure, "waiting for the child process failed");
  }
  child.running = false;
  if (WIFEXITED(status)) {
    return static_cast<unsigned long>(WEXITSTATUS(status));
  }
  return 1UL;
#endif
}

Result<unsigned long> terminate_child(ChildProcess& child) {
  if (!child.running) {
    return Error(ErrorCode::ProcessFailure, "the child process is not running");
  }
#if defined(_WIN32)
  if (TerminateProcess(static_cast<HANDLE>(child.handle), 99U) == 0) {
    return Error(ErrorCode::ProcessFailure, "the child process could not be terminated")
        .with("system_error", static_cast<std::uint64_t>(GetLastError()));
  }
  WaitForSingleObject(static_cast<HANDLE>(child.handle), INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(child.handle), &code);
  CloseHandle(static_cast<HANDLE>(child.handle));
  child.handle = nullptr;
  child.running = false;
  return static_cast<unsigned long>(code);
#else
  ::kill(static_cast<pid_t>(child.pid), SIGKILL);
  int status = 0;
  ::waitpid(static_cast<pid_t>(child.pid), &status, 0);
  child.running = false;
  return 99UL;
#endif
}

void release_child(ChildProcess& child) {
  if (child.handle == nullptr) {
    return;
  }
#if defined(_WIN32)
  CloseHandle(static_cast<HANDLE>(child.handle));
#endif
  child.handle = nullptr;
  child.running = false;
}

bool file_exists(const std::string& path) {
  std::error_code code;
  return std::filesystem::exists(std::filesystem::path(path), code);
}

Status write_text_file(const std::string& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.good()) {
    return Error(ErrorCode::IoFailure, "a file could not be created").with("path", path);
  }
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  stream.flush();
  if (!stream.good()) {
    return Error(ErrorCode::IoFailure, "a file could not be written").with("path", path);
  }
  return Status();
}

Result<std::string> read_text_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.good()) {
    return Error(ErrorCode::IoFailure, "a file could not be opened").with("path", path);
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

Status make_directory(const std::string& path) {
  std::error_code code;
  std::filesystem::create_directories(std::filesystem::path(path), code);
  if (code) {
    return Error(ErrorCode::IoFailure, "a directory could not be created")
        .with("path", path)
        .with("system_error", code.message());
  }
  return Status();
}

Status remove_tree(const std::string& path) {
  std::error_code code;
  std::filesystem::remove_all(std::filesystem::path(path), code);
  return Status();
}

// ---------------------------------------------------------------------------
// Child modes
// ---------------------------------------------------------------------------

namespace {

EngineOptions child_options(const std::string& directory) {
  EngineOptions options;
  options.root = directory;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  options.actor = thermal_zone_manager::ActorId::literal("child");
  return options;
}

Status establish_epoch(ThermalZoneEngine& engine, std::uint64_t target) {
  thermal_zone_manager::EpochRequest request;
  request.command = CommandId::from_value(1);
  request.attempt = thermal_zone_manager::AttemptId::first();
  request.expected_current = engine.fencing().epoch;
  request.target = ControlPlaneEpoch::from_value(target);
  Result<thermal_zone_manager::EpochReceipt> receipt = engine.advance_epoch(request);
  if (!receipt.ok()) {
    return receipt.error();
  }
  return Status();
}

Status apply_fixed_configuration(ThermalZoneEngine& engine, std::uint64_t marker) {
  ConfigurationRequest request;
  request.command = CommandId::from_value(2);
  request.attempt = thermal_zone_manager::AttemptId::first();
  request.epoch = engine.fencing().epoch;
  request.expected_generation = engine.configuration().generation();

  thermal_zone_manager::ZoneConfiguration zone;
  zone.id = ZoneId::from_value(1);
  Result<thermal_zone_manager::ZoneName> name =
      thermal_zone_manager::ZoneName::create("rack-a");
  if (!name.ok()) {
    return name.error();
  }
  zone.name = name.value();
  zone.generation = ZoneGeneration::first();
  zone.envelope.floor_temp = MilliCelsius{-5000};
  zone.envelope.derate_onset = MilliCelsius{60000};
  zone.envelope.ceiling_temp = MilliCelsius{80000};
  zone.envelope.critical_temp = MilliCelsius{90000};
  zone.declared_heat = MilliWatts{4000000 + static_cast<std::int64_t>(marker % 1000U)};
  zone.thermal_resistance = MicroKelvinPerWatt{12000};
  zone.derate_steps = 8;
  zone.recovery_margin = MilliCelsius{1000};
  request.zones.push_back(zone);

  request.policy.generation = thermal_zone_manager::PolicyGeneration::first();
  Result<thermal_zone_manager::ConfigurationReceipt> receipt = engine.apply_configuration(request);
  if (!receipt.ok()) {
    return receipt.error();
  }
  return Status();
}

}  // namespace

Status child_hold_lock(const std::string& directory, const std::string& ready_file,
                             const std::string& stop_file) {
  Result<std::unique_ptr<ThermalZoneEngine>> engine =
      ThermalZoneEngine::open(child_options(directory));
  if (!engine.ok()) {
    return engine.error();
  }
  if (!engine.value()->holds_writer()) {
    return Error(ErrorCode::Internal, "the child did not obtain the writer lock");
  }
  Status ready = write_text_file(ready_file, "ready");
  if (!ready.ok()) {
    return ready.error();
  }
  while (!file_exists(stop_file)) {
    sleep_ms(2);
  }
  return Status();
}

Status child_try_lock(const std::string& directory, const std::string& output_file) {
  Result<std::unique_ptr<ThermalZoneEngine>> engine =
      ThermalZoneEngine::open(child_options(directory));
  std::string text;
  if (engine.ok()) {
    text = engine.value()->holds_writer() ? "acquired" : "not-writer";
  } else {
    text = std::string("refused ") +
           std::string(thermal_zone_manager::to_string(engine.error().code()));
  }
  return write_text_file(output_file, text);
}

Status child_commit_loop(const std::string& directory, const std::string& counter_file) {
  Result<std::unique_ptr<ThermalZoneEngine>> engine =
      ThermalZoneEngine::open(child_options(directory));
  if (!engine.ok()) {
    return engine.error();
  }
  ThermalZoneEngine& target = *engine.value();
  if (target.fencing().epoch.is_zero()) {
    const Status established = establish_epoch(target, 1);
    if (!established.ok()) {
      return established.error();
    }
  }
  if (target.configuration().empty()) {
    const Status applied = apply_fixed_configuration(target, 0);
    if (!applied.ok()) {
      return applied.error();
    }
  }

  for (std::uint64_t round = 0;; ++round) {
    TemperatureObservation observation;
    observation.zone = ZoneId::from_value(1);
    observation.zone_generation = target.configuration().zones().front().generation;
    observation.evidence_generation = target.configuration().evidence_generation();
    observation.sequence = thermal_zone_manager::ObservationSequence::from_value(
        target.fencing().revision.raw() + 1U);
    observation.temperature = MilliCelsius{50000 + static_cast<std::int64_t>(round % 5000U)};
    observation.source = thermal_zone_manager::SourceId::literal("child-loop");
    observation.provenance = ProvenanceKind::SyntheticHarness;
    observation.observed_at = thermal_zone_manager::system_now();
    observation.validity = Nanoseconds{3600000000000LL};
    observation.publisher_epoch = target.fencing().epoch;
    observation.publisher_incarnation = target.incarnation();
    Result<thermal_zone_manager::ObservationReceipt> receipt =
        target.ingest_observation(observation);
    if (!receipt.ok()) {
      return receipt.error();
    }
    const Status written =
        write_text_file(counter_file, std::to_string(receipt.value().commit.raw()));
    if (!written.ok()) {
      return written.error();
    }
  }
}

Status child_read_state(const std::string& directory, const std::string& output_file) {
  EngineOptions options;
  options.root = directory;
  options.access = StoreAccess::ReadOnly;
  options.create_if_missing = false;
  Result<std::unique_ptr<ThermalZoneEngine>> engine = ThermalZoneEngine::open(options);
  if (!engine.ok()) {
    return engine.error();
  }
  const std::string text =
      "commit=" + thermal_zone_manager::to_string(engine.value()->fencing().commit) +
      " revision=" + thermal_zone_manager::to_string(engine.value()->fencing().revision) +
      " zones=" +
      std::to_string(static_cast<unsigned long long>(
          engine.value()->configuration().zone_count())) +
      " digest=" + thermal_zone_manager::to_hex(engine.value()->configuration().digest()) + "\n";
  return write_text_file(output_file, text);
}

Status child_publish_state(const std::string& directory) {
  Result<std::unique_ptr<ThermalZoneEngine>> engine =
      ThermalZoneEngine::open(child_options(directory));
  if (!engine.ok()) {
    return engine.error();
  }
  if (engine.value()->fencing().epoch.is_zero()) {
    const Status established = establish_epoch(*engine.value(), 1);
    if (!established.ok()) {
      return established.error();
    }
  }
  if (!engine.value()->configuration().empty()) {
    // The store already carries a declared world; publishing again would reuse
    // a command identifier, so the child reports success without re-applying.
    return Status();
  }
  return apply_fixed_configuration(*engine.value(), 7);
}

Status child_publish_and_hold(const std::string& directory, const std::string& ready_file,
                                    const std::string& stop_file) {
  const Status published = child_publish_state(directory);
  if (!published.ok()) {
    return published.error();
  }
  Status ready = write_text_file(ready_file, "ready");
  if (!ready.ok()) {
    return ready.error();
  }
  while (!file_exists(stop_file)) {
    sleep_ms(2);
  }
  return Status();
}

int run_child_mode(int argc, char** argv) {
  if (argc < 3) {
    return -1;
  }
  const std::string mode = argv[2];
  Status outcome = Error(ErrorCode::UnknownCommand, "unknown child mode");
  if (mode == "hold-lock" && argc >= 6) {
    outcome = child_hold_lock(argv[3], argv[4], argv[5]);
  } else if (mode == "try-lock" && argc >= 5) {
    outcome = child_try_lock(argv[3], argv[4]);
  } else if (mode == "commit-loop" && argc >= 5) {
    outcome = child_commit_loop(argv[3], argv[4]);
  } else if (mode == "read-state" && argc >= 5) {
    outcome = child_read_state(argv[3], argv[4]);
  } else if (mode == "publish" && argc >= 4) {
    outcome = child_publish_state(argv[3]);
  } else if (mode == "publish-and-hold" && argc >= 6) {
    outcome = child_publish_and_hold(argv[3], argv[4], argv[5]);
  } else {
    return -1;
  }
  if (outcome.ok()) {
    return 0;
  }
  std::fprintf(stderr, "child mode %s failed: %s\n", mode.c_str(),
               outcome.error().to_string().c_str());
  return 2;
}

}  // namespace tzm_test
