// SPDX-License-Identifier: Apache-2.0
// msg(key, {{"name", value}…}) — the English text of a cc-dwg note or error (messages.js).
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

#include "cc/base/value.hpp"

namespace cc::dwg {

struct Fill {
  std::string_view name;
  base::Value value;
};

[[nodiscard]] std::string msg(std::string_view key, std::initializer_list<Fill> fills = {});

}  // namespace cc::dwg
