// SPDX-License-Identifier: Apache-2.0
// superelev.js, grading.js and corridor.js (CivilCraft 0.5), ported function by function. Their arguments are JSON
// values as the JS gets them; numbers are read with Number(), as its arithmetic does.
#include "cc/civil/design.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <set>

#include "cc/appkit/jsops.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::make_object;
using base::Value;
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kMaxSteps = 1e5;  // hostile input only (docs/gaps.md): a zero step or fan step

double num(const Value& v) {
  return base::js_to_number(v);
}
Value get(const Value& o, std::string_view k) {
  return o.is_object() ? o.get(k) : Value();
}
const Array& arr(const Value& v) {
  static const Array kEmpty;
  return v.is_array() ? v.as_array() : kEmpty;
}
/// A temporary's array is moved out, so a reference bound to the result lives as long as the reference.
Array arr(Value&& v) {
  return v.is_array() ? std::move(v).as_array() : Array{};
}
double hyp(double a, double b) {
  return base::js_hypot(a, b);
}
double jmax(double a, double b) {
  return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::max(a, b);
}
double jmin(double a, double b) {
  return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::min(a, b);
}
double sign(double v) {
  if (std::isnan(v) || v == 0) {
    return v;
  }
  return v > 0 ? 1 : -1;
}
bool fin(const Value& v) {  // typeof v === 'number' && Number.isFinite(v)
  return v.is_number() && std::isfinite(v.as_number());
}
P2 xy(const Value& p) {
  return {num(get(p, "x")), num(get(p, "y"))};
}
Value p3v(double x, double y, double z) {
  return base::pt3(x, y, z);
}
Value strs(const std::vector<std::string>& v) {
  Array a;
  for (const auto& s : v) {
    a.emplace_back(s);
  }
  return a;
}
std::string fill_name(const Value& v) {
  return base::js_string(v);
}

/// A JS Set of numbers, then sorted ascending.
class StaSet {
 public:
  void add(double s) {
    if (s == 0) {
      s = 0;
    }
    bool fresh = false;
    if (std::isnan(s)) {
      fresh = !nan_;
      nan_ = true;
    } else {
      fresh = seen_.insert(s).second;
    }
    if (fresh) {
      order_.push_back(s);
    }
  }
  [[nodiscard]] std::vector<double> sorted() const {
    std::vector<double> v = order_;
    appkit::js_sort(v, [](double a, double b) { return a - b; });
    return v;
  }

 private:
  std::set<double> seen_;
  std::vector<double> order_;
  bool nan_ = false;
};

bool proper_cross(P2 a, P2 b, P2 c, P2 d) {
  auto o = [](P2 p, P2 q, P2 r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
  const double d1 = o(a, b, c);
  const double d2 = o(a, b, d);
  const double d3 = o(c, d, a);
  const double d4 = o(c, d, b);
  return ((d1 > 1e-12 && d2 < -1e-12) || (d1 < -1e-12 && d2 > 1e-12)) &&
         ((d3 > 1e-12 && d4 < -1e-12) || (d3 < -1e-12 && d4 > 1e-12));
}
bool near_poly(P2 p, const std::vector<P2>& poly, double tol) {
  for (std::size_t i = 0; i < poly.size(); ++i) {
    const P2 a = poly[i];
    const P2 b = poly[(i + 1) % poly.size()];
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double L = dx * dx + dy * dy;
    const double t = L != 0 && !std::isnan(L) ? jmax(0, jmin(1, ((p.x - a.x) * dx + (p.y - a.y) * dy) / L)) : 0;
    if (hyp(p.x - a.x - dx * t, p.y - a.y - dy * t) <= tol) {
      return true;
    }
  }
  return false;
}
bool crosses_poly(const std::vector<P2>& line, const std::vector<P2>& poly) {
  for (std::size_t i = 0; i + 1 < line.size(); ++i) {
    for (std::size_t j = 0; j < poly.size(); ++j) {
      if (proper_cross(line[i], line[i + 1], poly[j], poly[(j + 1) % poly.size()])) {
        return true;
      }
    }
  }
  return false;
}
std::vector<P2> xys(const Value& v) {
  std::vector<P2> out;
  for (const Value& p : arr(v)) {
    out.push_back(xy(p));
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------- superelev.js

Value super_defaults(std::string_view units) {
  auto rows = [](std::initializer_list<std::pair<double, double>> r) {
    Array a;
    for (const auto& [x, e] : r) {
      a.emplace_back(Array{Value(x), e < 0 ? Value(nullptr) : Value(e)});
    }
    return a;
  };
  if (units == "m") {
    return make_object({{"on", false},
                        {"delta", 0.005},
                        {"rate", rows({{2100, -1}, {1300, 0.02}, {800, 0.03}, {550, 0.04}, {380, 0.05}, {0, 0.06}})}});
  }
  return make_object({{"on", false},
                      {"delta", 0.005},
                      {"rate", rows({{7000, -1}, {4300, 0.02}, {2600, 0.03}, {1800, 0.04}, {1250, 0.05}, {0, 0.06}})}});
}

Value clean_super(const Value& o, std::string_view units) {
  if (!o.is_object() && !o.is_array()) {
    return nullptr;
  }
  const Value d = super_defaults(units);
  Array rate;
  const Value r_in = get(o, "rate");
  if (r_in.is_array()) {
    for (const Value& r : r_in.as_array()) {
      if (!r.is_array()) {
        continue;
      }
      const Value r0 = r.as_array().empty() ? Value() : r.as_array()[0];
      const Value r1 = r.as_array().size() > 1 ? r.as_array()[1] : Value();
      if (fin(r0) && r0.as_number() >= 0 && r0.as_number() < 1e7 &&
          (r1.is_null() || (fin(r1) && r1.as_number() >= 0 && r1.as_number() <= 0.2))) {
        rate.emplace_back(Array{r0, r1});
      }
      if (rate.size() >= 40) {
        break;
      }
    }
  }
  appkit::js_sort(
      rate, [](const Value& a, const Value& b) { return b.as_array()[0].as_number() - a.as_array()[0].as_number(); });
  const Value delta = get(o, "delta");
  const bool on = get(o, "on").is_bool() && get(o, "on").as_bool();
  return make_object(
      {{"on", on},
       {"delta", fin(delta) && delta.as_number() > 0 && delta.as_number() <= 0.05 ? delta : d.get("delta")},
       {"rate", rate.empty() ? d.get("rate") : Value(std::move(rate))}});
}

std::vector<RateRow> rate_rows(const Value& rate) {
  std::vector<RateRow> out;
  for (const Value& r : arr(rate)) {
    RateRow row;
    const Value r0 = r.is_array() && !r.as_array().empty() ? r.as_array()[0] : Value();
    const Value r1 = r.is_array() && r.as_array().size() > 1 ? r.as_array()[1] : Value();
    row.r = num(r0);
    if (!base::Value(r1).is_null() && !r1.is_undefined()) {
      row.e = num(r1);
    }
    out.push_back(row);
  }
  return out;
}

std::optional<double> rate_for(double R, const std::vector<RateRow>& rate) {
  for (const RateRow& r : rate) {
    if (R >= r.r) {
      return r.e;
    }
  }
  return rate.empty() ? std::nullopt : rate.back().e;
}

SuperPlan super_plan(const Alignment* al, const Value& sp, double nc, double lane, const i18n::Catalog& words) {
  SuperPlan plan;
  if (al == nullptr || !al->ok() || !get(sp, "on").truthy()) {
    return plan;
  }
  const std::vector<RateRow> rate = rate_rows(get(sp, "rate"));
  const double delta = num(get(sp, "delta"));
  std::size_t i = 0;
  for (const Element& c : al->elements) {
    if (!c.arc) {
      continue;
    }
    const std::size_t idx = i++;
    const auto e = rate_for(c.r, rate);
    if (!e || !(*e > 0)) {
      continue;
    }
    SuperCurve sc;
    sc.i = static_cast<double>(idx + 1);
    sc.R = c.r;
    sc.left = c.ccw;
    sc.e = *e;
    sc.Lr = (lane * *e) / delta;
    sc.Lt = (nc / std::max(*e, 1e-9)) * sc.Lr;
    sc.pc = c.sta;
    sc.pt = c.sta + c.len;
    sc.lc0 = sc.pc - (2 * sc.Lr) / 3;
    sc.fs0 = sc.pc + sc.Lr / 3;
    sc.fs1 = sc.pt - sc.Lr / 3;
    sc.lc1 = sc.pt + (2 * sc.Lr) / 3;
    sc.nc0 = sc.lc0 - sc.Lt;
    sc.nc1 = sc.lc1 + sc.Lt;
    sc.rc0 = sc.lc0 + (sc.Lr * jmin(1, nc / *e));
    sc.rc1 = sc.lc1 - (sc.Lr * jmin(1, nc / *e));
    if (sc.fs1 < sc.fs0) {
      plan.problems.push_back(
          words.t("se.short", {{"n", appkit::num_str(static_cast<double>(idx + 1))},
                               {"r", appkit::num_str(base::js_number(base::js_to_fixed(c.r, 2)))}}));
    }
    plan.curves.push_back(sc);
  }
  for (std::size_t k = 1; k < plan.curves.size(); ++k) {
    if (plan.curves[k].nc0 < plan.curves[k - 1].nc1) {
      plan.problems.push_back(words.t(
          "se.overlap", {{"a", appkit::num_str(plan.curves[k - 1].i)}, {"b", appkit::num_str(plan.curves[k].i)}}));
    }
  }
  return plan;
}

namespace {
double outside_at(const SuperCurve& c, double sta, double nc) {
  const double e = c.e;
  auto up = [&](double x) {
    if (x <= c.nc0) {
      return nc;
    }
    if (x <= c.lc0) {
      return nc - (nc * (x - c.nc0)) / std::max(c.Lt, 1e-9);
    }
    return -(e * (x - c.lc0)) / std::max(c.Lr, 1e-9);
  };
  auto down = [&](double x) {
    if (x >= c.nc1) {
      return nc;
    }
    if (x >= c.lc1) {
      return nc - (nc * (c.nc1 - x)) / std::max(c.Lt, 1e-9);
    }
    return -(e * (c.lc1 - x)) / std::max(c.Lr, 1e-9);
  };
  return jmax(jmax(up(sta), down(sta)), -e);
}
}  // namespace

SideSlopes slopes_at(const SuperPlan& plan, double sta, double nc) {
  SideSlopes s{nc, nc};
  for (const SuperCurve& c : plan.curves) {
    if (sta <= c.nc0 || sta >= c.nc1) {
      continue;
    }
    const double o = outside_at(c, sta, nc);
    const double inside = jmax(nc, -o);
    if (!c.left) {  // outside 'L'
      s.L = o;
      s.R = inside;
    } else {
      s.R = o;
      s.L = inside;
    }
  }
  return s;
}

std::vector<double> super_key_stations(const SuperPlan& plan) {
  std::vector<double> out;
  for (const SuperCurve& c : plan.curves) {
    for (const double v : {c.lc0, c.fs0, c.fs1, c.lc1, c.nc0, c.nc1, c.rc0, c.rc1}) {
      out.push_back(v);
    }
  }
  return out;
}

Value super_plan_value(const SuperPlan& p) {
  Array curves;
  for (const SuperCurve& c : p.curves) {
    curves.push_back(make_object({{"i", c.i},
                                  {"R", c.R},
                                  {"dir", c.left ? "left" : "right"},
                                  {"outside", c.left ? "R" : "L"},
                                  {"e", c.e},
                                  {"Lr", c.Lr},
                                  {"Lt", c.Lt},
                                  {"pc", c.pc},
                                  {"pt", c.pt},
                                  {"stations", make_object({{"lc0", c.lc0},
                                                            {"fs0", c.fs0},
                                                            {"fs1", c.fs1},
                                                            {"lc1", c.lc1},
                                                            {"nc0", c.nc0},
                                                            {"nc1", c.nc1},
                                                            {"rc0", c.rc0},
                                                            {"rc1", c.rc1}})}}));
  }
  return make_object({{"curves", std::move(curves)}, {"problems", strs(p.problems)}});
}

// ---------------------------------------------------------------------------------------------- grading.js

Cast cast_slope(const Surface& s, P2 p0, double ux, double uy, double z, double cut, double fill, double rate,
                double step, double far) {
  const auto z0 = s.elevation_at(p0.x, p0.y, 1e-6);
  if (!z0) {
    return {p0.x, p0.y, p0.x, p0.y, z, 0, "off"};
  }
  if (std::abs(*z0 - z) < 1e-9) {
    return {p0.x, p0.y, p0.x, p0.y, z, 0, "flat"};
  }
  const bool cut_side = *z0 > z;
  const double g = cut_side ? rate / cut : -rate / fill;
  auto f = [&](double d) -> std::optional<double> {
    const auto e = s.elevation_at(p0.x + ux * d, p0.y + uy * d, 1e-6);
    if (!e) {
      return std::nullopt;
    }
    return z + g * d - *e;
  };
  const auto f0 = f(0);
  const double s0 = f0 ? sign(*f0) : 0;  // Math.sign(null) is 0
  double a = 0;
  double b = step;
  auto fb = f(b);
  for (std::int64_t n = 0; fb && sign(*fb) == s0 && b < far && static_cast<double>(n) < 1e7; ++n) {
    a = b;
    b += step;
    fb = f(b);
  }
  if (!fb || b >= far) {
    return {p0.x, p0.y, p0.x + ux * a, p0.y + uy * a, z + g * a, a, "off"};
  }
  for (int it = 0; it < 60 && b - a > 1e-7; ++it) {
    const double m = (a + b) / 2;
    const auto fm = f(m);
    if (fm && sign(*fm) == s0) {
      a = m;
    } else {
      b = m;
    }
  }
  const double d = (a + b) / 2;
  return {p0.x, p0.y, p0.x + ux * d, p0.y + uy * d, z + g * d, d, cut_side ? "cut" : "fill"};
}

Value cast_value(const Cast& c) {
  return make_object(
      {{"x0", c.x0}, {"y0", c.y0}, {"x", c.x}, {"y", c.y}, {"z", c.z}, {"d", c.d}, {"kind", std::string(c.kind)}});
}

bool self_crosses(const std::vector<P2>& pts) {
  const std::size_t n = pts.size();
  for (std::size_t i = 0; i < n; ++i) {
    const P2 a = pts[i];
    const P2 b = pts[(i + 1) % n];
    for (std::size_t j = i + 2; j < n; ++j) {
      if (i == 0 && j == n - 1) {
        continue;
      }
      if (proper_cross(a, b, pts[j], pts[(j + 1) % n])) {
        return true;
      }
    }
  }
  return false;
}

Value daylight_pad(const Surface& s, const Value& pad, const Value& options, const i18n::Catalog& words) {
  Array problems;
  // the corners, repeats dropped (against the corner before in the given list), counter-clockwise
  const Array& in = arr(get(pad, "pts"));
  std::vector<Value> poly;
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (i == 0) {
      poly.push_back(in[i]);
      continue;
    }
    const P2 p = xy(in[i]);
    const P2 q = xy(in[i - 1]);
    if (hyp(p.x - q.x, p.y - q.y) > 1e-9) {
      poly.push_back(in[i]);
    }
  }
  auto pts_of = [&] {
    std::vector<P2> v;
    v.reserve(poly.size());
    for (const Value& p : poly) {
      v.push_back(xy(p));
    }
    return v;
  };
  if (is_clockwise(pts_of())) {
    std::ranges::reverse(poly);
  }
  if (poly.size() > 2) {
    const P2 a = xy(poly.front());
    const P2 b = xy(poly.back());
    if (hyp(a.x - b.x, a.y - b.y) < 1e-9) {
      poly.pop_back();
    }
  }
  const std::vector<P2> P = pts_of();
  auto fail = [&](const char* key) {
    return make_object({{"rays", Array{}},
                        {"daylight", Array{}},
                        {"problems", Array{Value(words.t(key))}},
                        {"poly", Array(poly.begin(), poly.end())}});
  };
  if (P.size() >= 4 && self_crosses(P)) {
    return fail("grade.err.cross");
  }
  if (P.size() < 3 || std::abs(poly_area(P)) < 1e-9) {
    return fail("grade.err.three");
  }
  const double cut = num(get(pad, "cut"));
  const double fill = num(get(pad, "fill"));
  const double z = num(get(pad, "z"));
  auto zv = [&](const Value& p) {
    const Value pz = get(p, "z");
    return fin(pz) ? pz.as_number() : z;
  };
  if (!(cut > 0) || !(fill > 0)) {
    return fail("grade.err.slopes");
  }
  double size = 0;
  for (const P2& p : P) {
    for (const P2& q : P) {
      size = jmax(size, hyp(p.x - q.x, p.y - q.y));
    }
  }
  const double spacing = num(get(options, "spacing"));
  const Value fs = get(options, "fanStep");
  const double fan_step = fs.is_undefined() ? std::numbers::pi / 18 : num(fs);
  const Value md = get(options, "maxDist");
  const double sp = spacing > 0 ? spacing : jmax(size / 20, 0.5);
  const SurfaceStats& sb = s.stats();
  double far = num(md);
  if (!md.truthy()) {
    far = 2 * hyp(sb.max_x - sb.min_x, sb.max_y - sb.min_y);
  }
  const std::size_t n = P.size();
  struct Ray {
    Cast c;
    double z0;
    std::optional<std::size_t> edge;
  };
  std::vector<Ray> rays;
  struct Concave {
    std::size_t i;
    P2 v;
    double bx, by;
  };
  std::vector<Concave> concave;
  struct EdgeN {
    double x, y, L;
  };
  auto edge_n = [&](std::size_t i) {
    const P2 a = P[i];
    const P2 b = P[(i + 1) % n];
    const double L = hyp(b.x - a.x, b.y - a.y);
    return EdgeN{(b.y - a.y) / L, -(b.x - a.x) / L, L};
  };
  auto cast = [&](P2 p0, double ux, double uy, double rate, double z0) {
    return Ray{cast_slope(s, p0, ux, uy, z0, cut, fill, rate, jmax(sp / 4, 0.05), far), z0, std::nullopt};
  };
  for (std::size_t i = 0; i < n; ++i) {
    const P2 v = P[i];
    const EdgeN n_in = edge_n((i + n - 1) % n);
    const EdgeN n_out = edge_n(i);
    const double turn = base::js_atan2(n_in.x * n_out.y - n_in.y * n_out.x, n_in.x * n_out.x + n_in.y * n_out.y);
    if (turn > 1e-9) {
      const double a0 = base::js_atan2(n_in.y, n_in.x);
      const double kk = std::ceil(turn / fan_step);
      double k = jmax(1, kk);
      k = std::isnan(kk) ? std::nan("") : std::min(k, kMaxSteps);
      for (std::int64_t j = 0; static_cast<double>(j) <= k; ++j) {
        const double a = a0 + (turn * static_cast<double>(j)) / k;
        rays.push_back(cast(v, base::js_cos(a), base::js_sin(a), 1, zv(poly[i])));
      }
    } else if (turn < -1e-9) {
      const double bx = n_in.x + n_out.x;
      const double by = n_in.y + n_out.y;
      const double L = hyp(bx, by);
      rays.push_back(cast(v, bx / L, by / L, base::js_cos(std::abs(turn) / 2), zv(poly[i])));
      concave.push_back({i, v, bx / L, by / L});
    } else {
      rays.push_back(cast(v, n_out.x, n_out.y, 1, zv(poly[i])));
    }
    const P2 b = P[(i + 1) % n];
    const double mm = std::ceil(n_out.L / sp);
    double m = jmax(1, mm);
    m = std::isnan(mm) ? std::nan("") : std::min(m, kMaxSteps);
    for (std::int64_t j = 1; static_cast<double>(j) < m; ++j) {
      const double t = static_cast<double>(j) / m;
      const double zi = zv(poly[i]);
      const double zb = zv(poly[(i + 1) % n]);
      Ray r = cast({v.x + (b.x - v.x) * t, v.y + (b.y - v.y) * t}, n_out.x, n_out.y, 1, zi + (zb - zi) * t);
      r.edge = i;
      rays.push_back(r);
    }
  }
  for (const Concave& c : concave) {
    auto side = [&](double x, double y) { return c.bx * (y - c.v.y) - c.by * (x - c.v.x); };
    for (std::size_t k = rays.size(); k-- > 0;) {
      const Ray& r = rays[k];
      if (!r.edge || (*r.edge != c.i && *r.edge != (c.i + n - 1) % n)) {
        continue;
      }
      const double s0 = side(r.c.x0, r.c.y0);
      const double s1 = side(r.c.x, r.c.y);
      if ((s0 > 1e-9 && s1 < -1e-9) || (s0 < -1e-9 && s1 > 1e-9)) {
        rays.erase(rays.begin() + static_cast<std::ptrdiff_t>(k));
      }
    }
  }
  const auto bad = std::ranges::count_if(rays, [](const Ray& r) { return std::string_view(r.c.kind) == "off"; });
  if (bad != 0) {
    problems.emplace_back(
        words.tn("grade.off", static_cast<double>(bad), {{"n", appkit::num_str(static_cast<double>(bad))}}));
  }
  Array rays_v;
  Array daylight;
  std::vector<P2> dl;
  for (const Ray& r : rays) {
    Value o = cast_value(r.c);
    o.set("z0", r.z0);
    if (r.edge) {
      o.set("edge", static_cast<double>(*r.edge));
    }
    rays_v.push_back(std::move(o));
    daylight.push_back(p3v(r.c.x, r.c.y, r.c.z));
    dl.push_back({r.c.x, r.c.y});
  }
  if (bad == 0 && self_crosses(dl)) {
    problems.emplace_back(words.t("grade.daycross"));
  }
  return make_object({{"rays", std::move(rays_v)},
                      {"daylight", std::move(daylight)},
                      {"problems", std::move(problems)},
                      {"poly", Array(poly.begin(), poly.end())}});
}

namespace {
double nice_cell(double v) {
  if (!(v > 0)) {
    return 1;
  }
  const double p = std::pow(10, std::floor(std::log10(v)));
  const double m = v / p;
  double f = 10;
  if (m < 1.5) {
    f = 1;
  } else if (m < 3.5) {
    f = 2;
  } else if (m < 7.5) {
    f = 5;
  }
  return f * p;
}
}  // namespace

GridVolume grid_volume(const Surface& base, const Surface& top, const Value& options) {
  const SurfaceStats& ba = base.stats();
  const SurfaceStats& bb = top.stats();
  const Value bounds = get(options, "bounds");
  double min_x = 0;
  double min_y = 0;
  double max_x = 0;
  double max_y = 0;
  if (bounds.truthy()) {
    min_x = num(get(bounds, "minX"));
    min_y = num(get(bounds, "minY"));
    max_x = num(get(bounds, "maxX"));
    max_y = num(get(bounds, "maxY"));
  } else {
    min_x = jmax(ba.min_x, bb.min_x);
    min_y = jmax(ba.min_y, bb.min_y);
    max_x = jmin(ba.max_x, bb.max_x);
    max_y = jmin(ba.max_y, bb.max_y);
  }
  std::vector<std::vector<P2>> regions;
  bool have_regions = false;
  const Value region = get(options, "region");
  if (region.truthy()) {
    have_regions = true;
    const Array& r = arr(region);
    const bool many = !r.empty() && r[0].is_array();
    if (many) {
      for (const Value& poly : r) {
        if (poly.truthy()) {
          regions.push_back(xys(poly));
        }
      }
    } else {
      regions.push_back(xys(region));
    }
  }
  if (have_regions && !regions.empty()) {
    double r0 = kInf;
    double r1 = kInf;
    double r2 = -kInf;
    double r3 = -kInf;
    for (const auto& poly : regions) {
      for (const P2& p : poly) {
        r0 = jmin(r0, p.x);
        r1 = jmin(r1, p.y);
        r2 = jmax(r2, p.x);
        r3 = jmax(r3, p.y);
      }
    }
    min_x = jmax(min_x, r0);
    min_y = jmax(min_y, r1);
    max_x = jmin(max_x, r2);
    max_y = jmin(max_y, r3);
  }
  const double W = max_x - min_x;
  const double H = max_y - min_y;
  const double cell = num(get(options, "cell"));
  GridVolume out;
  const bool area = W > 0 && H > 0;  // (NaN: none)
  if (!area) {
    out.cell = (std::isnan(cell) || cell == 0) ? 0 : cell;
    return out;
  }
  const Value mc = get(options, "maxCells");
  const double max_cells = mc.is_undefined() ? 4e6 : num(mc);
  double c = cell > 0 ? cell : nice_cell(std::sqrt((W * H) / 250000));
  for (int guard = 0; (W / c) * (H / c) > max_cells && guard < 2000; ++guard) {
    c *= 2;
  }
  const double i0 = std::floor(min_x / c);
  const double i1 = std::ceil(max_x / c);
  const double j0 = std::floor(min_y / c);
  const double j1 = std::ceil(max_y / c);
  double cut_v = 0;
  double fill_v = 0;
  double cells = 0;
  const double A = c * c;
  // (the cell count is capped by max_cells above; the 4·10⁷ guard only stops a hostile maxCells)
  if (std::isfinite(i0) && std::isfinite(i1) && std::isfinite(j0) && std::isfinite(j1) &&
      (i1 - i0) * (j1 - j0) <= 4e7) {
    const auto nj = static_cast<std::int64_t>(std::max(0.0, j1 - j0));
    const auto ni = static_cast<std::int64_t>(std::max(0.0, i1 - i0));
    for (std::int64_t qj = 0; qj < nj; ++qj) {
      const double y = (j0 + static_cast<double>(qj) + 0.5) * c;
      for (std::int64_t qi = 0; qi < ni; ++qi) {
        const double x = (i0 + static_cast<double>(qi) + 0.5) * c;
        if (have_regions &&
            !std::ranges::any_of(regions, [&](const auto& poly) { return point_in_poly(x, y, poly); })) {
          continue;
        }
        const auto z0 = base.elevation_at(x, y);
        const auto z1 = top.elevation_at(x, y);
        if (!z0 || !z1) {
          continue;
        }
        cells++;
        const double dz = *z1 - *z0;
        if (dz > 0) {
          fill_v += dz * A;
        } else {
          cut_v -= dz * A;
        }
      }
    }
  }
  out = {cut_v, fill_v, fill_v - cut_v, cells, c, cells * A};
  return out;
}

namespace {
Value pad_patch(const Surface& surf, const Value& pad, const i18n::Catalog& words) {
  const Value opt = get(pad, "options");
  const Value dl = daylight_pad(surf, pad, opt.truthy() ? opt : Value::object(), words);
  const Array& problems = arr(dl.get("problems"));
  const Array& rays = arr(dl.get("rays"));
  if (!problems.empty() || rays.empty()) {
    return make_object(
        {{"problems", problems.empty() ? Value(Array{Value(words.t("grade.none"))}) : dl.get("problems")},
         {"region", nullptr},
         {"points", Array{}},
         {"breaklines", Array{}},
         {"info", dl}});
  }
  const Value region = dl.get("daylight");
  const double pad_z = num(get(pad, "z"));
  auto zv = [&](const Value& p) {
    const Value pz = get(p, "z");
    return fin(pz) ? pz.as_number() : pad_z;
  };
  const Value nm = get(pad, "name");
  const std::string name = nm.truthy() ? fill_name(nm) : words.t("scene.pad.default");
  Array bl;
  Array edge;
  const Array& poly = arr(dl.get("poly"));
  for (std::size_t i = 0; i <= poly.size(); ++i) {
    const Value& p = poly[i % poly.size()];
    const P2 q = xy(p);
    edge.push_back(p3v(q.x, q.y, zv(p)));
  }
  bl.push_back(make_object({{"name", words.t("grade.bl.edge", {{"name", name}})}, {"pts", std::move(edge)}}));
  Array dlp;
  const Array& reg = arr(region);
  for (std::size_t i = 0; i <= reg.size(); ++i) {
    const Value& p = reg[i % reg.size()];
    dlp.push_back(p3v(num(p.get("x")), num(p.get("y")), num(p.get("z"))));
  }
  bl.push_back(make_object({{"name", words.t("grade.bl.daylight", {{"name", name}})}, {"pts", std::move(dlp)}}));
  for (const Value& r : rays) {
    if (num(r.get("d")) > 1e-6) {
      const Value z0 = r.get("z0");
      bl.push_back(make_object(
          {{"name", "slope"},
           {"pts", Array{p3v(num(r.get("x0")), num(r.get("y0")), (z0.is_null() || z0.is_undefined()) ? pad_z : num(z0)),
                         p3v(num(r.get("x")), num(r.get("y")), num(r.get("z")))}}}));
    }
  }
  return make_object(
      {{"problems", Array{}}, {"region", region}, {"points", Array{}}, {"breaklines", std::move(bl)}, {"info", dl}});
}
}  // namespace

Graded graded_surface(std::shared_ptr<Surface> base, const Value& base_points, const Value& base_breaklines,
                      const std::vector<GradeItem>& items, const i18n::Catalog& words, const BuildFn& build) {
  Array regions;
  Array report_items;
  Array notes;
  Array errors;
  Array infos;
  std::shared_ptr<Surface> surf = std::move(base);
  Array points = arr(base_points);
  Array breaklines = arr(base_breaklines);
  bool any = false;
  auto report = [&] { return make_object({{"notes", notes}, {"errors", errors}, {"items", report_items}}); };
  for (const GradeItem& item : items) {
    const Value pt = item.patch ? item.patch(*surf) : pad_patch(*surf, item.pad, words);
    report_items.push_back(make_object(
        {{"name", item.patch ? Value(item.name) : get(item.pad, "name")}, {"problems", pt.get("problems")}}));
    infos.push_back(pt.get("info"));
    const Value region_v = pt.get("region");
    if (!arr(pt.get("problems")).empty() || !region_v.truthy() || arr(region_v).size() < 3) {
      regions.emplace_back(nullptr);
      continue;
    }
    regions.push_back(region_v);
    const std::vector<P2> region = xys(region_v);
    auto inside = [&](const Value& p) {
      const P2 q = xy(p);
      return point_in_poly(q.x, q.y, region) || near_poly(q, region, 1e-6);
    };
    Array kept;
    for (Value& p : points) {
      if (!inside(p)) {
        kept.push_back(std::move(p));
      }
    }
    points = std::move(kept);
    Array kept_b;
    for (Value& b : breaklines) {
      const Array& bp = arr(get(b, "pts"));
      if (std::ranges::all_of(bp, [&](const Value& p) { return !inside(p); }) &&
          !crosses_poly(xys(get(b, "pts")), region)) {
        kept_b.push_back(std::move(b));
      }
    }
    breaklines = std::move(kept_b);
    for (const Value& p : arr(pt.get("points"))) {
      points.push_back(p);
    }
    for (const Value& b : arr(pt.get("breaklines"))) {
      breaklines.push_back(b);
    }
    Built built = build(Value(points), Value(breaklines));
    if (!built.surface) {
      for (const auto& e : built.report.errors) {
        errors.emplace_back(e);
      }
      return {nullptr, report(), Value(std::move(regions)), Value(std::move(infos))};
    }
    surf = built.surface;
    any = true;
    for (const auto& e : built.report.notes) {
      notes.emplace_back(e);
    }
  }
  return {any ? surf : nullptr, report(), Value(std::move(regions)), Value(std::move(infos))};
}

// ---------------------------------------------------------------------------------------------- corridor.js

Value default_assembly(std::string_view units) {
  if (units == "m") {
    return make_object({{"lane", 3.6},
                        {"laneSlope", 0.02},
                        {"shoulder", 1.2},
                        {"shoulderSlope", 0.04},
                        {"cut", 2},
                        {"fill", 3},
                        {"step", 5}});
  }
  return make_object({{"lane", 12},
                      {"laneSlope", 0.02},
                      {"shoulder", 4},
                      {"shoulderSlope", 0.04},
                      {"cut", 2},
                      {"fill", 3},
                      {"step", 10}});
}

namespace {
bool seg_cross(P2 a, P2 b, P2 c, P2 d) {
  return proper_cross(a, b, c, d);
}
}  // namespace

Value corridor_patch(const Surface* surface, const Alignment* al, const Profile* prof, const Value& asm_,
                     const i18n::Catalog& words) {
  auto empty = [&](const char* key) {
    return make_object({{"problems", Array{Value(words.t(key))}},
                        {"region", nullptr},
                        {"points", Array{}},
                        {"breaklines", Array{}},
                        {"info", make_object({{"sections", Array{}}, {"lines", Value::object()}})}});
  };
  if (al == nullptr || !al->ok()) {
    return empty("corr.err.al");
  }
  if (prof == nullptr || !prof->ok()) {
    return empty("corr.err.prof");
  }
  Value A = default_assembly("ft");
  if (asm_.is_object()) {
    for (const base::Member* m : base::js_members(asm_.as_object())) {
      A.set(m->key, m->value);
    }
  }
  const double lane = num(A.get("lane"));
  const double lane_slope = num(A.get("laneSlope"));
  const double shoulder = num(A.get("shoulder"));
  const double shoulder_slope = num(A.get("shoulderSlope"));
  const double cut = num(A.get("cut"));
  const double fill = num(A.get("fill"));
  const bool slopes = cut > 0 && fill > 0;
  if (!slopes) {
    return empty("corr.err.slopes");
  }
  if (surface == nullptr) {
    return empty("corr.err.two");  // the JS throws reading the bounds of nothing (never from the app)
  }
  const double from = jmax(al->start, prof->start);
  const double to = jmin(al->end, prof->end);
  if (!(to - from > 1e-6)) {
    return empty("corr.err.overlap");
  }
  StaSet st;
  st.add(from);
  st.add(to);
  const double step_in = num(A.get("step"));
  const double step = step_in > 0 ? step_in : 10;
  {
    double s = std::ceil(from / step) * step;
    for (std::int64_t n = 0; s < to && static_cast<double>(n) < kMaxSteps; ++n) {
      st.add(base::js_number(base::js_to_fixed(s, 6)));
      s += step;
    }
  }
  for (const auto& k : al->key_stations()) {
    if (k.sta > from && k.sta < to) {
      st.add(k.sta);
    }
  }
  for (const ProfileCurve& c : prof->curves) {
    for (const double s : {c.bvc, c.evc}) {
      if (s > from && s < to) {
        st.add(s);
      }
    }
  }
  Array problems;
  std::optional<SuperPlan> sp;
  const Value sup = A.get("super");
  if (get(sup, "on").truthy()) {
    const Value nc_v = A.get("laneSlope");
    const Value lane_v = A.get("lane");
    sp = super_plan(al, sup, nc_v.is_undefined() ? 0.02 : num(nc_v), lane_v.is_undefined() ? 12 : num(lane_v), words);
    for (const double s : super_key_stations(*sp)) {
      if (s > from && s < to) {
        st.add(base::js_number(base::js_to_fixed(s, 6)));
      }
    }
    for (const auto& p : sp->problems) {
      problems.emplace_back(p);
    }
  }
  std::vector<double> stations;
  for (const double s : st.sorted()) {
    if (stations.empty() || s - stations.back() > 1e-6) {
      stations.push_back(s);
    }
  }
  const SurfaceStats& sb = surface->stats();
  const double far = 2 * hyp(sb.max_x - sb.min_x, sb.max_y - sb.min_y);
  struct Q {
    double x, y, z;
  };
  struct Sec {
    double sta;
    std::optional<SideSlopes> slopes;
    std::array<std::array<Q, 4>, 2> side;  // L, R: cl, eop, sh, dl
    std::array<const char*, 2> kind;
  };
  std::vector<Sec> sections;
  double off = 0;
  for (const double sta : stations) {
    const auto p = al->point_at(sta);
    const auto z0 = prof->elevation_at(sta);
    if (!z0 || !p) {
      continue;
    }
    const double rx = base::js_sin(p->dir);
    const double ry = -base::js_cos(p->dir);
    Sec sec{};
    sec.sta = sta;
    const SideSlopes sl = sp ? slopes_at(*sp, sta, lane_slope) : SideSlopes{lane_slope, lane_slope};
    if (sp) {
      sec.slopes = sl;
    }
    for (std::size_t k = 0; k < 2; ++k) {
      const double sx = k == 0 ? -1 : 1;
      const double ux = rx * sx;
      const double uy = ry * sx;
      const Q cl{p->x, p->y, *z0};
      const double ls = k == 0 ? sl.L : sl.R;
      const double ss = ls > shoulder_slope ? ls : shoulder_slope;
      const Q eop{p->x + ux * lane, p->y + uy * lane, *z0 - lane * ls};
      const Q sh{eop.x + ux * shoulder, eop.y + uy * shoulder, eop.z - shoulder * ss};
      const Cast r = cast_slope(*surface, {sh.x, sh.y}, ux, uy, sh.z, cut, fill, 1, jmax(0.1, step / 20), far);
      if (std::string_view(r.kind) == "off") {
        off++;
      }
      sec.side[k] = {cl, eop, sh, Q{r.x, r.y, r.z}};
      sec.kind[k] = r.kind;
    }
    sections.push_back(sec);
  }
  if (sections.size() < 2) {
    return empty("corr.err.two");
  }
  if (off != 0) {
    problems.emplace_back(words.tn("corr.off", off, {{"n", appkit::num_str(off)}}));
  }
  auto qv = [](const Q& q) { return p3v(q.x, q.y, q.z); };
  auto dlv = [](const Q& q, const char* kind) {
    return make_object({{"x", q.x}, {"y", q.y}, {"z", q.z}, {"kind", std::string(kind)}});
  };
  auto point_of = [&](const Sec& s, std::size_t side, std::size_t k) {
    return k == 3 ? dlv(s.side[side][3], s.kind[side]) : qv(s.side[side][k]);
  };
  auto line = [&](std::size_t side, std::size_t k) {
    Array a;
    for (const Sec& s : sections) {
      a.push_back(point_of(s, side, k));
    }
    return a;
  };
  const std::array<std::pair<const char*, std::pair<std::size_t, std::size_t>>, 7> kLines{{{"CL", {0, 0}},
                                                                                           {"EOP_L", {0, 1}},
                                                                                           {"SH_L", {0, 2}},
                                                                                           {"DL_L", {0, 3}},
                                                                                           {"EOP_R", {1, 1}},
                                                                                           {"SH_R", {1, 2}},
                                                                                           {"DL_R", {1, 3}}}};
  Value lines = Value::object();
  for (const auto& [name, sk] : kLines) {
    lines.append(name, line(sk.first, sk.second));
  }
  Array region;
  std::vector<P2> region_xy;
  for (const Sec& s : sections) {
    region.push_back(qv(s.side[0][3]));
    region_xy.push_back({s.side[0][3].x, s.side[0][3].y});
  }
  for (std::size_t i = sections.size(); i-- > 0;) {
    region.push_back(qv(sections[i].side[1][3]));
    region_xy.push_back({sections[i].side[1][3].x, sections[i].side[1][3].y});
  }
  bool crossing = false;
  for (std::size_t i = 0; i + 1 < sections.size() && !crossing; ++i) {
    for (std::size_t k = 0; k < 2; ++k) {
      const auto& a = sections[i].side[k];
      const auto& b = sections[i + 1].side[k];
      if (seg_cross({a[0].x, a[0].y}, {a[3].x, a[3].y}, {b[0].x, b[0].y}, {b[3].x, b[3].y})) {
        crossing = true;
        break;
      }
    }
  }
  if (crossing || (problems.empty() && self_crosses(region_xy))) {
    problems.emplace_back(words.t("corr.cross"));
  }
  Array points;
  for (const Sec& s : sections) {
    for (std::size_t k = 0; k < 2; ++k) {
      for (std::size_t q = k == 0 ? 0 : 1; q < 4; ++q) {
        points.push_back(qv(s.side[k][q]));
      }
    }
  }
  Array breaklines;
  for (const auto& [name, sk] : kLines) {
    Array pts;
    for (const Sec& s : sections) {
      pts.push_back(qv(s.side[sk.first][sk.second]));
    }
    breaklines.push_back(make_object({{"name", name}, {"pts", std::move(pts)}}));
  }
  for (const Sec& s : sections) {
    Array pts;
    for (std::size_t q = 4; q-- > 0;) {
      pts.push_back(qv(s.side[0][q]));
    }
    for (std::size_t q = 1; q < 4; ++q) {
      pts.push_back(qv(s.side[1][q]));
    }
    breaklines.push_back(make_object({{"name", "section"}, {"pts", std::move(pts)}}));
  }
  Array secs;
  for (const Sec& s : sections) {
    Value o = make_object({{"sta", s.sta}});
    if (s.slopes) {
      o.set("slopes", make_object({{"L", s.slopes->L}, {"R", s.slopes->R}}));
    }
    for (std::size_t k = 0; k < 2; ++k) {
      Array side;
      for (std::size_t q = 0; q < 4; ++q) {
        side.push_back(point_of(s, k, q));
      }
      o.set(k == 0 ? "L" : "R", std::move(side));
    }
    secs.push_back(std::move(o));
  }
  return make_object({{"problems", std::move(problems)},
                      {"region", std::move(region)},
                      {"points", std::move(points)},
                      {"breaklines", std::move(breaklines)},
                      {"info", make_object({{"sections", std::move(secs)},
                                            {"lines", std::move(lines)},
                                            {"from", from},
                                            {"to", to},
                                            {"assembly", A},
                                            {"super", sp ? super_plan_value(*sp) : Value(nullptr)}})}});
}

}  // namespace cc::civil
