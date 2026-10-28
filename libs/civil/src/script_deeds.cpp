// SPDX-License-Identifier: Apache-2.0
// Script ops for parcels.js and traverse.js (tools/parity/civil/ops-civil.mjs has their JS twins).
#include <memory>

#include "cc/civil/deeds.hpp"
#include "cc/civil/script.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::Value;

Value opt(const std::optional<double>& v) {
  return v ? Value(*v) : Value(nullptr);
}

}  // namespace

void add_deeds_ops(std::map<std::string, ScriptOp, std::less<>>& m) {
  m["pc.clean"] = [](const Array& a, ScriptCtx&) -> Value { return clean_parcel(arg(a, 0)); };
  m["pc.parseAngle"] = [](const Array& a, ScriptCtx&) -> Value { return opt(parse_angle(arg(a, 0))); };
  m["pc.parseBearing"] = [](const Array& a, ScriptCtx&) -> Value { return opt(parse_bearing(arg(a, 0))); };
  m["pc.parseCalls"] = [](const Array& a, ScriptCtx& c) -> Value { return parse_calls(arg(a, 0), *c.words); };
  m["pc.runText"] = [](const Array& a, ScriptCtx& c) -> Value {
    return run_calls(arg(a, 0), parse_calls(arg(a, 1), *c.words).get("calls"));
  };
  m["pc.runCalls"] = [](const Array& a, ScriptCtx&) -> Value { return run_calls(arg(a, 0), arg(a, 1)); };
  m["pc.courses"] = [](const Array& a, ScriptCtx&) -> Value { return courses(arg(a, 0)); };
  m["pc.area"] = [](const Array& a, ScriptCtx&) -> Value { return parcel_area(arg(a, 0)); };
  m["pc.perimeter"] = [](const Array& a, ScriptCtx&) -> Value { return perimeter(arg(a, 0)); };
  m["pc.centroid"] = [](const Array& a, ScriptCtx&) -> Value { return centroid(arg(a, 0)); };
  m["pc.areaWords"] = [](const Array& a, ScriptCtx& c) -> Value {
    return area_words(num(arg(a, 0)), str(arg(a, 1)), *c.words);
  };
  m["pc.legal"] = [](const Array& a, ScriptCtx& c) -> Value {
    return legal_description(arg(a, 0), arg(a, 1), *c.words);
  };
  m["pc.labels"] = [](const Array& a, ScriptCtx&) -> Value { return parcel_labels(arg(a, 0), arg(a, 1)); };

  m["tv.parse"] = [](const Array& a, ScriptCtx& c) -> Value {
    return parse_traverse(arg(a, 0), nullish(arg(a, 1)) ? "angles" : str(arg(a, 1)), *c.words);
  };
  m["tv.run"] = [](const Array& a, ScriptCtx& c) -> Value {
    Value o = arg(a, 0);
    if (o.truthy() && field(o, "legsText").is_string()) {
      const Value mode = field(o, "mode");
      o.set("legs",
            parse_traverse(field(o, "legsText"), mode.is_undefined() ? "angles" : str(mode), *c.words).get("legs"));
    }
    auto r = std::make_shared<Value>(run_traverse(o, *c.words));
    c.keep(r);
    return *r;
  };
  m["tv.report"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value u = field(arg(a, 0), "units");
    return traverse_report(self_as<Value>(c), u.is_undefined() ? "ft" : str(u), *c.words);
  };
  m["tv.dir2az"] = [](const Array& a, ScriptCtx&) -> Value { return dir2az(num(arg(a, 0))); };
}

}  // namespace cc::civil
