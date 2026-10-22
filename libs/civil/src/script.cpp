// SPDX-License-Identifier: Apache-2.0
#include "cc/civil/script.hpp"

#include <cmath>
#include <exception>
#include <limits>

#include "cc/base/js.hpp"

namespace cc::civil {
namespace {

using base::Value;

// NOLINTNEXTLINE(misc-no-recursion) — bounded by the JSON parser's depth cap
Value dec(const Value& v) {
  if (v.is_string()) {
    const std::string& s = v.as_string();
    if (s == "$NaN") {
      return std::numeric_limits<double>::quiet_NaN();
    }
    if (s == "$Inf") {
      return std::numeric_limits<double>::infinity();
    }
    if (s == "$-Inf") {
      return -std::numeric_limits<double>::infinity();
    }
    return v;
  }
  if (v.is_array()) {
    base::Array out;
    out.reserve(v.as_array().size());
    for (const Value& x : v.as_array()) {
      out.push_back(dec(x));
    }
    return out;
  }
  if (v.is_object() && !v.is_point()) {
    Value o = Value::object();
    for (const auto& m : v.as_object()) {
      o.append(m.key, dec(m.value));
    }
    return o;
  }
  return v;
}

const std::map<std::string, ScriptOp, std::less<>>& ops() {
  static const auto kOps = [] {
    std::map<std::string, ScriptOp, std::less<>> m;
    add_core_ops(m);
    add_alignment_ops(m);
    return m;
  }();
  return kOps;
}

}  // namespace

Value arg(const base::Array& a, std::size_t i) {
  return i < a.size() ? a[i] : Value();
}
double num(const Value& v) {
  return base::js_to_number(v);
}
std::string str(const Value& v) {
  return base::js_string(v);
}
Value field(const Value& o, std::string_view k) {
  return o.is_object() ? o.get(k) : Value();
}
bool nullish(const Value& v) {
  return v.is_null() || v.is_undefined();
}
const base::Array& arr(const Value& v) {
  static const base::Array kEmpty;
  return v.is_array() ? v.as_array() : kEmpty;
}

std::vector<std::string> script_ops() {
  std::vector<std::string> out;
  for (const auto& [k, v] : ops()) {
    out.push_back(k);
  }
  return out;
}

Value run_script(const Value& script, const i18n::Catalog& words) {
  ScriptCtx ctx{&words, {}, nullptr, {}};
  Value out = Value::array();
  const Value calls = field(script, "calls");
  if (!calls.is_array()) {
    return out;
  }
  for (const Value& call : calls.as_array()) {
    Value r = Value::object();
    try {
      const std::string op = str(field(call, "op"));
      const Value self = field(call, "self");
      ctx.self = nullptr;
      if (!nullish(self)) {
        const auto it = ctx.reg.find(str(self));
        if (it == ctx.reg.end()) {
          throw ScriptError{"no object " + str(self)};
        }
        ctx.self = &it->second;
      }
      const Value as = field(call, "as");
      ctx.as = nullish(as) ? std::string() : str(as);
      const Value args = dec(field(call, "args"));
      const auto it = ops().find(op);
      if (it == ops().end()) {
        throw ScriptError{"unknown op " + op};
      }
      r.set("ok", it->second(args.is_array() ? args.as_array() : base::Array{}, ctx));
    } catch (const ScriptError& e) {
      r.set("err", e.message);
    } catch (const std::exception& e) {
      r.set("err", std::string("internal: ") + e.what());
    }
    out.push(std::move(r));
  }
  return out;
}

}  // namespace cc::civil
