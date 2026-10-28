// SPDX-License-Identifier: Apache-2.0
// Script ops for grading.js, corridor.js and superelev.js (tools/parity/civil/ops-civil.mjs has their JS twins).
#include <cmath>
#include <memory>

#include "cc/civil/design.hpp"
#include "cc/civil/script.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::make_object;
using base::Value;

Value surface_view_or_null(const std::shared_ptr<Surface>& s) {
  return s ? surface_view(*s) : Value(nullptr);
}

const Surface& surface_arg(const ScriptCtx& c, const Value& name) {
  const auto s = reg_get<Surface>(c, str(name));
  if (!s) {
    throw ScriptError{"Cannot read properties of undefined (reading 'elevationAt')"};
  }
  return *s;
}

}  // namespace

void add_design_ops(std::map<std::string, ScriptOp, std::less<>>& m) {
  m["grading.castSlope"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Surface& s = surface_arg(c, arg(a, 0));
    const Value o = arg(a, 7);
    const Value rate = field(o, "rate");
    const Value step = field(o, "step");
    const Value far = field(o, "far");
    const Value p0 = arg(a, 1);
    return cast_value(cast_slope(s, {num(field(p0, "x")), num(field(p0, "y"))}, num(arg(a, 2)), num(arg(a, 3)),
                                 num(arg(a, 4)), num(arg(a, 5)), num(arg(a, 6)), rate.is_undefined() ? 1 : num(rate),
                                 step.is_undefined() ? 0.25 : num(step), far.is_undefined() ? 1e4 : num(far)));
  };
  m["grading.daylightPad"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value o = arg(a, 2);
    return daylight_pad(surface_arg(c, arg(a, 0)), arg(a, 1), nullish(o) ? Value::object() : o, *c.words);
  };
  m["grading.gridVolume"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value o = arg(a, 2);
    const GridVolume g =
        grid_volume(surface_arg(c, arg(a, 0)), surface_arg(c, arg(a, 1)), nullish(o) ? Value::object() : o);
    return make_object(
        {{"cut", g.cut}, {"fill", g.fill}, {"net", g.net}, {"cells", g.cells}, {"cell", g.cell}, {"area", g.area}});
  };
  m["grading.selfCrosses"] = [](const Array& a, ScriptCtx&) -> Value {
    std::vector<P2> pts;
    for (const Value& p : arr(arg(a, 0))) {
      pts.push_back({num(field(p, "x")), num(field(p, "y"))});
    }
    return self_crosses(pts);
  };
  m["grading.gradedSurface"] = [](const Array& a, ScriptCtx& c) -> Value {
    auto base = reg_get<Surface>(c, str(arg(a, 0)));
    if (!base) {
      throw ScriptError{"Cannot read properties of undefined"};
    }
    std::vector<GradeItem> items;
    for (const Value& it : arr(arg(a, 3))) {
      const Value cor = field(it, "corridor");
      if (it.truthy() && cor.truthy()) {
        GradeItem g;
        g.name = str(field(it, "name"));
        const auto al = reg_get<Alignment>(c, str(field(cor, "alignment")));
        const auto pr = reg_get<Profile>(c, str(field(cor, "profile")));
        const Value asm_ = field(cor, "asm");
        const i18n::Catalog* words = c.words;
        g.patch = [al, pr, asm_, words](const Surface& sf) {
          return corridor_patch(&sf, al.get(), pr.get(), asm_, *words);
        };
        items.push_back(std::move(g));
      } else {
        items.push_back({it, {}, {}});
      }
    }
    const Value pts = arg(a, 1);
    const Value bls = arg(a, 2);
    const i18n::Catalog* w = c.words;
    Graded g =
        graded_surface(base, pts.truthy() ? pts : Value::array(), bls.truthy() ? bls : Value::array(), items, *c.words,
                       [w](const Value& p, const Value& b) { return build_surface(build_input_from(p, b), *w); });
    if (g.surface) {
      c.keep(g.surface);
    }
    return make_object({{"surface", surface_view_or_null(g.surface)},
                        {"report", g.report},
                        {"regions", g.regions},
                        {"items", g.items}});
  };

  m["corridor.defaultAssembly"] = [](const Array& a, ScriptCtx&) -> Value {
    return default_assembly(str(arg(a, 0)) == "m" && arg(a, 0).is_string() ? "m" : "ft");
  };
  m["corridor.patch"] = [](const Array& a, ScriptCtx& c) -> Value {
    const auto s = reg_get<Surface>(c, str(arg(a, 0)));
    const auto al = nullish(arg(a, 1)) ? nullptr : reg_get<Alignment>(c, str(arg(a, 1)));
    const auto pr = nullish(arg(a, 2)) ? nullptr : reg_get<Profile>(c, str(arg(a, 2)));
    return corridor_patch(s.get(), al.get(), pr.get(), arg(a, 3), *c.words);
  };

  m["super.defaults"] = [](const Array& a, ScriptCtx&) -> Value {
    return super_defaults(arg(a, 0).is_string() && arg(a, 0).as_string() == "m" ? "m" : "ft");
  };
  m["super.clean"] = [](const Array& a, ScriptCtx&) -> Value {
    return clean_super(arg(a, 0), arg(a, 1).is_string() && arg(a, 1).as_string() == "m" ? "m" : "ft");
  };
  m["super.rateFor"] = [](const Array& a, ScriptCtx&) -> Value {
    const double R = num(arg(a, 0));
    const Array& rate = arr(arg(a, 1));
    auto item = [](const Value& row, std::size_t k) {
      return row.is_array() && row.as_array().size() > k ? row.as_array()[k] : Value();
    };
    for (const Value& row : rate) {
      if (R >= num(item(row, 0))) {
        return item(row, 1);
      }
    }
    return rate.empty() ? Value(nullptr) : item(rate.back(), 1);
  };
  m["super.plan"] = [](const Array& a, ScriptCtx& c) -> Value {
    const auto al = reg_get<Alignment>(c, str(arg(a, 0)));
    const Value o = arg(a, 1);
    const Value nc = field(o, "nc");
    const Value lane = field(o, "lane");
    auto plan = std::make_shared<SuperPlan>(super_plan(al.get(), field(o, "super"), nc.is_undefined() ? 0.02 : num(nc),
                                                       lane.is_undefined() ? 12 : num(lane), *c.words));
    c.keep(plan);
    return super_plan_value(*plan);
  };
  m["super.slopesAt"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value nc = arg(a, 1);
    const SideSlopes s = slopes_at(self_as<SuperPlan>(c), num(arg(a, 0)), nullish(nc) ? 0.02 : num(nc));
    return make_object({{"L", s.L}, {"R", s.R}});
  };
  m["super.keyStations"] = [](const Array&, ScriptCtx& c) -> Value {
    Array out;
    for (const double v : super_key_stations(self_as<SuperPlan>(c))) {
      out.emplace_back(v);
    }
    return out;
  };
}

}  // namespace cc::civil
