// SPDX-License-Identifier: Apache-2.0
// A strict JSON parser into cc::base::Value with hard limits (AGENTS.md section 4: input is hostile; depth and size are
// capped before anything is allocated for them). Accepts what write_json() writes, so engine models travel over a pipe
// (the importer helper) and back unchanged — except the parity kit's markers, which stay what they are: "$NaN",
// "$Inf", "$-Inf" are strings and {"$base64": …} is an object.
#pragma once

#include <cstddef>
#include <string_view>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"

namespace cc::base {

struct JsonLimits {
  std::size_t max_bytes = std::size_t{1} << 30U;   // input size
  std::size_t max_depth = 512;                     // nested arrays/objects
  std::size_t max_string = std::size_t{1} << 28U;  // one string, bytes
};

/// Parses one JSON document (surrounding whitespace allowed). Duplicate object keys: the last wins, in the first
/// one's place (JSON.parse).
[[nodiscard]] Result<Value> parse_json(std::string_view text, const JsonLimits& limits = {});

}  // namespace cc::base
