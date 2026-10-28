// SPDX-License-Identifier: Apache-2.0
// The driver's configs.*, gdt.*, bom.*, threads.*, spline.* and i18n.t ops.
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/partcraft/bom.hpp"
#include "cc/partcraft/configs.hpp"
#include "cc/partcraft/font.hpp"
#include "cc/partcraft/form.hpp"
#include "cc/partcraft/gdt.hpp"
#include "cc/partcraft/spline.hpp"
#include "cc/partcraft/threads.hpp"
#include "jsx.hpp"
#include "ops.hpp"

namespace cc::partcraft::ops {
namespace {

using base::Value;

template <class T>
T ok_or_throw(base::Result<T> r) {
  if (!r) {
    throw JsError(r.error().message);
  }
  return std::move(*r);
}

double number(const Value& v) {
  return base::js_to_number(v);
}

/// A count argument (segments, points per span): the JS loops that many times, and runs out of memory or time on a
/// huge one; the fork refuses one over 100 000 (docs/gaps.md, hostile input only).
double count(const Value& v, double def) {
  const double n = v.is_undefined() ? def : base::js_to_number(v);
  if (n > 1e5) {
    throw JsError("a count over 100000 (the fork's cap)");
  }
  return n;
}

Value dims_value(const threads::Dims& g) {
  return base::make_object({{"d", g.d}, {"p", g.p}, {"d1", g.d1}, {"d2", g.d2}, {"depth", g.depth}});
}
Value corners_value(const threads::Corners& c) {
  base::Array a;
  for (const auto& [u, r] : c) {
    a.emplace_back(base::Array{u, r});
  }
  return a;
}
threads::Corners corners_of(const Value& v) {
  threads::Corners c;
  for (const auto& p : spline::points_of(v)) {
    c.push_back(p);
  }
  if (c.empty()) {
    throw JsError("Cannot read properties of undefined (reading '0')");
  }
  return c;
}

spline::FitOptions fit_options(const Value& o) {
  spline::FitOptions f;
  f.closed = jsx::get(o, "closed").truthy();
  for (const auto& [key, slot] : {std::pair{"a0", &f.a0}, std::pair{"a1", &f.a1}}) {
    const Value v = jsx::get(o, key);
    if (v.is_number()) {
      *slot = v.as_number();
    }
  }
  return f;
}

}  // namespace

void add_modules(Table& t, const i18n::Catalog& words) {
  const i18n::Catalog* w = &words;
  // ---- configs.js
  t["configs.read"] = [w](Args a) { return ok_or_throw(configs::read_configs(arg(a, 0), arg(a, 1), *w)); };
  t["configs.rowFromCurrent"] = [w](Args a) { return configs::row_from_current(arg(a, 0), arg(a, 1), *w); };
  t["configs.saveRow"] = [w](Args a) { return ok_or_throw(configs::save_row(arg(a, 0), arg(a, 1), *w)); };
  t["configs.applyRow"] = [w](Args a) { return ok_or_throw(configs::apply_row(arg(a, 0), arg(a, 1), *w)); };
  t["configs.rowMatches"] = [w](Args a) { return Value(configs::row_matches(arg(a, 0), arg(a, 1), *w)); };
  t["configs.renameRow"] = [w](Args a) {
    return ok_or_throw(configs::rename_row(arg(a, 0), arg(a, 1), arg(a, 2), *w));
  };
  t["configs.deleteRow"] = [](Args a) { return configs::delete_row(arg(a, 0), arg(a, 1)); };
  t["configs.followParams"] = [](Args a) { return configs::follow_params(arg(a, 0), arg(a, 1), arg(a, 2)); };
  t["configs.rowsFromTable"] = [w](Args a) {
    return ok_or_throw(configs::rows_from_table(base::js_string(arg(a, 0)), arg(a, 1), *w));
  };
  t["configs.csv"] = [](Args a) { return Value(configs::configs_csv(arg(a, 0))); };
  // ---- gdt.js
  t["gdt.readNotes"] = [w](Args a) { return ok_or_throw(gdt::read_notes(arg(a, 0), *w)); };
  t["gdt.validate"] = [w](Args a) {
    const auto r = gdt::validate(arg(a, 0), *w);
    return r ? Value(*r) : Value(nullptr);
  };
  t["gdt.describe"] = [w](Args a) { return Value(ok_or_throw(gdt::describe(arg(a, 0), *w))); };
  t["gdt.newAnnotation"] = [w](Args a) {
    const Value props = arg(a, 1).is_undefined() || arg(a, 1).is_null() ? Value::object() : arg(a, 1);
    return ok_or_throw(gdt::new_annotation(arg(a, 0), props, *w));
  };
  t["gdt.geometry"] = [w](Args a) {
    const auto anchor = spline::points_of(base::Array{arg(a, 1)});
    return ok_or_throw(gdt::annotation_geometry(arg(a, 0), anchor.at(0), *w));
  };
  t["gdt.textW"] = [](Args a) { return Value(font::advance(base::js_string(arg(a, 0))) / 1000 * number(arg(a, 1))); };
  // ---- bom.js
  t["bom.readMaterial"] = [w](Args a) { return ok_or_throw(bom::read_material(arg(a, 0), *w)); };
  t["bom.readPartProps"] = [w](Args a) { return ok_or_throw(bom::read_part_props(arg(a, 0), *w)); };
  t["bom.presetOf"] = [w](Args a) { return Value(bom::preset_of(arg(a, 0), *w)); };
  t["bom.materials"] = [w](Args) {
    base::Array out;
    for (const auto& m : bom::materials(*w)) {
      out.emplace_back(base::make_object({{"name", m.name}, {"density", m.density}}));
    }
    return Value(std::move(out));
  };
  t["bom.rows"] = [](Args a) {
    std::vector<std::pair<std::string, Value>> stats;
    for (const auto& [k, v] : jsx::entries(arg(a, 1))) {
      if (v.truthy()) {
        stats.emplace_back(k, v);
      }
    }
    return bom::bom_rows(arg(a, 0), stats);
  };
  t["bom.csv"] = [](Args a) { return Value(bom::bom_csv(arg(a, 0))); };
  t["bom.csvCell"] = [](Args a) {
    const Value text = jsx::get(arg(a, 1), "text");
    return Value(bom::csv_cell(arg(a, 0), text.is_undefined() || text.truthy()));
  };
  // ---- threads.js
  t["threads.dims"] = [](Args a) {
    const auto g = ok_or_throw(threads::thread_dims(base::js_string(arg(a, 0)), base::js_string(arg(a, 1))));
    return g ? dims_value(*g) : Value(nullptr);
  };
  t["threads.sizeFor"] = [](Args a) {
    const auto s = threads::size_for(base::js_string(arg(a, 0)), number(arg(a, 1)), arg(a, 2).truthy());
    return s ? Value(*s) : Value(nullptr);
  };
  t["threads.profileCorners"] = [](Args a) {
    const Value shift = arg(a, 2);
    const auto c = ok_or_throw(threads::profile_corners(base::js_string(arg(a, 0)), base::js_string(arg(a, 1)),
                                                        shift.is_undefined() ? 0 : number(shift)));
    return c ? corners_value(*c) : Value(nullptr);
  };
  t["threads.profileAt"] = [](Args a) { return Value(threads::profile_at(corners_of(arg(a, 0)), number(arg(a, 1)))); };
  t["threads.boltSection"] = [](Args a) {
    const Value o = arg(a, 1);
    const Value seg = jsx::get(o, "segments");
    return spline::points_value(
        threads::bolt_section(corners_of(arg(a, 0)), jsx::get(o, "hand") == Value("left"), count(seg, 96)));
  };
  t["threads.callout"] = [w](Args a) {
    const Value o = arg(a, 2);
    const threads::CalloutOptions co{.external = jsx::get(o, "external").truthy(),
                                     .length = number(jsx::get(o, "length")),
                                     .through = jsx::get(o, "through").truthy(),
                                     .modelled = jsx::get(o, "modelled").truthy()};
    return Value(ok_or_throw(threads::thread_callout(base::js_string(arg(a, 0)), base::js_string(arg(a, 1)), co, *w)));
  };
  t["threads.sizeLabel"] = [](Args a) {
    return Value(threads::size_label(base::js_string(arg(a, 0)), base::js_string(arg(a, 1))));
  };
  // ---- sketch-spline.js
  t["spline.interpFit"] = [w](Args a) {
    return spline::to_value(ok_or_throw(spline::interp_fit(spline::points_of(arg(a, 0)), fit_options(arg(a, 1)), *w)));
  };
  t["spline.controlCurve"] = [](Args a) {
    const Value o = arg(a, 1);
    const Value k = jsx::get(o, "knots");
    std::optional<std::vector<double>> knots;
    if (k.truthy() && k.is_array()) {
      knots.emplace();
      for (const Value& x : k.as_array()) {
        knots->push_back(number(x));
      }
    }
    return spline::to_value(spline::control_curve(spline::points_of(arg(a, 0)), jsx::get(o, "closed").truthy(), knots));
  };
  t["spline.tessellate"] = [](Args a) {
    const Value n = arg(a, 1);
    return spline::points_value(ok_or_throw(spline::tessellate(spline::from_value(arg(a, 0)), count(n, 16))));
  };
  t["spline.tessellateTol"] = [](Args a) {
    return spline::points_value(ok_or_throw(spline::tessellate_tol(spline::from_value(arg(a, 0)), number(arg(a, 1)))));
  };
  t["spline.areaTerm"] = [](Args a) { return Value(ok_or_throw(spline::area_term(spline::from_value(arg(a, 0))))); };
  t["spline.arcLength"] = [](Args a) { return Value(ok_or_throw(spline::arc_length(spline::from_value(arg(a, 0))))); };
  t["spline.closestParam"] = [](Args a) {
    const auto p = spline::points_of(base::Array{arg(a, 1)});
    const Value n = arg(a, 2);
    return Value(ok_or_throw(spline::closest_param(spline::from_value(arg(a, 0)), p.at(0), count(n, 12))));
  };
  t["spline.curvatureAt"] = [](Args a) {
    return Value(ok_or_throw(spline::curvature_at(spline::from_value(arg(a, 0)), number(arg(a, 1)))));
  };
  t["spline.derivsAt"] = [](Args a) {
    const Value n = arg(a, 2);
    const double k0 = n.is_undefined() ? 1 : number(n);
    const double k = std::isnan(k0) ? -1 : k0;  // NaN derivatives: none (the JS's loop never runs)
    return spline::points_value(ok_or_throw(spline::derivs_at(spline::from_value(arg(a, 0)), number(arg(a, 1)),
                                                              static_cast<int>(std::clamp(k, -1.0, 64.0)))));
  };
  t["spline.pointAt"] = [](Args a) {
    const auto p = ok_or_throw(spline::point_at(spline::from_value(arg(a, 0)), number(arg(a, 1))));
    return Value(base::Array{p[0], p[1]});
  };
  t["spline.insertKnot"] = [](Args a) {
    return spline::to_value(ok_or_throw(spline::insert_knot(spline::from_value(arg(a, 0)), number(arg(a, 1)))));
  };
  t["spline.segment"] = [](Args a) {
    return spline::to_value(
        ok_or_throw(spline::segment(spline::from_value(arg(a, 0)), number(arg(a, 1)), number(arg(a, 2)))));
  };
  t["spline.reverse"] = [](Args a) { return spline::to_value(spline::reverse(spline::from_value(arg(a, 0)))); };
  t["spline.normalizeKnots"] = [](Args a) {
    return spline::to_value(spline::normalize_knots(spline::from_value(arg(a, 0))));
  };
  t["spline.knotProblem"] = [w](Args a) {
    const auto r = spline::knot_problem(arg(a, 0), number(arg(a, 1)), *w);
    return r ? Value(*r) : Value(nullptr);
  };
  t["spline.offsetFit"] = [w](Args a) {
    const auto r =
        ok_or_throw(spline::offset_fit(spline::from_value(arg(a, 0)), number(arg(a, 1)), number(arg(a, 2)), *w));
    return base::make_object({{"Q", spline::points_value(r.Q)},
                              {"a0", r.a0 ? Value(*r.a0) : Value(nullptr)},
                              {"a1", r.a1 ? Value(*r.a1) : Value(nullptr)},
                              {"closed", r.closed},
                              {"err", r.err},
                              {"curve", spline::to_value(r.curve)}});
  };
  // ---- form.js
  t["form.read"] = [w](Args a) { return ok_or_throw(form::read_form(arg(a, 0), *w)); };
  t["form.box"] = [](Args a) {
    const auto n = [&](std::size_t i, double def) { return arg(a, i).is_undefined() ? def : number(arg(a, i)); };
    return form::box(n(0, 1), n(1, 1), n(2, 1), n(3, 100), n(4, 100), n(5, 100));
  };
  // ---- i18n.js
  t["i18n.t"] = [w](Args a) { return Value(w->t(base::js_string(arg(a, 0)))); };
}

}  // namespace cc::partcraft::ops
