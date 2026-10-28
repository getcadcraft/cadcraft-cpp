// SPDX-License-Identifier: Apache-2.0
// featureline.js (CivilCraft 0.5): feature lines with grades, in a profile view and the screen overlay. Lines are JSON
// objects ({ id, name, pts: [{x, y, z}], closed, use, daylight, cut, fill }) as the JS keeps them.
#include "cc/civil/featureline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

#include "cc/appkit/jsops.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/civil/jsmap.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::make_object;
using base::Value;
constexpr double kPi = std::numbers::pi;
constexpr double kTau = 2 * kPi;
constexpr std::size_t kMaxVertices = 10000;

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
bool fin(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number());
}
double px(const Value& p) {
  return num(get(p, "x"));
}
double py(const Value& p) {
  return num(get(p, "y"));
}
double pz(const Value& p) {
  return num(get(p, "z"));
}
double seg_len(const Value& a, const Value& b) {
  return base::js_hypot(px(b) - px(a), py(b) - py(a));
}
double jmax(double a, double b) {
  return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::max(a, b);
}
double jmin(double a, double b) {
  return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::min(a, b);
}
bool closed_of(const Value& f) {
  return get(f, "closed").truthy();
}
/// { ...f, pts }
Value with_pts(const Value& f, Array pts) {
  Value o = f.is_object() ? f : Value::object();
  o.set("pts", std::move(pts));
  return o;
}
/// { ...p, z }
Value with_z(const Value& p, double z) {
  Value o = p.is_object() ? p : Value::object();
  o.set("z", z);
  return o;
}
/// a[i] for a JS index argument: a whole number in range, else nothing
const Value* at_index(const Array& a, const Value& i) {
  if (!i.is_number()) {
    return nullptr;
  }
  const double d = i.as_number();
  if (!(d >= 0) || std::floor(d) != d || d >= static_cast<double>(a.size())) {
    return nullptr;
  }
  return &a[static_cast<std::size_t>(d)];
}

}  // namespace

Value feature_defaults(std::string_view units) {
  return make_object({{"use", "eg"}, {"daylight", false}, {"cut", 2}, {"fill", 3}, {"offset", units == "m" ? 1 : 3}});
}

Value clean_feature(const Value& o, std::string_view units, const i18n::Catalog& words) {
  if ((!o.is_object() && !o.is_array()) || !get(o, "pts").is_array()) {
    return nullptr;
  }
  Array pts;
  const Array& in = arr(get(o, "pts"));
  for (std::size_t i = 0; i < in.size() && i < kMaxVertices; ++i) {
    const Value& p = in[i];
    const Value x = get(p, "x");
    const Value y = get(p, "y");
    const Value z = get(p, "z");
    if (p.truthy() && fin(x) && fin(y) && std::abs(x.as_number()) < 1e9 && std::abs(y.as_number()) < 1e9) {
      pts.push_back(
          base::pt3(x.as_number(), y.as_number(), fin(z) && std::abs(z.as_number()) < 1e6 ? z.as_number() : 0));
    }
  }
  if (pts.size() < 2) {
    return nullptr;
  }
  const Value d = feature_defaults(units);
  const bool closed = get(o, "closed").is_bool() && get(o, "closed").as_bool() && pts.size() >= 3;
  const std::string id = base::js_string(get(o, "id"));
  bool id_ok = id.size() >= 2 && id.size() <= 10 && id[0] == 'f';
  for (std::size_t i = 1; id_ok && i < id.size(); ++i) {
    id_ok = id[i] >= '0' && id[i] <= '9';
  }
  const Value nm = get(o, "name");
  std::string name = (nm.is_null() || nm.is_undefined()) ? std::string() : base::js_string(nm);
  for (char& ch : name) {
    const auto u = static_cast<unsigned char>(ch);
    if (u < 0x20 || u == 0x7F) {
      ch = ' ';
    }
  }
  name = std::string(base::js_trim(name));
  if (const auto u16 = base::to_utf16(name); u16.size() > 120) {
    std::string cut;
    base::append_utf16(cut, std::span(u16).first(120));
    name = cut;
  }
  const Value use = get(o, "use");
  const bool use_ok =
      use.is_string() && (use.as_string() == "eg" || use.as_string() == "design" || use.as_string() == "off");
  const Value cut = get(o, "cut");
  const Value fill = get(o, "fill");
  return make_object({{"id", id_ok ? Value(id) : Value(nullptr)},
                      {"name", name.empty() ? words.t("toolbar.feature") : name},
                      {"pts", std::move(pts)},
                      {"closed", closed},
                      {"use", use_ok ? use : d.get("use")},
                      {"daylight", get(o, "daylight").is_bool() && get(o, "daylight").as_bool()},
                      {"cut", fin(cut) && cut.as_number() > 0 && cut.as_number() < 1000 ? cut : d.get("cut")},
                      {"fill", fin(fill) && fill.as_number() > 0 && fill.as_number() < 1000 ? fill : d.get("fill")}});
}

std::vector<double> stations(const Value& f) {
  std::vector<double> out{0};
  const Array& P = arr(get(f, "pts"));
  const std::size_t n = P.size();
  const std::size_t m = closed_of(f) || n == 0 ? n : n - 1;
  for (std::size_t i = 0; i < m; ++i) {
    out.push_back(out[i] + seg_len(P[i], P[(i + 1) % n]));
  }
  return out;
}

std::vector<FlSegment> segments(const Value& f) {
  const Array& P = arr(get(f, "pts"));
  const std::size_t n = P.size();
  const std::size_t m = closed_of(f) || n == 0 ? n : n - 1;
  std::vector<FlSegment> out;
  for (std::size_t i = 0; i < m; ++i) {
    const std::size_t j = (i + 1) % n;
    const double len = seg_len(P[i], P[j]);
    const double dz = pz(P[j]) - pz(P[i]);
    out.push_back({i, j, len, dz, len > 1e-12 ? std::optional(dz / len) : std::nullopt});
  }
  return out;
}

Value segments_value(const std::vector<FlSegment>& s) {
  Array out;
  for (const FlSegment& g : s) {
    out.push_back(make_object(
        {{"i", g.i}, {"j", g.j}, {"len", g.len}, {"dz", g.dz}, {"grade", g.grade ? Value(*g.grade) : Value(nullptr)}}));
  }
  return out;
}

Value set_elevation(const Value& f, const Value& i, const Value& z) {
  const Array& P = arr(get(f, "pts"));
  const Value* p = at_index(P, i);
  if (!fin(z) || p == nullptr || !p->truthy()) {
    return f;
  }
  Array pts;
  for (std::size_t k = 0; k < P.size(); ++k) {
    pts.push_back(static_cast<double>(k) == i.as_number() ? with_z(P[k], z.as_number()) : P[k]);
  }
  return with_pts(f, std::move(pts));
}

Value set_grade(const Value& f, const Value& i, const Value& grade) {
  const auto segs = segments(f);
  if (!i.is_number() || !(i.as_number() >= 0) || std::floor(i.as_number()) != i.as_number() ||
      i.as_number() >= static_cast<double>(segs.size())) {
    return f;
  }
  const FlSegment& s = segs[static_cast<std::size_t>(i.as_number())];
  if (!fin(grade) || !(s.len > 1e-12)) {
    return f;
  }
  const Array& P = arr(get(f, "pts"));
  return set_elevation(f, Value(static_cast<double>(s.j)), pz(P[s.i]) + grade.as_number() * s.len);
}

Value raise(const Value& f, const Value& dz) {
  if (!fin(dz)) {
    return f;
  }
  Array pts;
  for (const Value& p : arr(get(f, "pts"))) {
    pts.push_back(with_z(p, pz(p) + dz.as_number()));
  }
  return with_pts(f, std::move(pts));
}

Value even_grade(const Value& f, const Value& iv, const Value& jv) {
  const Array& P = arr(get(f, "pts"));
  const auto n = static_cast<double>(P.size());
  const double i = num(iv);
  const double j = num(jv);
  const bool span = i >= 0 && j < n && j > i + 1;  // (NaN: none)
  if (!span) {
    return f;
  }
  const std::vector<double> st = stations(f);
  auto sat = [&](double k) {
    return (k >= 0 && std::floor(k) == k && k < static_cast<double>(st.size())) ? st[static_cast<std::size_t>(k)]
                                                                                : std::nan("");
  };
  const double L = sat(j) - sat(i);
  if (!(L > 1e-12)) {
    return f;
  }
  const Value* pi = at_index(P, Value(i));
  const Value* pj = at_index(P, Value(j));
  const double z0 = pi != nullptr ? pz(*pi) : std::nan("");
  const double z1 = pj != nullptr ? pz(*pj) : std::nan("");
  Array pts;
  for (std::size_t k = 0; k < P.size(); ++k) {
    const auto kd = static_cast<double>(k);
    pts.push_back(kd > i && kd < j ? with_z(P[k], z0 + ((z1 - z0) * (st[k] - sat(i))) / L) : P[k]);
  }
  return with_pts(f, std::move(pts));
}

Value from_surface(const Value& f, const Surface* surface, const Value& which) {
  Array missing;
  Array pts;
  const Array& P = arr(get(f, "pts"));
  for (std::size_t k = 0; k < P.size(); ++k) {
    if (which.truthy()) {
      const bool in = std::ranges::any_of(
          arr(which), [&](const Value& w) { return w.is_number() && w.as_number() == static_cast<double>(k); });
      if (!in) {
        pts.push_back(P[k]);
        continue;
      }
    }
    const auto z = surface != nullptr ? surface->elevation_at(px(P[k]), py(P[k]), 1e-6) : std::nullopt;
    if (!z) {
      missing.emplace_back(static_cast<double>(k));
      pts.push_back(P[k]);
      continue;
    }
    pts.push_back(with_z(P[k], *z));
  }
  return make_object({{"f", with_pts(f, std::move(pts))}, {"missing", std::move(missing)}});
}

Value offset_feature(const Value& f, const Value& dist_v, const Value& opts, const i18n::Catalog& words) {
  auto fail = [&](const char* key) { return make_object({{"f", nullptr}, {"problem", words.t(key)}}); };
  if (!fin(dist_v) || dist_v.as_number() == 0) {
    return fail("fl.offset.zero");
  }
  const double dist = dist_v.as_number();
  const Value dz = get(opts, "dz");
  const Value slope = get(opts, "slope");
  double raise_by = 0;
  if (fin(dz)) {
    raise_by = dz.as_number();
  } else if (fin(slope)) {
    raise_by = std::abs(dist) * slope.as_number();
  }
  const Array& P = arr(get(f, "pts"));
  const std::size_t n = P.size();
  const bool closed = closed_of(f);
  struct N {
    double x, y;
  };
  const std::size_t m = closed || n == 0 ? n : n - 1;
  std::vector<N> seg_n;
  for (std::size_t i = 0; i < m; ++i) {
    const Value& a = P[i];
    const Value& b = P[(i + 1) % n];
    const double L = seg_len(a, b);
    if (!(L > 1e-12)) {
      return fail("fl.offset.repeat");
    }
    seg_n.push_back({(py(b) - py(a)) / L, -(px(b) - px(a)) / L});
  }
  Array out;
  std::vector<P2> o2;
  for (std::size_t i = 0; i < n; ++i) {
    std::optional<N> n_in;
    std::optional<N> n_out;
    if (closed) {
      if (m != 0) {
        n_in = seg_n[(i + m - 1) % m];
        n_out = seg_n[i % m];
      }
    } else {
      if (i >= 1 && i - 1 < seg_n.size()) {
        n_in = seg_n[i - 1];
      }
      if (i < seg_n.size()) {
        n_out = seg_n[i];
      }
    }
    double ox = 0;
    double oy = 0;
    if (!n_in) {
      if (!n_out) {
        throw std::runtime_error("Cannot read properties of undefined (reading 'x')");
      }
      ox = n_out->x * dist;
      oy = n_out->y * dist;
    } else if (!n_out) {
      ox = n_in->x * dist;
      oy = n_in->y * dist;
    } else {
      const double bx = n_in->x + n_out->x;
      const double by = n_in->y + n_out->y;
      const double c = (bx * n_out->x + by * n_out->y) / base::js_hypot(bx, by);
      if (base::js_hypot(bx, by) < 1e-9 || c < 0.1) {
        return fail("fl.offset.sharp");
      }
      const double L = base::js_hypot(bx, by);
      ox = (bx / L) * (dist / c);
      oy = (by / L) * (dist / c);
    }
    const double x = px(P[i]) + ox;
    const double y = py(P[i]) + oy;
    out.push_back(base::pt3(x, y, pz(P[i]) + raise_by));
    o2.push_back({x, y});
  }
  for (std::size_t i = 0; i < m; ++i) {
    const Value& a = P[i];
    const Value& b = P[(i + 1) % n];
    const P2 c = o2[i];
    const P2 d = o2[(i + 1) % n];
    if ((px(b) - px(a)) * (d.x - c.x) + (py(b) - py(a)) * (d.y - c.y) <= 0) {
      return fail("fl.offset.fold");
    }
  }
  Value g = f.is_object() ? f : Value::object();
  g.set("id", nullptr);
  g.set("pts", std::move(out));
  return make_object({{"f", std::move(g)}, {"problem", nullptr}});
}

Value feature_breaklines(const Value& features, const Value& use) {
  Array out;
  for (const Value& f : arr(features)) {
    const Value u = get(f, "use");
    const Array& P = arr(get(f, "pts"));
    if (!strict_equal(u, use) || P.size() < 2) {
      continue;
    }
    Array pts;
    for (const Value& p : P) {
      pts.push_back(base::pt3(px(p), py(p), pz(p)));
    }
    if (closed_of(f)) {
      pts.push_back(base::pt3(px(P[0]), py(P[0]), pz(P[0])));
    }
    out.push_back(make_object({{"name", get(f, "name")}, {"pts", std::move(pts)}, {"feature", get(f, "id")}}));
  }
  return out;
}

Value feature_item(const Value& f) {
  const Value use = get(f, "use");
  const Array& P = arr(get(f, "pts"));
  if (!(use.is_string() && use.as_string() == "design") || !closed_of(f) || P.size() < 3) {
    return nullptr;
  }
  Array pts;
  double sum = 0;
  for (const Value& p : P) {
    pts.push_back(base::pt3(px(p), py(p), pz(p)));
    sum += pz(p);
  }
  const double z = sum / static_cast<double>(pts.size());
  if (get(f, "daylight").truthy()) {
    return make_object({{"id", get(f, "id")},
                        {"kind", "feature"},
                        {"name", get(f, "name")},
                        {"pts", std::move(pts)},
                        {"z", z},
                        {"cut", get(f, "cut")},
                        {"fill", get(f, "fill")}});
  }
  Array closed_pts = pts;
  closed_pts.push_back(pts[0]);
  const Value patch =
      make_object({{"problems", Array{}},
                   {"region", pts},
                   {"points", Array{}},
                   {"breaklines", Array{make_object({{"name", get(f, "name")}, {"pts", std::move(closed_pts)}})}},
                   {"info", make_object({{"poly", pts}})}});
  return make_object({{"id", get(f, "id")}, {"kind", "feature"}, {"name", get(f, "name")}, {"patch", patch}});
}

std::string grade_label(const Value& g) {
  if (!fin(g)) {
    return "—";
  }
  return std::string(g.as_number() >= 0 ? "+" : "") + base::js_to_fixed(g.as_number() * 100, 2) + "%";
}

namespace {
struct Cross {
  double t, sta;
};
std::vector<Cross> cross_element(const Element& e, P2 a, P2 b) {
  std::vector<Cross> out;
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  if (!e.arc) {
    const double ex = e.p1.x - e.p0.x;
    const double ey = e.p1.y - e.p0.y;
    const double den = dx * ey - dy * ex;
    if (std::abs(den) < 1e-15) {
      return out;
    }
    const double wx = e.p0.x - a.x;
    const double wy = e.p0.y - a.y;
    const double t = (wx * ey - wy * ex) / den;
    const double u = (wx * dy - wy * dx) / den;
    if (t >= -1e-12 && t <= 1 + 1e-12 && u >= -1e-12 && u <= 1 + 1e-12) {
      out.push_back({jmin(1, jmax(0, t)), e.sta + jmin(1, jmax(0, u)) * e.len});
    }
    return out;
  }
  const double fx = a.x - e.c.x;
  const double fy = a.y - e.c.y;
  const double A = dx * dx + dy * dy;
  const double B = 2 * (fx * dx + fy * dy);
  const double C = fx * fx + fy * fy - e.r * e.r;
  const double disc = B * B - 4 * A * C;
  if (A < 1e-24 || disc < 0) {
    return out;
  }
  const double sq = std::sqrt(disc);
  std::vector<double> ts;
  if (disc == 0) {
    ts.push_back(-B / (2 * A));
  } else {
    ts.push_back((-B - sq) / (2 * A));
    ts.push_back((-B + sq) / (2 * A));
  }
  for (const double t : ts) {
    if (t < -1e-12 || t > 1 + 1e-12) {
      continue;
    }
    const double x = a.x + t * dx;
    const double y = a.y + t * dy;
    double sw = std::fmod((base::js_atan2(y - e.c.y, x - e.c.x) - e.a0) * (e.ccw ? 1 : -1), kTau);
    if (sw < 0) {
      sw += kTau;
    }
    if (sw > e.delta + 1e-12) {
      continue;
    }
    out.push_back({jmin(1, jmax(0, t)), e.sta + jmin(sw, e.delta) * e.r});
  }
  return out;
}
}  // namespace

std::vector<FlCrossing> alignment_crossings(const Alignment* al, const Value& f) {
  const Array& P = arr(get(f, "pts"));
  if (al == nullptr || al->elements.empty() || P.empty()) {
    return {};
  }
  std::vector<FlCrossing> out;
  for (const FlSegment& sg : segments(f)) {
    if (!(sg.len > 1e-12)) {
      continue;
    }
    const Value& a = P[sg.i];
    const Value& b = P[sg.j];
    for (const Element& e : al->elements) {
      for (const Cross& c : cross_element(e, {px(a), py(a)}, {px(b), py(b)})) {
        out.push_back({c.sta, pz(a) + c.t * sg.dz, sg.i, sg.grade});
      }
    }
  }
  appkit::js_sort(out, [](const FlCrossing& p, const FlCrossing& q) { return p.sta - q.sta; });
  std::vector<FlCrossing> kept;
  for (std::size_t k = 0; k < out.size(); ++k) {
    if (k == 0 || std::abs(out[k].sta - out[k - 1].sta) > 1e-6 || std::abs(out[k].z - out[k - 1].z) > 1e-6) {
      kept.push_back(out[k]);
    }
  }
  return kept;
}

std::vector<std::vector<FlRunPt>> projected_runs(const Alignment* al, const Value& f, double within) {
  const Array& P = arr(get(f, "pts"));
  if (!(within > 0) || al == nullptr || al->elements.empty() || P.empty()) {
    return {};
  }
  std::vector<std::size_t> idx(P.size());
  for (std::size_t i = 0; i < idx.size(); ++i) {
    idx[i] = i;
  }
  if (closed_of(f)) {
    idx.push_back(0);
  }
  const auto cross = alignment_crossings(al, f);
  std::vector<std::vector<FlRunPt>> runs;
  std::vector<FlRunPt> run;
  auto flush = [&] {
    if (run.size() >= 2) {
      double lo = run[0].sta;
      double hi = run[0].sta;
      for (const FlRunPt& q : run) {
        lo = jmin(lo, q.sta);
        hi = jmax(hi, q.sta);
      }
      if (hi - lo > 1e-6) {
        runs.push_back(run);
      }
    }
    run.clear();
  };
  for (std::size_t k = 0; k < idx.size(); ++k) {
    const Value& p = P[idx[k]];
    const auto so = al->station_offset(px(p), py(p));
    const bool inside = so && !so->outside && std::abs(so->off) <= within + 1e-9;
    if (!inside) {
      flush();
      continue;
    }
    if (k > 0 && !run.empty()) {
      for (const FlCrossing& c : cross) {
        if (c.i == idx[k - 1]) {
          run.push_back({c.sta, c.z, 0});
        }
      }
    }
    run.push_back({so->sta, pz(p), so->off});
  }
  flush();
  for (auto& r : runs) {
    if (r.back().sta < r.front().sta) {
      std::ranges::reverse(r);
    }
  }
  return runs;
}

Value crossings_value(const std::vector<FlCrossing>& c) {
  Array out;
  for (const FlCrossing& x : c) {
    out.push_back(
        make_object({{"sta", x.sta}, {"z", x.z}, {"i", x.i}, {"grade", x.grade ? Value(*x.grade) : Value(nullptr)}}));
  }
  return out;
}
Value runs_value(const std::vector<std::vector<FlRunPt>>& runs) {
  Array out;
  for (const auto& r : runs) {
    Array a;
    for (const FlRunPt& q : r) {
      a.push_back(make_object({{"sta", q.sta}, {"z", q.z}, {"off", q.off}}));
    }
    out.emplace_back(std::move(a));
  }
  return out;
}

Value feature_profile(const Alignment* al, const Value& features, double within) {
  Array out;
  for (const Value& f : arr(features)) {
    const auto c = alignment_crossings(al, f);
    const auto r = projected_runs(al, f, within);
    if (!c.empty() || !r.empty()) {
      out.push_back(make_object({{"id", get(f, "id")},
                                 {"name", get(f, "name")},
                                 {"use", get(f, "use")},
                                 {"crossings", crossings_value(c)},
                                 {"runs", runs_value(r)}}));
    }
  }
  return out;
}

Value grade_overlay(const Value& features, double k, double ox, double oy, double min_seg, double min_gap,
                    const Value& skip) {
  Array out;
  std::vector<P2> placed;
  for (const Value& f : arr(features)) {
    const Array& P = arr(get(f, "pts"));
    const Value id = get(f, "id");
    if (!f.truthy() || P.empty() || strict_equal(id, skip)) {
      continue;
    }
    std::vector<P2> S;
    for (const Value& p : P) {
      S.push_back({(px(p) - ox) * k, -(py(p) - oy) * k});
    }
    for (const FlSegment& sg : segments(f)) {
      const P2 a = S[sg.i];
      const P2 b = S[sg.j];
      const double len = base::js_hypot(b.x - a.x, b.y - a.y);
      if (len < min_seg || !sg.grade) {
        continue;
      }
      double ang = base::js_atan2(b.y - a.y, b.x - a.x);
      const double along = ang;
      if (std::abs(ang) > kPi / 2) {
        ang += ang > 0 ? -kPi : kPi;
      }
      const bool level = std::abs(*sg.grade) < 5e-5;
      out.push_back(make_object({{"kind", "grade"},
                                 {"x", (a.x + b.x) / 2},
                                 {"y", (a.y + b.y) / 2},
                                 {"ang", ang},
                                 {"text", base::js_to_fixed(std::abs(*sg.grade) * 100, 2) + "%"},
                                 {"down", level ? Value(nullptr) : Value(*sg.grade < 0 ? along : along + kPi)}}));
    }
    for (std::size_t i = 0; i < P.size(); ++i) {
      const P2 q = S[i];
      if (std::ranges::any_of(placed, [&](const P2& o) { return base::js_hypot(o.x - q.x, o.y - q.y) < min_gap; })) {
        continue;
      }
      placed.push_back(q);
      out.push_back(make_object({{"kind", "elev"},
                                 {"x", q.x},
                                 {"y", q.y},
                                 {"text", base::js_to_fixed(base::js_round(pz(P[i]) * 100) / 100, 2)}}));
    }
  }
  return out;
}

}  // namespace cc::civil
