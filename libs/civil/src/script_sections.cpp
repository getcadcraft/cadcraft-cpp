// SPDX-License-Identifier: Apache-2.0
// Script ops for sections.js (tools/parity/civil/ops-civil.mjs has their JS twins).
#include <cmath>
#include <memory>

#include "cc/civil/script.hpp"
#include "cc/civil/sections.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::make_object;
using base::Value;

std::string units_arg(const Value& v) {
  return v.is_string() && v.as_string() == "m" ? "m" : "ft";
}
std::optional<double> finite_or_none(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number()) ? std::optional(v.as_number()) : std::nullopt;
}
std::vector<std::vector<P2>> polys(const Value& v) {
  std::vector<std::vector<P2>> out;
  for (const Value& r : arr(v)) {
    std::vector<P2> poly;
    for (const Value& p : arr(r)) {
      poly.push_back({num(field(p, "x")), num(field(p, "y"))});
    }
    out.push_back(std::move(poly));
  }
  return out;
}

}  // namespace

void add_sections_ops(std::map<std::string, ScriptOp, std::less<>>& m) {
  m["sec.defaults"] = [](const Array& a, ScriptCtx&) -> Value {
    Value d = sample_defaults(units_arg(arg(a, 0)));
    d.set("extra", Array{});
    return d;
  };
  m["sec.clean"] = [](const Array& a, ScriptCtx&) -> Value { return clean_sampling(arg(a, 0), units_arg(arg(a, 1))); };
  m["sec.stations"] = [](const Array& a, ScriptCtx& c) -> Value {
    const auto al = reg_get<Alignment>(c, str(arg(a, 0)));
    const Value o = arg(a, 1);
    std::vector<double> extra;
    for (const Value& e : arr(field(o, "extra"))) {
      extra.push_back(num(e));
    }
    Array out;
    for (const double s : sample_stations(al.get(), num(field(o, "step")), extra, finite_or_none(field(o, "from")),
                                          finite_or_none(field(o, "to")))) {
      out.emplace_back(s);
    }
    return out;
  };
  m["sec.line"] = [](const Array& a, ScriptCtx& c) -> Value {
    const auto al = reg_get<Alignment>(c, str(arg(a, 0)));
    if (!al) {
      throw ScriptError{"Cannot read properties of undefined (reading 'pointAt')"};
    }
    const auto l = sample_line(*al, num(arg(a, 1)), arg(a, 2), arg(a, 3));
    if (!l) {
      throw ScriptError{"Cannot read properties of null (reading 'dir')"};
    }
    return line_value(*l);
  };
  m["sec.surfaceSection"] = [](const Array& a, ScriptCtx& c) -> Value {
    const auto s = nullish(arg(a, 0)) ? nullptr : reg_get<Surface>(c, str(arg(a, 0)));
    const Value pa = arg(a, 1);
    const Value pb = arg(a, 2);
    return runs_value(surface_section(s.get(), {num(field(pa, "x")), num(field(pa, "y"))},
                                      {num(field(pb, "x")), num(field(pb, "y"))}, num(arg(a, 3)), num(arg(a, 4))));
  };
  m["sec.zAt"] = [](const Array& a, ScriptCtx&) -> Value {
    const auto z = z_at(runs_from(arg(a, 0)), num(arg(a, 1)));
    return z ? Value(*z) : Value(nullptr);
  };
  m["sec.cutFill"] = [](const Array& a, ScriptCtx&) -> Value {
    const CutFill cf = cut_fill(runs_from(arg(a, 0)), runs_from(arg(a, 1)));
    Array parts;
    for (const Part& p : cf.parts) {
      parts.push_back(make_object({{"o0", p.o0}, {"o1", p.o1}, {"kind", p.cut ? "cut" : "fill"}}));
    }
    return make_object({{"cut", cf.cut}, {"fill", cf.fill}, {"parts", std::move(parts)}});
  };
  m["sec.featureMarks"] = [](const Array& a, ScriptCtx&) -> Value {
    return feature_marks(arg(a, 0), line_from(arg(a, 1)));
  };
  m["sec.regionSpans"] = [](const Array& a, ScriptCtx&) -> Value {
    Array out;
    for (const auto& [o0, o1] : region_spans(line_from(arg(a, 0)), polys(arg(a, 1)))) {
      out.emplace_back(Array{Value(o0), Value(o1)});
    }
    return out;
  };
  m["sec.clipRuns"] = [](const Array& a, ScriptCtx&) -> Value {
    std::vector<std::pair<double, double>> spans;
    for (const Value& s : arr(arg(a, 1))) {
      const Array& p = arr(s);
      spans.emplace_back(p.empty() ? std::nan("") : num(p[0]), p.size() < 2 ? std::nan("") : num(p[1]));
    }
    return runs_value(clip_runs(runs_from(arg(a, 0)), spans));
  };
  m["sec.build"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value o = arg(a, 0);
    const auto al = reg_get<Alignment>(c, str(field(o, "alignment")));
    const auto g = nullish(field(o, "ground")) ? nullptr : reg_get<Surface>(c, str(field(o, "ground")));
    const auto d = nullish(field(o, "design")) ? nullptr : reg_get<Surface>(c, str(field(o, "design")));
    SectionInput in;
    in.alignment = al.get();
    in.ground = g.get();
    in.design = d.get();
    if (field(o, "regions").truthy()) {
      in.regions = polys(field(o, "regions"));
    }
    in.corridor = nullish(field(o, "corridor")) ? Value(nullptr) : field(o, "corridor");
    in.sampling = nullish(field(o, "sampling")) ? Value(nullptr) : field(o, "sampling");
    in.from = finite_or_none(field(o, "from"));
    in.to = finite_or_none(field(o, "to"));
    auto set = std::make_shared<Value>(build_sections(in));
    c.keep(set);
    return *set;
  };
  m["sec.csv"] = [](const Array& a, ScriptCtx& c) -> Value {
    return sections_csv(self_as<Value>(c), units_arg(field(arg(a, 0), "units")), *c.words);
  };
}

}  // namespace cc::civil
