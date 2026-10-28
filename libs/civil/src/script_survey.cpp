// SPDX-License-Identifier: Apache-2.0
// Script ops for masshaul.js, solve-why.js and staking.js (tools/parity/civil/ops-civil.mjs has their JS twins).
#include <cmath>
#include <memory>

#include "cc/civil/script.hpp"
#include "cc/civil/survey.hpp"

namespace cc::civil {
namespace {

ZAt z_source(const ScriptCtx& c, const base::Value& v) {
  const base::Value prof = field(v, "profile");
  if (prof.truthy()) {
    if (auto p = reg_get<Profile>(c, str(prof)); p) {
      return [p](double sta, double, double, double) { return p->elevation_at(sta); };
    }
  }
  const base::Value surf = field(v, "surface");
  if (surf.truthy()) {
    if (auto s = reg_get<Surface>(c, str(surf)); s) {
      return [s](double, double, double x, double y) { return s->elevation_at(x, y); };
    }
  }
  const base::Value flat = field(v, "flat");
  if (flat.is_number()) {
    const double f = flat.as_number();
    return [f](double, double, double, double) { return std::optional(f); };
  }
  return [](double, double, double, double) { return std::optional<double>(); };
}

using base::Array;
using base::Value;

const Alignment& alignment_arg(const ScriptCtx& c, const Value& name) {
  const auto al = reg_get<Alignment>(c, str(name));
  if (!al) {
    throw ScriptError{"Cannot read properties of undefined (reading 'start')"};
  }
  return *al;
}

}  // namespace

void add_survey_ops(std::map<std::string, ScriptOp, std::less<>>& m) {
  m["mh.massHaul"] = [](const Array& a, ScriptCtx& c) -> Value {
    auto r = std::make_shared<Value>(
        mass_haul(arg(a, 0), field(arg(a, 1), "factor").is_undefined() ? Value(1) : field(arg(a, 1), "factor")));
    c.keep(r);
    return *r;
  };
  m["mh.csv"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value u = field(arg(a, 0), "units");
    return mass_haul_csv(self_as<Value>(c), u.is_undefined() ? "ft" : str(u), *c.words);
  };
  m["why.curve"] = [](const Array& a, ScriptCtx&) -> Value { return curve_why(arg(a, 0), arg(a, 1)); };
  m["why.maxRadius"] = [](const Array& a, ScriptCtx&) -> Value { return max_radius(arg(a, 0), arg(a, 1)); };
  m["why.vertical"] = [](const Array& a, ScriptCtx&) -> Value { return vertical_why(arg(a, 0)); };

  m["sk.parseOffsets"] = [](const Array& a, ScriptCtx&) -> Value { return parse_offsets(arg(a, 0)); };
  m["sk.cutFillText"] = [](const Array& a, ScriptCtx&) -> Value { return cut_fill_text(arg(a, 0)); };
  m["sk.offsetText"] = [](const Array& a, ScriptCtx&) -> Value { return offset_text(num(arg(a, 0))); };
  m["sk.csv"] = [](const Array& a, ScriptCtx& c) -> Value { return staking_csv(arg(a, 0), arg(a, 1), *c.words); };
  m["sk.stations"] = [](const Array& a, ScriptCtx& c) -> Value {
    Array out;
    for (const double s : stake_stations(alignment_arg(c, arg(a, 0)), arg(a, 1))) {
      out.emplace_back(s);
    }
    return out;
  };
  m["sk.rows"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value o = arg(a, 1);
    const ZAt d = z_source(c, field(o, "design"));
    const ZAt g = z_source(c, field(o, "ground"));
    return stake_rows(alignment_arg(c, arg(a, 0)), o, d, g);
  };
  m["sk.pointRows"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value o = arg(a, 2).truthy() ? arg(a, 2) : Value::object();
    const ZAt d = z_source(c, field(o, "design"));
    const bool has_ground = field(o, "ground").truthy();
    const ZAt g = has_ground ? z_source(c, field(o, "ground")) : ZAt{};
    return point_rows(alignment_arg(c, arg(a, 0)), arg(a, 1), d, has_ground ? &g : nullptr);
  };
}

}  // namespace cc::civil
