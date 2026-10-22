// SPDX-License-Identifier: Apache-2.0
// The op table of run_script: each module's ops are added by its own file (ops_shapes.cpp, …).
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "cc/base/value.hpp"

namespace cc::partcraft::ops {

using Args = std::span<const base::Value>;
using Fn = std::function<base::Value(Args)>;
using Table = std::map<std::string, Fn, std::less<>>;

/// args[i], or undefined past the end (a JS function called with fewer arguments).
[[nodiscard]] inline const base::Value& arg(Args a, std::size_t i) {
  static const base::Value kUndefined;
  return i < a.size() ? a[i] : kUndefined;
}
/// A JSON null standing for an argument left out (the driver's `x ?? undefined`).
[[nodiscard]] inline base::Value opt(Args a, std::size_t i) {
  const base::Value& v = arg(a, i);
  return v.is_null() ? base::Value{} : v;
}
/// What a JS op throws: its message becomes the call's {err}.
struct JsError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
/// The TypeError V8 gives for `x.key` on null / undefined.
[[noreturn]] inline void throw_read(const base::Value& of, std::string_view key) {
  throw JsError(std::string("Cannot read properties of ") + (of.is_null() ? "null" : "undefined") + " (reading '" +
                std::string(key) + "')");
}

void add_shapes(Table& t);

}  // namespace cc::partcraft::ops
