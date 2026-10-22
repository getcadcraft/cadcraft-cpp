// SPDX-License-Identifier: Apache-2.0
// Call scripts: the C++ side of tools/parity/civil.mjs. A script is {calls: [{op, args, self, as}]}; each call runs one
// CivilCraft engine function (units.formatStation, tin.buildSurface, surface.contours, …) on JSON arguments and gives
// {ok: result} or {err: message}. `as` keeps the object a call made under a name, `self` calls a method on one. The
// harness runs the same script on the frozen 0.5 JS modules and compares every result.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::civil {

class Surface;
class Alignment;
class Profile;

/// An object a script keeps between calls.
using ScriptObject = std::variant<std::shared_ptr<Surface>, std::shared_ptr<Alignment>, std::shared_ptr<Profile>>;

struct ScriptCtx {
  const i18n::Catalog* words;  // never null
  std::map<std::string, ScriptObject> reg;
  const ScriptObject* self = nullptr;
  std::string as;  // the call's `as`, or empty
  void keep(ScriptObject o) {
    if (!as.empty()) {
      reg.insert_or_assign(as, std::move(o));
    }
  }
};

/// One op: arguments (decoded: "$NaN" … are numbers again) → result. Throws ScriptError for the JS's thrown errors.
using ScriptOp = std::function<base::Value(const base::Array& args, ScriptCtx& ctx)>;

struct ScriptError {
  std::string message;
};

/// Runs a script; the result is one {ok} / {err} per call.
[[nodiscard]] base::Value run_script(const base::Value& script, const i18n::Catalog& words);

/// The names of the ops run_script knows, sorted.
[[nodiscard]] std::vector<std::string> script_ops();

/// The op tables (one per area; script_*.cpp).
void add_core_ops(std::map<std::string, ScriptOp, std::less<>>& m);
void add_alignment_ops(std::map<std::string, ScriptOp, std::less<>>& m);

// helpers the op tables share
[[nodiscard]] base::Value arg(const base::Array& a, std::size_t i);         // undefined past the end
[[nodiscard]] double num(const base::Value& v);                             // Number(v)
[[nodiscard]] std::string str(const base::Value& v);                        // String(v)
[[nodiscard]] base::Value field(const base::Value& o, std::string_view k);  // o[k] (undefined when not an object)
[[nodiscard]] bool nullish(const base::Value& v);
[[nodiscard]] const base::Array& arr(const base::Value& v);  // the array, or an empty one
/// The registered object `name` of type T, or null.
template <class T>
[[nodiscard]] std::shared_ptr<T> reg_get(const ScriptCtx& c, const std::string& name) {
  const auto it = c.reg.find(name);
  if (it == c.reg.end() || !std::holds_alternative<std::shared_ptr<T>>(it->second)) {
    return nullptr;
  }
  return std::get<std::shared_ptr<T>>(it->second);
}
/// The call's `self` as a T (a ScriptError when it is something else).
template <class T>
[[nodiscard]] T& self_as(const ScriptCtx& c) {
  if (c.self == nullptr || !std::holds_alternative<std::shared_ptr<T>>(*c.self)) {
    throw ScriptError{"self is not the right kind of object"};
  }
  return *std::get<std::shared_ptr<T>>(*c.self);
}

}  // namespace cc::civil
