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

#include "thermal_zone_manager/ids.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "thermal_zone_manager/limits.hpp"

namespace thermal_zone_manager {
namespace {

// Strict UTF-8: rejects overlong forms, surrogates, values above U+10FFFF,
// control characters and the C1 control block.
bool validate_utf8_no_controls(std::string_view text) {
  std::size_t index = 0;
  const std::size_t size = text.size();
  while (index < size) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t continuation = 0;
    if (lead <= 0x7F) {
      if (lead < 0x20 || lead == 0x7F) {
        return false;
      }
      ++index;
      continue;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
      continuation = 1;
    } else if (lead == 0xE0) {
      if (index + 1 >= size) {
        return false;
      }
      const auto second = static_cast<unsigned char>(text[index + 1]);
      if (second < 0xA0 || second > 0xBF) {
        return false;
      }
      continuation = 2;
    } else if (lead >= 0xE1 && lead <= 0xEC) {
      continuation = 2;
    } else if (lead == 0xED) {
      if (index + 1 >= size) {
        return false;
      }
      const auto second = static_cast<unsigned char>(text[index + 1]);
      if (second < 0x80 || second > 0x9F) {
        return false;
      }
      continuation = 2;
    } else if (lead == 0xEE || lead == 0xEF) {
      continuation = 2;
    } else if (lead == 0xF0) {
      if (index + 1 >= size) {
        return false;
      }
      const auto second = static_cast<unsigned char>(text[index + 1]);
      if (second < 0x90 || second > 0xBF) {
        return false;
      }
      continuation = 3;
    } else if (lead >= 0xF1 && lead <= 0xF3) {
      continuation = 3;
    } else if (lead == 0xF4) {
      if (index + 1 >= size) {
        return false;
      }
      const auto second = static_cast<unsigned char>(text[index + 1]);
      if (second < 0x80 || second > 0x8F) {
        return false;
      }
      continuation = 3;
    } else {
      return false;
    }

    if (lead == 0xC2) {
      // U+0080 .. U+009F are C1 controls.
      const auto second = static_cast<unsigned char>(text[index + 1]);
      if (second >= 0x80 && second <= 0x9F) {
        return false;
      }
    }
    for (std::size_t step = 1; step <= continuation; ++step) {
      const auto next = static_cast<unsigned char>(text[index + step]);
      if (next < 0x80 || next > 0xBF) {
        return false;
      }
    }
    index += continuation + 1;
  }
  return true;
}

bool has_edge_whitespace(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  const char first = text.front();
  const char last = text.back();
  const auto is_space = [](char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' ||
           value == '\v';
  };
  return is_space(first) || is_space(last);
}

}  // namespace

bool is_valid_identifier_text(std::string_view text) {
  return !text.empty() && !has_edge_whitespace(text) && validate_utf8_no_controls(text);
}

Result<BoundedText> BoundedText::create(std::string_view text, std::size_t max_length) {
  if (text.empty()) {
    return Error(ErrorCode::InvalidArgument, "a text identity may not be empty");
  }
  if (text.size() > max_length) {
    return Error(ErrorCode::NameTooLong, "a text identity exceeds its declared length limit")
        .with("length", static_cast<std::uint64_t>(text.size()))
        .with("limit", static_cast<std::uint64_t>(max_length));
  }
  if (!validate_utf8_no_controls(text)) {
    return Error(ErrorCode::TextNotValidUtf8,
                 "a text identity must be well-formed UTF-8 without control characters");
  }
  if (has_edge_whitespace(text)) {
    return Error(ErrorCode::InvalidArgument,
                 "a text identity may not begin or end with whitespace");
  }
  BoundedText result;
  result.text_ = std::string(text);
  return result;
}

BoundedText BoundedText::literal(std::string_view text) {
  if (text.empty() || text.size() > kMaxTextLength || !is_valid_identifier_text(text)) {
    return BoundedText();
  }
  BoundedText result;
  result.text_ = std::string(text);
  return result;
}

Result<ActorId> ActorId::create(std::string_view text) {
  Result<BoundedText> base = BoundedText::create(text, kMaxActorLength);
  if (!base.ok()) {
    return base.error();
  }
  return ActorId(std::move(base.value()));
}

Result<SourceId> SourceId::create(std::string_view text) {
  Result<BoundedText> base = BoundedText::create(text, kMaxSourceLength);
  if (!base.ok()) {
    return base.error();
  }
  return SourceId(std::move(base.value()));
}

Result<ZoneName> ZoneName::create(std::string_view text) {
  Result<BoundedText> base = BoundedText::create(text, kMaxZoneNameLength);
  if (!base.ok()) {
    return base.error();
  }
  return ZoneName(std::move(base.value()));
}

#define TZM_COUNTER_TO_STRING(Type)                          \
  std::string to_string(Type value) {                        \
    return std::to_string(value.raw());                      \
  }

TZM_COUNTER_TO_STRING(ZoneId)
TZM_COUNTER_TO_STRING(ZoneGeneration)
TZM_COUNTER_TO_STRING(ConfigurationGeneration)
TZM_COUNTER_TO_STRING(EvidenceGeneration)
TZM_COUNTER_TO_STRING(StoreRevision)
TZM_COUNTER_TO_STRING(ControlPlaneEpoch)
TZM_COUNTER_TO_STRING(CommitSequence)
TZM_COUNTER_TO_STRING(ObservationSequence)
TZM_COUNTER_TO_STRING(EvaluationId)
TZM_COUNTER_TO_STRING(CommandId)
TZM_COUNTER_TO_STRING(AttemptId)
TZM_COUNTER_TO_STRING(ControllerIncarnation)
TZM_COUNTER_TO_STRING(PolicyGeneration)

#undef TZM_COUNTER_TO_STRING

}  // namespace thermal_zone_manager
