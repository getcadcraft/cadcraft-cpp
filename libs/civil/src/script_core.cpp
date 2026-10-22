// SPDX-License-Identifier: Apache-2.0
// Script ops for units.js, points.js and tin.js (tools/parity/civil/driver.mjs has their JS twins).
#include <algorithm>
#include <cmath>
#include <optional>

#include "cc/base/js.hpp"
#include "cc/civil/points.hpp"
#include "cc/civil/script.hpp"
#include "cc/civil/tin.hpp"
#include "cc/civil/units.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::make_object;
using base::Value;

std::optional<double> opt_num(const Value& v) {  // `x ?? undefined` for a number argument
  return nullish(v) ? std::nullopt : std::optional(num(v));
}
std::string unit_arg(const Value& v) {
  return nullish(v) ? std::string("ft") : str(v);
}
Value nums(const std::vector<double>& v) {
  Array a;
  a.reserve(v.size());
  for (const double x : v) {
    a.emplace_back(x);
  }
  return a;
}
Value ints(const std::vector<std::int32_t>& v) {
  Array a;
  a.reserve(v.size());
  for (const std::int32_t x : v) {
    a.emplace_back(x);
  }
  return a;
}
Value strs(const std::vector<std::string>& v) {
  Array a;
  for (const auto& s : v) {
    a.emplace_back(s);
  }
  return a;
}
Value p2s(const std::vector<P2>& v) {
  Array a;
  a.reserve(v.size());
  for (const P2& p : v) {
    a.push_back(base::pt2(p.x, p.y));
  }
  return a;
}
std::vector<P2> to_p2(const Value& v) {
  std::vector<P2> out;
  for (const Value& p : arr(v)) {
    out.push_back({num(field(p, "x")), num(field(p, "y"))});
  }
  return out;
}

// ---- points
SurveyPoint to_point(const Value& p) {
  SurveyPoint s;
  s.n = str(field(p, "n"));
  s.x = num(field(p, "x"));
  s.y = num(field(p, "y"));
  const Value z = field(p, "z");
  if (!z.is_null()) {
    s.z = num(z);
  }
  const Value d = field(p, "d");
  s.d = d.truthy() ? str(d) : std::string();
  return s;
}
std::vector<SurveyPoint> to_points(const Value& v) {
  std::vector<SurveyPoint> out;
  for (const Value& p : arr(v)) {
    out.push_back(to_point(p));
  }
  return out;
}
Value point_value(const SurveyPoint& p) {
  return make_object({{"n", p.n}, {"x", p.x}, {"y", p.y}, {"z", p.z ? Value(*p.z) : Value(nullptr)}, {"d", p.d}});
}

Value parsed_value(const ParsedPoints& r) {
  Array pts;
  for (const SurveyPoint& p : r.points) {
    pts.push_back(point_value(p));
  }
  Array bad;
  for (const BadRow& b : r.bad) {
    bad.push_back(make_object({{"line", b.line}, {"text", b.text}, {"why", b.why}}));
  }
  Value o = make_object({{"points", std::move(pts)},
                         {"format", r.format ? Value(*r.format) : Value(nullptr)},
                         {"formatFrom", r.format_from},
                         {"delimiter", r.delimiter ? Value(*r.delimiter) : Value(nullptr)},
                         {"header", r.header ? strs(*r.header) : Value(nullptr)},
                         {"bad", std::move(bad)},
                         {"badCount", r.bad_count},
                         {"duplicates", r.duplicates}});
  if (r.latlon) {
    o.set("latlon", true);
  }
  return o;
}

// ---- tin
Value report_value(const TinReport& r, bool faces) {
  Value o = make_object({{"notes", strs(r.notes)}, {"errors", strs(r.errors)}});
  if (!faces) {
    o.set("skippedNoZ", r.skipped_no_z);
    o.set("merged", r.merged);
    o.set("crossings", r.crossings);
    o.set("zConflicts", r.z_conflicts);
    o.set("constraintFailures", r.constraint_failures);
    if (r.breakline_segments) {
      o.set("breaklineSegments", *r.breakline_segments);
    }
  }
  return o;
}
Value surf_view(const Surface& s) {
  Array ce;
  for (const auto& [a, b] : s.constrained_edges) {
    ce.emplace_back(Array{Value(a), Value(b)});
  }
  return make_object({{"X", nums(s.X())},
                      {"Y", nums(s.Y())},
                      {"Z", nums(s.Z())},
                      {"tris", ints(s.tris())},
                      {"ox", s.ox()},
                      {"oy", s.oy()},
                      {"nT", static_cast<double>(s.triangle_count())},
                      {"constrainedEdges", std::move(ce)}});
}
Value built_value(const Built& b, bool faces) {
  return make_object(
      {{"surface", b.surface ? surf_view(*b.surface) : Value(nullptr)}, {"report", report_value(b.report, faces)}});
}
const Surface& self_surface(ScriptCtx& c) {
  if (c.self == nullptr || !std::holds_alternative<std::shared_ptr<Surface>>(*c.self)) {
    throw ScriptError{"not a surface"};
  }
  return *std::get<std::shared_ptr<Surface>>(*c.self);
}
Value stats_value(const SurfaceStats& s) {
  return make_object(
      {{"points", s.points},
       {"triangles", s.triangles},
       {"minZ", s.min_z},
       {"maxZ", s.max_z},
       {"meanZ", s.mean_z},
       {"area2d", s.area2d},
       {"area3d", s.area3d},
       {"bounds", make_object({{"minX", s.min_x}, {"minY", s.min_y}, {"maxX", s.max_x}, {"maxY", s.max_y}})}});
}

BuildInput to_build(const Value& a) {
  BuildInput in;
  for (const Value& p : arr(field(a, "points"))) {
    TinPoint t{num(field(p, "x")), num(field(p, "y")), std::nullopt};
    const Value z = field(p, "z");
    if (z.is_number() && std::isfinite(z.as_number())) {
      t.z = z.as_number();
    }
    in.points.push_back(t);
  }
  for (const Value& b : arr(field(a, "breaklines"))) {
    Breakline bl;
    for (const Value& p : arr(field(b, "pts"))) {
      bl.pts.push_back({num(field(p, "x")), num(field(p, "y")), num(field(p, "z"))});
    }
    in.breaklines.push_back(std::move(bl));
  }
  const Value bnd = field(a, "boundary");
  if (bnd.truthy()) {
    in.boundary = to_p2(bnd);
  }
  const Value me = field(a, "maxEdge");
  in.max_edge = me.is_undefined() ? 0 : num(me);
  return in;
}

}  // namespace

void add_core_ops(std::map<std::string, ScriptOp, std::less<>>& m) {
  // ---- units.js
  m["units.formatStation"] = [](const Array& a, ScriptCtx&) -> Value {
    return format_station(num(arg(a, 0)), unit_arg(arg(a, 1)), opt_num(arg(a, 2)));
  };
  m["units.parseStation"] = [](const Array& a, ScriptCtx&) -> Value {
    const Value s = arg(a, 0);
    const auto r = parse_station(nullish(s) ? std::string() : str(s), unit_arg(arg(a, 1)));
    return r ? Value(*r) : Value(nullptr);
  };
  m["units.isNum"] = [](const Array& a, ScriptCtx&) -> Value {
    const Value s = arg(a, 0);
    if (s.is_number()) {
      return std::isfinite(s.as_number());
    }
    return is_num(str(s));
  };
  m["units.fixed"] = [](const Array& a, ScriptCtx&) -> Value {
    return fixed(num(arg(a, 0)), opt_num(arg(a, 1)).value_or(2));
  };
  m["units.grouped"] = [](const Array& a, ScriptCtx&) -> Value {
    return grouped(num(arg(a, 0)), opt_num(arg(a, 1)).value_or(0));
  };
  m["units.pct"] = [](const Array& a, ScriptCtx&) -> Value {
    return pct(num(arg(a, 0)), opt_num(arg(a, 1)).value_or(2));
  };
  m["units.volumeText"] = [](const Array& a, ScriptCtx&) -> Value {
    return volume_text(num(arg(a, 0)), unit_arg(arg(a, 1)), opt_num(arg(a, 2)).value_or(1));
  };
  m["units.areaText"] = [](const Array& a, ScriptCtx&) -> Value {
    return area_text(num(arg(a, 0)), unit_arg(arg(a, 1)), opt_num(arg(a, 2)).value_or(0));
  };
  m["units.bearing"] = [](const Array& a, ScriptCtx&) -> Value { return bearing(num(arg(a, 0))); };
  m["units.dms"] = [](const Array& a, ScriptCtx&) -> Value { return dms(num(arg(a, 0))); };

  // ---- points.js
  m["points.parsePoints"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value t = arg(a, 0);
    const Value o = arg(a, 1);
    const Value f = field(o, "format");
    const Value p = field(o, "prefer");
    return parsed_value(parse_points(nullish(t) ? std::string() : str(t), f.is_undefined() ? "auto" : str(f),
                                     p.is_undefined() ? "PNEZD" : str(p), *c.words));
  };
  m["points.groupKey"] = [](const Array& a, ScriptCtx&) -> Value {
    const Value d = arg(a, 0);
    return group_key(d.truthy() ? str(d) : std::string());
  };
  m["points.pointGroups"] = [](const Array& a, ScriptCtx&) -> Value {
    Array out;
    for (const GroupCount& g : point_groups(to_points(arg(a, 0)))) {
      out.push_back(make_object({{"key", g.key}, {"count", g.count}}));
    }
    return out;
  };
  m["points.groupPoints"] = [](const Array& a, ScriptCtx&) -> Value {
    const Value v = arg(a, 0);
    Array out;
    for (const std::size_t i : group_points(to_points(v), str(arg(a, 1)))) {
      out.push_back(arr(v)[i]);
    }
    return out;
  };
  m["points.writePoints"] = [](const Array& a, ScriptCtx&) -> Value {
    return write_points(to_points(arg(a, 0)), opt_num(arg(a, 1)).value_or(3));
  };
  m["points.lassoPoints"] = [](const Array& a, ScriptCtx&) -> Value {
    const Value v = arg(a, 0);
    std::vector<XY> path;
    for (const P2& p : to_p2(arg(a, 1))) {
      path.push_back({p.x, p.y});
    }
    Array out;
    for (const std::size_t i : lasso_points(to_points(v), path, str(arg(a, 2)), num(arg(a, 3)))) {
      out.push_back(field(arr(v)[i], "n"));
    }
    return out;
  };

  // ---- tin.js
  m["tin.buildSurface"] = [](const Array& a, ScriptCtx& c) -> Value {
    Built b = build_surface(to_build(arg(a, 0)), *c.words);
    if (b.surface) {
      c.keep(b.surface);
    }
    return built_value(b, false);
  };
  m["tin.surfaceFromFaces"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value in = arg(a, 0);
    FaceInput fi;
    for (const Value& p : arr(field(in, "pts"))) {
      if (p.truthy()) {
        fi.pts.emplace_back(P3{num(field(p, "x")), num(field(p, "y")), num(field(p, "z"))});
      } else {
        fi.pts.emplace_back(std::nullopt);
      }
    }
    for (const Value& f : arr(field(in, "faces"))) {
      std::vector<double> ids;
      for (const Value& i : arr(f)) {
        ids.push_back(i.is_number() ? i.as_number() : std::nan(""));
      }
      fi.faces.push_back(std::move(ids));
    }
    Built b = surface_from_faces(fi, *c.words);
    if (b.surface) {
      c.keep(b.surface);
    }
    return built_value(b, true);
  };
  m["tin.convexHull"] = [](const Array& a, ScriptCtx&) -> Value {
    const Value v = arg(a, 0);
    Array out;
    for (const std::size_t i : convex_hull(to_p2(v))) {
      out.push_back(arr(v)[i]);
    }
    return out;
  };
  m["tin.polyArea"] = [](const Array& a, ScriptCtx&) -> Value { return poly_area(to_p2(arg(a, 0))); };
  m["tin.ccw"] = [](const Array& a, ScriptCtx&) -> Value {
    Array out = arr(arg(a, 0));
    if (is_clockwise(to_p2(arg(a, 0)))) {
      std::ranges::reverse(out);
    }
    return out;
  };
  m["tin.pointInPoly"] = [](const Array& a, ScriptCtx&) -> Value {
    return point_in_poly(num(arg(a, 0)), num(arg(a, 1)), to_p2(arg(a, 2)));
  };
  m["tin.clipConvex"] = [](const Array& a, ScriptCtx&) -> Value {
    const Value sub = arg(a, 0);
    Array out;
    for (const Clipped& p : clip_convex(to_p2(sub), to_p2(arg(a, 1)))) {
      out.push_back(p.src >= 0 ? arr(sub)[static_cast<std::size_t>(p.src)] : base::pt2(p.x, p.y));
    }
    return out;
  };

  // ---- Surface methods
  m["surface.view"] = [](const Array&, ScriptCtx& c) -> Value { return surf_view(self_surface(c)); };
  m["surface.stats"] = [](const Array&, ScriptCtx& c) -> Value { return stats_value(self_surface(c).stats()); };
  m["surface.elevationAt"] = [](const Array& a, ScriptCtx& c) -> Value {
    const auto z = self_surface(c).elevation_at(num(arg(a, 0)), num(arg(a, 1)), opt_num(arg(a, 2)).value_or(1e-9));
    return z ? Value(*z) : Value(nullptr);
  };
  m["surface.contours"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value o = arg(a, 1);
    const Value base = field(o, "base");
    const Value ml = field(o, "maxLevels");
    const auto r = self_surface(c).contours(num(arg(a, 0)), base.is_undefined() ? 0 : num(base),
                                            ml.is_undefined() ? 2000 : num(ml));
    if (!r) {
      return nullptr;
    }
    Array out;
    for (const Contour& k : *r) {
      out.push_back(make_object({{"z", k.z}, {"pts", p2s(k.pts)}, {"closed", k.closed}}));
    }
    return out;
  };
  m["surface.bands"] = [](const Array& a, ScriptCtx& c) -> Value {
    std::vector<double> levels;
    for (const Value& v : arr(arg(a, 0))) {
      levels.push_back(num(v));
    }
    Array out;
    for (const Band& b : self_surface(c).bands(levels)) {
      out.push_back(make_object({{"band", b.band}, {"loop", nums(b.loop)}}));
    }
    return out;
  };
  m["surface.slopes"] = [](const Array&, ScriptCtx& c) -> Value {
    Array out;
    for (const Slope& s : self_surface(c).slopes()) {
      out.push_back(make_object({{"x", s.x}, {"y", s.y}, {"slope", s.slope}, {"dir", s.dir}, {"size", s.size}}));
    }
    return out;
  };
  m["surface.edges"] = [](const Array&, ScriptCtx& c) -> Value { return ints(self_surface(c).edges()); };
  m["surface.outline"] = [](const Array&, ScriptCtx& c) -> Value {
    Array out;
    for (const auto& l : self_surface(c).outline()) {
      out.push_back(p2s(l));
    }
    return out;
  };
  m["surface.toFaces"] = [](const Array&, ScriptCtx& c) -> Value {
    const auto f = self_surface(c).to_faces();
    Array pts;
    for (const P3& p : f.pts) {
      pts.push_back(base::pt3(p.x, p.y, p.z));
    }
    Array faces;
    for (const auto& t : f.faces) {
      faces.emplace_back(Array{Value(t[0]), Value(t[1]), Value(t[2])});
    }
    return make_object({{"pts", std::move(pts)}, {"faces", std::move(faces)}});
  };
  m["surface.hull"] = [](const Array&, ScriptCtx& c) -> Value { return p2s(self_surface(c).hull()); };
  m["surface.triangles"] = [](const Array&, ScriptCtx& c) -> Value {
    Array out;
    for (const auto& t : self_surface(c).triangles()) {
      out.push_back(nums(std::vector<double>(t.begin(), t.end())));
    }
    return out;
  };
}

}  // namespace cc::civil
