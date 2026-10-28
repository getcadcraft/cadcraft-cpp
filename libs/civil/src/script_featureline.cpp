// SPDX-License-Identifier: Apache-2.0
// Script ops for featureline.js (tools/parity/civil/ops-civil.mjs has their JS twins).
#include "cc/civil/featureline.hpp"
#include "cc/civil/script.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::Value;

std::string units_of(const Value& v) {
  return v.is_string() && v.as_string() == "m" ? "m" : "ft";
}
double num_or(const Value& v, double d) {
  return nullish(v) ? d : num(v);
}

}  // namespace

void add_featureline_ops(std::map<std::string, ScriptOp, std::less<>>& m) {
  m["fl.clean"] = [](const Array& a, ScriptCtx& c) -> Value {
    return clean_feature(arg(a, 0), units_of(arg(a, 1)), *c.words);
  };
  m["fl.stations"] = [](const Array& a, ScriptCtx&) -> Value {
    Array out;
    for (const double s : stations(arg(a, 0))) {
      out.emplace_back(s);
    }
    return out;
  };
  m["fl.segments"] = [](const Array& a, ScriptCtx&) -> Value { return segments_value(segments(arg(a, 0))); };
  m["fl.setElevation"] = [](const Array& a, ScriptCtx&) -> Value {
    return set_elevation(arg(a, 0), arg(a, 1), arg(a, 2));
  };
  m["fl.setGrade"] = [](const Array& a, ScriptCtx&) -> Value { return set_grade(arg(a, 0), arg(a, 1), arg(a, 2)); };
  m["fl.raise"] = [](const Array& a, ScriptCtx&) -> Value { return raise(arg(a, 0), arg(a, 1)); };
  m["fl.evenGrade"] = [](const Array& a, ScriptCtx&) -> Value { return even_grade(arg(a, 0), arg(a, 1), arg(a, 2)); };
  m["fl.fromSurface"] = [](const Array& a, ScriptCtx& c) -> Value {
    const auto s = nullish(arg(a, 1)) ? nullptr : reg_get<Surface>(c, str(arg(a, 1)));
    return from_surface(arg(a, 0), s.get(), nullish(arg(a, 2)) ? Value(nullptr) : arg(a, 2));
  };
  m["fl.offset"] = [](const Array& a, ScriptCtx& c) -> Value {
    return offset_feature(arg(a, 0), arg(a, 1), arg(a, 2), *c.words);
  };
  m["fl.breaklines"] = [](const Array& a, ScriptCtx&) -> Value { return feature_breaklines(arg(a, 0), arg(a, 1)); };
  m["fl.item"] = [](const Array& a, ScriptCtx&) -> Value { return feature_item(arg(a, 0)); };
  m["fl.gradeLabel"] = [](const Array& a, ScriptCtx&) -> Value { return grade_label(arg(a, 0)); };
  m["fl.crossings"] = [](const Array& a, ScriptCtx& c) -> Value {
    const auto al = reg_get<Alignment>(c, str(arg(a, 0)));
    return crossings_value(alignment_crossings(al.get(), arg(a, 1)));
  };
  m["fl.runs"] = [](const Array& a, ScriptCtx& c) -> Value {
    const auto al = reg_get<Alignment>(c, str(arg(a, 0)));
    return runs_value(projected_runs(al.get(), arg(a, 1), num(arg(a, 2))));
  };
  m["fl.profile"] = [](const Array& a, ScriptCtx& c) -> Value {
    const auto al = reg_get<Alignment>(c, str(arg(a, 0)));
    return feature_profile(al.get(), arg(a, 1), num_or(field(arg(a, 2), "within"), 0));
  };
  m["fl.overlay"] = [](const Array& a, ScriptCtx&) -> Value {
    const Value t = arg(a, 1).truthy() ? arg(a, 1) : Value::object();
    const Value o = arg(a, 2);
    const Value skip = field(o, "skip");
    return grade_overlay(arg(a, 0), num_or(field(t, "k"), 1), num_or(field(t, "ox"), 0), num_or(field(t, "oy"), 0),
                         num_or(field(o, "minSeg"), 60), num_or(field(o, "minGap"), 34),
                         skip.is_undefined() ? Value(nullptr) : skip);
  };
}

}  // namespace cc::civil
