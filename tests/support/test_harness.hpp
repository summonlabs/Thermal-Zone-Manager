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

#ifndef TZM_TEST_HARNESS_HPP
#define TZM_TEST_HARNESS_HPP

#include <cstdint>
#include <string>
#include <type_traits>

#include "thermal_zone_manager/errors.hpp"

namespace tzm_test {

using TestBody = void (*)();

void register_test(const char* suite, const char* name, TestBody body);

struct Registrar {
  Registrar(const char* suite, const char* name, TestBody body) {
    register_test(suite, name, body);
  }
};

// Thrown to abandon the current test without running the rest of its body.
struct AbortTest {};

void report_failure(const char* file, int line, const std::string& message);
void report_note(const std::string& message);

int run_all(int argc, char** argv);

// -- value rendering for assertion messages ---------------------------------

std::string debug_text(const std::string& value);
std::string debug_text(std::string_view value);
std::string debug_text(const char* value);
std::string debug_text(bool value);
std::string debug_text(std::nullptr_t);

template <class T>
std::string debug_text(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(value);
  } else {
    return "<value>";
  }
}

}  // namespace tzm_test

#define TZM_TEST(suite, name)                                            \
  static void tzm_test_##suite##_##name();                               \
  static const ::tzm_test::Registrar tzm_registrar_##suite##_##name(     \
      #suite, #name, &tzm_test_##suite##_##name);                        \
  static void tzm_test_##suite##_##name()

#define TZM_CHECK(expr)                                                          \
  do {                                                                           \
    if (!(expr)) {                                                               \
      ::tzm_test::report_failure(__FILE__, __LINE__, "check failed: " #expr);    \
    }                                                                            \
  } while (false)

#define TZM_REQUIRE(expr)                                                        \
  do {                                                                           \
    if (!(expr)) {                                                               \
      ::tzm_test::report_failure(__FILE__, __LINE__, "require failed: " #expr);  \
      throw ::tzm_test::AbortTest();                                             \
    }                                                                            \
  } while (false)

#define TZM_CHECK_EQ(lhs_expr, rhs_expr)                                              \
  do {                                                                                \
    const auto tzm_lhs = (lhs_expr);                                                  \
    const auto tzm_rhs = (rhs_expr);                                                  \
    if (!(tzm_lhs == tzm_rhs)) {                                                      \
      ::tzm_test::report_failure(                                                     \
          __FILE__, __LINE__,                                                         \
          std::string("expected ") + #lhs_expr + " == " + #rhs_expr + ", got " +      \
              ::tzm_test::debug_text(tzm_lhs) + " vs " + ::tzm_test::debug_text(tzm_rhs)); \
    }                                                                                 \
  } while (false)

#define TZM_CHECK_NE(lhs_expr, rhs_expr)                                              \
  do {                                                                                \
    const auto tzm_lhs = (lhs_expr);                                                  \
    const auto tzm_rhs = (rhs_expr);                                                  \
    if (tzm_lhs == tzm_rhs) {                                                         \
      ::tzm_test::report_failure(                                                     \
          __FILE__, __LINE__,                                                         \
          std::string("expected ") + #lhs_expr + " != " + #rhs_expr + ", both are " + \
              ::tzm_test::debug_text(tzm_lhs));                                       \
    }                                                                                 \
  } while (false)

#define TZM_REQUIRE_EQ(lhs_expr, rhs_expr)                                            \
  do {                                                                                \
    const auto tzm_lhs = (lhs_expr);                                                  \
    const auto tzm_rhs = (rhs_expr);                                                  \
    if (!(tzm_lhs == tzm_rhs)) {                                                      \
      ::tzm_test::report_failure(                                                     \
          __FILE__, __LINE__,                                                         \
          std::string("expected ") + #lhs_expr + " == " + #rhs_expr + ", got " +      \
              ::tzm_test::debug_text(tzm_lhs) + " vs " + ::tzm_test::debug_text(tzm_rhs)); \
      throw ::tzm_test::AbortTest();                                                  \
    }                                                                                 \
  } while (false)

#define TZM_CHECK_OK(expr)                                                       \
  do {                                                                           \
    auto&& tzm_result = (expr);                                                  \
    if (!tzm_result.ok()) {                                                      \
      ::tzm_test::report_failure(__FILE__, __LINE__,                             \
                                 std::string("expected success from " #expr      \
                                             " but got: ") +                     \
                                     tzm_result.error().to_string());            \
    }                                                                            \
  } while (false)

#define TZM_REQUIRE_OK(expr)                                                     \
  do {                                                                           \
    auto&& tzm_result = (expr);                                                  \
    if (!tzm_result.ok()) {                                                      \
      ::tzm_test::report_failure(__FILE__, __LINE__,                             \
                                 std::string("expected success from " #expr      \
                                             " but got: ") +                     \
                                     tzm_result.error().to_string());            \
      throw ::tzm_test::AbortTest();                                             \
    }                                                                            \
  } while (false)

#define TZM_CHECK_ERR(expr, expected_code)                                            \
  do {                                                                                \
    auto&& tzm_result = (expr);                                                       \
    if (tzm_result.ok()) {                                                            \
      ::tzm_test::report_failure(__FILE__, __LINE__,                                  \
                                 std::string("expected " #expected_code " from " #expr \
                                             " but it succeeded"));                   \
    } else if (tzm_result.error().code() != (expected_code)) {                        \
      ::tzm_test::report_failure(                                                     \
          __FILE__, __LINE__,                                                         \
          std::string("expected " #expected_code " from " #expr " but got ") +        \
              std::string(::thermal_zone_manager::to_string(tzm_result.error().code())) + \
              ": " + tzm_result.error().to_string());                                 \
    }                                                                                 \
  } while (false)

#define TZM_REQUIRE_ERR(expr, expected_code)                                          \
  do {                                                                                \
    auto&& tzm_result = (expr);                                                       \
    if (tzm_result.ok()) {                                                            \
      ::tzm_test::report_failure(__FILE__, __LINE__,                                  \
                                 std::string("expected " #expected_code " from " #expr \
                                             " but it succeeded"));                   \
      throw ::tzm_test::AbortTest();                                                  \
    } else if (tzm_result.error().code() != (expected_code)) {                        \
      ::tzm_test::report_failure(                                                     \
          __FILE__, __LINE__,                                                         \
          std::string("expected " #expected_code " from " #expr " but got ") +        \
              std::string(::thermal_zone_manager::to_string(tzm_result.error().code())) + \
              ": " + tzm_result.error().to_string());                                 \
      throw ::tzm_test::AbortTest();                                                  \
    }                                                                                 \
  } while (false)

#endif  // TZM_TEST_HARNESS_HPP
