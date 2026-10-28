// SPDX-License-Identifier: Apache-2.0
// masshaul.js, solve-why.js and staking.js (CivilCraft 0.5), ported function by function.
#include "cc/civil/survey.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <set>

#include "cc/appkit/jsops.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/civil/units.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::make_object;
using base::Value;
constexpr double kPi = std::numbers::pi;
constexpr double kInf = std::numeric_limits<double>::infinity();

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
double or0(double v) {
  return (std::isnan(v) || v == 0) ? 0 : v;
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
std::string strip_commas(std::string s) {
  std::erase(s, ',');
  return s;
}
std::string csv_q(const std::string& x) {
  if (x.find_first_of("\",\r\n") == std::string::npos) {
    return x;
  }
  std::string o = "\"";
  for (const char c : x) {
    if (c == '"') {
      o += "\"\"";
    } else {
      o.push_back(c);
    }
  }
  return o + "\"";
}
std::string rows_csv(const std::vector<std::vector<std::string>>& rows) {
  std::string out;
  for (std::size_t r = 0; r < rows.size(); ++r) {
    if (r != 0) {
      out += "\r\n";
    }
    for (std::size_t i = 0; i < rows[r].size(); ++i) {
      out += (i != 0 ? "," : "") + csv_q(rows[r][i]);
    }
  }
  return out + "\r\n";
}

}  // namespace

// ---------------------------------------------------------------------------------------------- masshaul.js

Value mass_haul(const Value& rows_v, const Value& factor) {
  const double k = fin(factor) && factor.as_number() > 0 && factor.as_number() < 5 ? factor.as_number() : 1;
  struct Pt {
    double sta, m;
  };
  std::vector<Pt> points;
  double m = 0;
  double cut = 0;
  double fill = 0;
  const Array& rows = arr(rows_v);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const Value& r = rows[i];
    if (i > 0) {
      const double cv = or0(num(get(r, "cutVol")));
      const double fv = or0(num(get(r, "fillVol")));
      m += cv * k - fv;
      cut += cv;
      fill += fv;
    }
    points.push_back({num(get(r, "sta")), m});
  }
  std::vector<double> balance;
  for (std::size_t i = 1; i < points.size(); ++i) {
    const Pt a = points[i - 1];
    const Pt b = points[i];
    if ((a.m > 0 && b.m < 0) || (a.m < 0 && b.m > 0)) {
      balance.push_back(a.sta + ((b.sta - a.sta) * a.m) / (a.m - b.m));
    } else if (b.m == 0 && a.m != 0 && i < points.size() - 1) {
      balance.push_back(b.sta);
    }
  }
  std::vector<double> cuts;
  if (!points.empty()) {
    cuts.push_back(points.front().sta);
  }
  cuts.insert(cuts.end(), balance.begin(), balance.end());
  if (!points.empty()) {
    cuts.push_back(points.back().sta);
  }
  auto at = [&](double s) {
    for (std::size_t i = 1; i < points.size(); ++i) {
      if (s <= points[i].sta + 1e-9) {
        const Pt a = points[i - 1];
        const Pt b = points[i];
        const double f = b.sta > a.sta ? (s - a.sta) / (b.sta - a.sta) : 0;
        return a.m + (b.m - a.m) * f;
      }
    }
    return points.empty() ? 0.0 : points.back().m;
  };
  Array spans;
  for (std::size_t j = 1; j < cuts.size(); ++j) {
    const double s0 = cuts[j - 1];
    const double s1 = cuts[j];
    if (!(s1 > s0)) {
      continue;
    }
    std::vector<Pt> pts{{s0, at(s0)}};
    for (const Pt& p : points) {
      if (p.sta > s0 + 1e-9 && p.sta < s1 - 1e-9) {
        pts.push_back(p);
      }
    }
    pts.push_back({s1, at(s1)});
    const double e = pts.back().m;
    double peak = 0;
    for (const Pt& q : pts) {
      if (std::abs(q.m) > std::abs(peak)) {
        peak = q.m;
      }
    }
    const double volume = std::abs(peak) - std::abs(e);
    if (!(volume > 1e-9)) {
      continue;
    }
    const double sg = sign(peak);
    auto ex = [&](double v) { return jmax(0, sg * v - std::abs(e)); };
    double area = 0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
      const Pt a = pts[i - 1];
      const Pt b = pts[i];
      const double fa = ex(a.m);
      const double fb = ex(b.m);
      const double L = b.sta - a.sta;
      if (fa > 0 && fb > 0) {
        area += ((fa + fb) / 2) * L;
      } else if (fa > 0 || fb > 0) {
        const double ga = sg * a.m - std::abs(e);
        const double gb = sg * b.m - std::abs(e);
        const double x = (ga / (ga - gb)) * L;
        area += fa > 0 ? (fa * x) / 2 : (fb * (L - x)) / 2;
      }
    }
    spans.push_back(make_object(
        {{"from", s0}, {"to", s1}, {"volume", volume}, {"dir", peak > 0 ? "ahead" : "back"}, {"haul", area / volume}}));
  }
  Array pv;
  for (const Pt& p : points) {
    pv.push_back(make_object({{"sta", p.sta}, {"m", p.m}}));
  }
  Array bv;
  for (const double b : balance) {
    bv.emplace_back(b);
  }
  const double end = m;
  const char* kind = "borrow";
  if (std::abs(end) < 1e-6 * jmax(1, cut + fill)) {
    kind = "balanced";
  } else if (end > 0) {
    kind = "waste";
  }
  return make_object(
      {{"points", std::move(pv)},
       {"balance", std::move(bv)},
       {"spans", std::move(spans)},
       {"totals", make_object({{"cut", cut}, {"cutAdj", cut * k}, {"fill", fill}, {"end", end}, {"kind", kind}})},
       {"factor", k}});
}

std::string mass_haul_csv(const Value& res, std::string_view units, const i18n::Catalog& w) {
  const Unit& u = unit_of(units);
  auto v = [&](double x) { return strip_commas(fixed(x / u.vol_per, 1)); };
  auto d = [&](double x) { return strip_commas(fixed(x, 2)); };
  const std::string vol(u.vol);
  std::vector<std::vector<std::string>> rows{{w.t("mh.c.sta"), w.t("mh.c.m", {{"u", vol}})}};
  for (const Value& p : arr(get(res, "points"))) {
    rows.push_back({appkit::num_str(num(get(p, "sta"))), v(num(get(p, "m")))});
  }
  rows.emplace_back();
  rows.push_back({w.t("mh.c.from"), w.t("mh.c.to"), w.t("mh.c.vol", {{"u", vol}}), w.t("mh.c.dir"),
                  w.t("mh.c.haul", {{"u", units == "m" ? "m" : "ft"}})});
  for (const Value& s : arr(get(res, "spans"))) {
    rows.push_back({appkit::num_str(num(get(s, "from"))), appkit::num_str(num(get(s, "to"))), v(num(get(s, "volume"))),
                    w.t("mh.dir." + base::js_string(get(s, "dir"))), d(num(get(s, "haul")))});
  }
  rows.emplace_back();
  rows.push_back({w.t("mh.c.factor"), base::js_string(get(res, "factor"))});
  rows.push_back({w.t("mh.c.end", {{"u", vol}}), v(num(get(get(res, "totals"), "end")))});
  return rows_csv(rows);
}

// ---------------------------------------------------------------------------------------------- solve-why.js

namespace {
struct Curve {
  double d = 0, r = 0, T = 0, tan_half = 0;
  bool back = false;
};
double turn(P2 a, P2 b, P2 c) {
  const double a1 = base::js_atan2(b.y - a.y, b.x - a.x);
  const double a2 = base::js_atan2(c.y - b.y, c.x - b.x);
  double d = a2 - a1;
  for (int g = 0; d > kPi && g < 64; ++g) {
    d -= 2 * kPi;
  }
  for (int g = 0; d < -kPi && g < 64; ++g) {
    d += 2 * kPi;
  }
  return d;
}
double down(double v) {
  return std::floor(v * 100 + 1e-9) / 100;
}
double up(double v) {
  return std::ceil(v * 100 - 1e-9) / 100;
}
double dist(P2 p, P2 q) {
  return base::js_hypot(q.x - p.x, q.y - p.y);
}
struct Pis {
  std::vector<P2> p;
  std::vector<Value> r;
};
Pis pis_of(const Value& v, bool finite_only) {
  Pis out;
  for (const Value& p : arr(v)) {
    const Value x = get(p, "x");
    const Value y = get(p, "y");
    if (finite_only && !(fin(x) && fin(y))) {
      continue;
    }
    out.p.push_back({num(x), num(y)});
    out.r.push_back(get(p, "r"));
  }
  return out;
}
std::vector<Curve> curves_of(const Pis& P) {
  std::vector<Curve> out;
  const std::size_t n = P.p.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (i == 0 || i == n - 1) {
      out.push_back({});
      continue;
    }
    const double d = turn(P.p[i - 1], P.p[i], P.p[i + 1]);
    const double rr = num(P.r[i]);
    const double r = rr > 0 ? rr : 0;
    const double tan_half = base::js_tan(std::abs(d) / 2);
    const bool back = std::abs(std::abs(d) - kPi) < 1e-6;
    out.push_back({d, r, back || std::abs(d) < 1e-9 ? 0 : r * tan_half, tan_half, back});
  }
  return out;
}
double max_radius_of(const Pis& P, const std::vector<Curve>& C, std::size_t i) {
  if (i >= C.size() || i == 0 || i == P.p.size() - 1 || C[i].back) {
    return 0;
  }
  if (std::abs(C[i].d) < 1e-9) {
    return kInf;
  }
  const double room = jmin(dist(P.p[i - 1], P.p[i]) - C[i - 1].T, dist(P.p[i], P.p[i + 1]) - C[i + 1].T);
  return room <= 0 ? 0 : room / C[i].tan_half;
}
}  // namespace

double max_radius(const Value& pis, const Value& iv) {
  const Pis P = pis_of(pis, false);
  const auto C = curves_of(P);
  const double i = num(iv);
  if (!(i >= 0) || std::floor(i) != i || i >= static_cast<double>(C.size())) {
    return 0;  // C[i] is undefined
  }
  return max_radius_of(P, C, static_cast<std::size_t>(i));
}

Value curve_why(const Value& pis, const Value& opts) {
  const Pis P = pis_of(pis, true);
  Array out;
  const std::size_t n = P.p.size();
  if (n < 3) {
    return out;
  }
  const auto C = curves_of(P);
  for (std::size_t i = 0; i < n; ++i) {
    if (C[i].back && C[i].r > 0) {
      out.push_back(make_object({{"kind", "back"}, {"pi", i}}));
    }
  }
  auto inner = [&](std::size_t i) { return i > 0 && i < n - 1 && !C[i].back && std::abs(C[i].d) > 1e-9; };
  for (std::size_t a = 0; a + 1 < n; ++a) {
    const std::size_t b = a + 1;
    const double L = dist(P.p[a], P.p[b]);
    const double Ta = C[a].T;
    const double Tb = C[b].T;
    if (Ta + Tb <= L + 1e-9) {
      continue;
    }
    auto fix = [&](std::size_t i, double other) -> Value {
      if (!inner(i)) {
        return nullptr;
      }
      const double here = L - other;
      const double far = i == a ? dist(P.p[a - 1], P.p[a]) - C[a - 1].T : dist(P.p[b], P.p[b + 1]) - C[b + 1].T;
      const double room = jmin(here, far);
      return room > 0 ? Value(down(room / C[i].tan_half)) : Value(nullptr);
    };
    const double f = L / (Ta + Tb);
    out.push_back(make_object({{"kind", "overlap"},
                               {"a", a},
                               {"b", b},
                               {"dist", L},
                               {"need", Ta + Tb},
                               {"Ta", Ta},
                               {"Tb", Tb},
                               {"fixA", fix(a, Tb)},
                               {"fixB", fix(b, Ta)},
                               {"both", make_object({{"ra", inner(a) ? down(C[a].r * f) : 0.0},
                                                     {"rb", inner(b) ? down(C[b].r * f) : 0.0}})}}));
  }
  const Value mr = get(opts, "minR");
  const double min_r = mr.is_undefined() ? 0 : num(mr);
  if (min_r > 0) {
    for (std::size_t i = 0; i < n; ++i) {
      const Curve& c = C[i];
      if (i == 0 || i == n - 1 || !(c.r > 0) || c.r >= min_r - 1e-9 || c.back) {
        continue;
      }
      const double max_r = max_radius_of(P, C, i);
      out.push_back(make_object({{"kind", "min"},
                                 {"pi", i},
                                 {"r", c.r},
                                 {"minR", mr},
                                 {"maxR", max_r},
                                 {"fix", min_r <= max_r + 1e-9 ? Value(up(min_r)) : Value(nullptr)}}));
    }
  }
  return out;
}

Value vertical_why(const Value& pvis_in) {
  struct V {
    Value src;
    double sta, z;
    double i0;
  };
  std::vector<V> pvis;
  {
    double i0 = 0;
    for (const Value& p : arr(pvis_in)) {
      const Value s = get(p, "sta");
      const Value z = get(p, "z");
      if (fin(s) && fin(z)) {
        pvis.push_back({p, s.as_number(), z.as_number(), i0});
        i0 += 1;
      }
    }
  }
  appkit::js_sort(pvis, [](const V& a, const V& b) { return a.sta - b.sta; });
  Array out;
  double prev_end = pvis.empty() ? 0 : pvis[0].sta;
  for (std::size_t i = 1; i + 1 < pvis.size(); ++i) {
    const V& v = pvis[i];
    const double g1 = (v.z - pvis[i - 1].z) / (v.sta - pvis[i - 1].sta);
    const double g2 = (pvis[i + 1].z - v.z) / (pvis[i + 1].sta - v.sta);
    const double A = (g2 - g1) * 100;
    double L = or0(num(get(v.src, "L")));
    const double K = num(get(v.src, "K"));
    if (L == 0 && K > 0) {
      L = K * std::abs(A);
    }
    if (!(L > 0) || std::abs(A) < 1e-9) {
      continue;
    }
    const double back = v.sta - jmax(pvis[i - 1].sta, prev_end);
    const double fwd = pvis[i + 1].sta - v.sta;
    const double room = 2 * jmin(back, fwd);
    if (L > room + 1e-9) {
      const std::optional<double> fx = room > 0 ? std::optional(down(room)) : std::nullopt;
      out.push_back(make_object({{"kind", "vc"},
                                 {"pvi", i},
                                 {"index", v.i0},
                                 {"L", L},
                                 {"room", room},
                                 {"fix", fx ? Value(*fx) : Value(nullptr)},
                                 {"K", fx && *fx != 0 ? Value(*fx / std::abs(A)) : Value(nullptr)},
                                 {"side", back < fwd ? "before" : "past"}}));
      prev_end = v.sta + fx.value_or(0) / 2;
    } else {
      prev_end = v.sta + L / 2;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------------------------- staking.js

namespace {
constexpr double kEps = 1e-9;
std::optional<P2> place(const Alignment& al, double sta, double off) {
  const auto p = al.point_at(sta);
  if (!p) {
    return std::nullopt;
  }
  return P2{p->x + off * base::js_sin(p->dir), p->y - off * base::js_cos(p->dir)};
}
Value num_or_null(const std::optional<double>& v) {
  return v && std::isfinite(*v) ? Value(*v) : Value(nullptr);
}
std::string fixed_num(const Value& v, int d) {
  return fin(v) ? base::js_to_fixed(v.as_number(), d) : std::string();
}
}  // namespace

std::vector<double> stake_stations(const Alignment& al, const Value& opts) {
  const Value fv = get(opts, "from");
  const Value tv = get(opts, "to");
  const Value sv = get(opts, "step");
  const Value kv = get(opts, "keys");
  const double from = fv.is_undefined() ? al.start : num(fv);
  const double to = tv.is_undefined() ? al.end : num(tv);
  const double step = sv.is_undefined() ? 50 : num(sv);
  const bool keys = kv.is_undefined() ? true : kv.truthy();
  const double a = jmax(al.start, jmin(from, to));
  const double b = jmin(al.end, jmax(from, to));
  std::vector<double> set;
  std::set<double> seen;
  auto add = [&](double s) {
    const double k = s == 0 ? 0 : s;
    if (std::isnan(k) ? std::ranges::none_of(set, [](double x) { return std::isnan(x); }) : seen.insert(k).second) {
      set.push_back(k);
    }
  };
  add(a);
  add(b);
  if (step > 0) {
    const double k0 = std::ceil((a - kEps) / step);
    for (std::int64_t q = 0; (k0 + static_cast<double>(q)) * step <= b + kEps && q < 10000000; ++q) {
      add(jmin(b, jmax(a, (k0 + static_cast<double>(q)) * step)));
    }
  }
  if (keys) {
    for (const Element& e : al.elements) {
      for (const double s : {e.sta, e.sta + e.len}) {
        if (s > a + kEps && s < b - kEps) {
          add(s);
        }
      }
    }
  }
  appkit::js_sort(set, [](double x, double y) { return x - y; });
  std::vector<double> out;
  for (std::size_t i = 0; i < set.size(); ++i) {
    if (i == 0 || set[i] - set[i - 1] > 1e-6) {
      out.push_back(set[i]);
    }
  }
  return out;
}

Value stake_rows(const Alignment& al, const Value& opts, const ZAt& design, const ZAt& ground) {
  Array rows;
  const Value ov = get(opts, "offsets");
  const Array offsets = ov.is_undefined() ? Array{Value(0)} : arr(ov);
  const Value fv = get(opts, "first");
  double n = fv.is_undefined() ? 1 : num(fv);
  const Value pv = get(opts, "prefix");
  const std::string prefix = pv.is_undefined() ? std::string() : base::js_string(pv);
  for (const Value& sv : arr(get(opts, "stations"))) {
    const double sta = num(sv);
    for (const Value& offv : offsets) {
      const double off = num(offv);
      const auto xy = place(al, sta, off);
      if (!xy) {
        throw std::runtime_error("Cannot read properties of null (reading 'x')");
      }
      const auto d = design(sta, off, xy->x, xy->y);
      const auto g = ground(std::nan(""), 0, xy->x, xy->y);
      const bool df = d && std::isfinite(*d);
      const bool gf = g && std::isfinite(*g);
      rows.push_back(make_object({{"id", prefix + appkit::num_str(n)},
                                  {"sta", sv},
                                  {"off", offv},
                                  {"x", xy->x},
                                  {"y", xy->y},
                                  {"design", num_or_null(d)},
                                  {"ground", num_or_null(g)},
                                  {"cf", df && gf ? Value(*g - *d) : Value(nullptr)}}));
      n += 1;
    }
  }
  return rows;
}

Value point_rows(const Alignment& al, const Value& points, const ZAt& design, const ZAt* ground) {
  struct Row {
    Value v;
    double sta, off;
  };
  std::vector<Row> rows;
  double outside = 0;
  for (const Value& p : arr(points)) {
    const double x = num(get(p, "x"));
    const double y = num(get(p, "y"));
    const auto so = al.station_offset(x, y);
    if (!so || so->outside) {
      outside++;
      continue;
    }
    const auto d = design(so->sta, so->off, x, y);
    std::optional<double> g;
    if (fin(get(p, "z"))) {
      g = get(p, "z").as_number();
    } else if (ground != nullptr) {
      g = (*ground)(std::nan(""), 0, x, y);
    }
    const bool df = d && std::isfinite(*d);
    const bool gf = g && std::isfinite(*g);
    rows.push_back({make_object({{"id", get(p, "n")},
                                 {"sta", so->sta},
                                 {"off", so->off},
                                 {"x", get(p, "x")},
                                 {"y", get(p, "y")},
                                 {"design", num_or_null(d)},
                                 {"ground", num_or_null(g)},
                                 {"cf", df && gf ? Value(*g - *d) : Value(nullptr)}}),
                    so->sta, so->off});
  }
  appkit::js_sort(rows, [](const Row& a, const Row& b) {
    const double d = a.sta - b.sta;
    return (d != 0 && !std::isnan(d)) ? d : a.off - b.off;
  });
  Array out;
  for (Row& r : rows) {
    out.push_back(std::move(r.v));
  }
  return make_object({{"rows", std::move(out)}, {"outside", outside}});
}

Value parse_offsets(const Value& text) {
  const std::string src0 =
      base::js_to_upper((text.is_null() || text.is_undefined()) ? std::string() : base::js_string(text));
  auto word = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  };
  auto digit = [](char c) { return c >= '0' && c <= '9'; };
  // /\bCL\b/g → '0'
  std::string s1;
  for (std::size_t i = 0; i < src0.size();) {
    const bool start = i == 0 || !word(src0[i - 1]);
    if (start && src0.compare(i, 2, "CL") == 0 && (i + 2 >= src0.size() || !word(src0[i + 2]))) {
      s1.push_back('0');
      i += 2;
      continue;
    }
    s1.push_back(src0[i]);
    ++i;
  }
  // /(^|[\s,;])([LR])\s+(\d)/g → '$1$2$3'
  std::string s2;
  for (std::size_t i = 0; i < s1.size();) {
    auto try_at = [&](std::size_t p, std::size_t lead) -> std::size_t {  // p = where [LR] is; returns the end or 0
      if (p >= s1.size() || (s1[p] != 'L' && s1[p] != 'R')) {
        return 0;
      }
      std::size_t q = p + 1;
      const std::size_t q0 = q;
      while (q < s1.size()) {
        const std::size_t n = base::js_space_at(s1, q);
        if (n == 0) {
          break;
        }
        q += n;
      }
      if (q == q0 || q >= s1.size() || !digit(s1[q])) {
        return 0;
      }
      (void)lead;
      return q + 1;
    };
    if (i == 0) {
      if (const std::size_t e = try_at(0, 0); e != 0) {
        s2.push_back(s1[0]);
        s2.push_back(s1[e - 1]);
        i = e;
        continue;
      }
    }
    std::size_t lead = 0;
    if (s1[i] == ',' || s1[i] == ';') {
      lead = 1;
    } else {
      lead = base::js_space_at(s1, i);
    }
    if (lead != 0) {
      if (const std::size_t e = try_at(i + lead, lead); e != 0) {
        s2.append(s1, i, lead);
        s2.push_back(s1[i + lead]);
        s2.push_back(s1[e - 1]);
        i = e;
        continue;
      }
    }
    s2.push_back(s1[i]);
    ++i;
  }
  // /(\d)\s+([LR])\b/g → '$1$2'
  std::string src;
  for (std::size_t i = 0; i < s2.size();) {
    if (digit(s2[i])) {
      std::size_t q = i + 1;
      while (q < s2.size()) {
        const std::size_t n = base::js_space_at(s2, q);
        if (n == 0) {
          break;
        }
        q += n;
      }
      if (q > i + 1 && q < s2.size() && (s2[q] == 'L' || s2[q] == 'R') && (q + 1 >= s2.size() || !word(s2[q + 1]))) {
        src.push_back(s2[i]);
        src.push_back(s2[q]);
        i = q + 1;
        continue;
      }
    }
    src.push_back(s2[i]);
    ++i;
  }
  // split on /[\s,;]+/, drop empty tokens
  std::vector<std::string> toks;
  std::string cur;
  for (std::size_t i = 0; i < src.size();) {
    const std::size_t n = base::js_space_at(src, i);
    if (n != 0 || src[i] == ',' || src[i] == ';') {
      if (!cur.empty()) {
        toks.push_back(cur);
      }
      cur.clear();
      i += n != 0 ? n : 1;
      continue;
    }
    cur.push_back(src[i]);
    ++i;
  }
  if (!cur.empty()) {
    toks.push_back(cur);
  }
  std::vector<double> out;
  for (const std::string& tok : toks) {
    // /^([LR])?(-?\d+(?:\.\d+)?)([LR])?$/
    std::size_t i = 0;
    char s_pre = 0;
    char s_post = 0;
    if (i < tok.size() && (tok[i] == 'L' || tok[i] == 'R')) {
      s_pre = tok[i++];
    }
    const std::size_t n0 = i;
    if (i < tok.size() && tok[i] == '-') {
      ++i;
    }
    const std::size_t d0 = i;
    while (i < tok.size() && digit(tok[i])) {
      ++i;
    }
    bool ok = i > d0;
    if (ok && i + 1 < tok.size() && tok[i] == '.' && digit(tok[i + 1])) {
      ++i;
      while (i < tok.size() && digit(tok[i])) {
        ++i;
      }
    }
    const std::size_t n1 = i;
    if (ok && i < tok.size() && (tok[i] == 'L' || tok[i] == 'R')) {
      s_post = tok[i++];
    }
    ok = ok && i == tok.size();
    if (!ok || (s_pre != 0 && s_post != 0)) {
      return make_object({{"error", tok}});
    }
    double v = base::js_number(std::string_view(tok).substr(n0, n1 - n0));
    const char side = s_pre != 0 ? s_pre : s_post;
    if (side != 0) {
      if (v < 0) {
        return make_object({{"error", tok}});
      }
      v = side == 'L' ? -v : v;
    }
    if (std::abs(v) > 1e5) {
      return make_object({{"error", tok}});
    }
    if (std::ranges::none_of(out, [&](double o) { return std::abs(o - v) < 1e-9; })) {
      out.push_back(v);
    }
  }
  if (out.size() > 200) {
    return make_object({{"error", "…"}});
  }
  Array a;
  for (const double v : out) {
    a.emplace_back(v);
  }
  return a;
}

std::string cut_fill_text(const Value& cf) {
  if (!fin(cf)) {
    return {};
  }
  const double c = cf.as_number();
  if (std::abs(c) < 0.005) {
    return "0.00";
  }
  return std::string(c > 0 ? "C " : "F ") + strip_commas(fixed(std::abs(c), 2));
}

std::string offset_text(double off) {
  if (std::abs(off) < 0.005) {
    return "0.00";
  }
  return strip_commas(fixed(std::abs(off), 2)) + (off < 0 ? " L" : " R");
}

std::string staking_csv(const Value& rows, const Value& opts, const i18n::Catalog& w) {
  const Value uv = get(opts, "units");
  const std::string units = uv.is_undefined() ? std::string("ft") : base::js_string(uv);
  const std::string u = units == "m" ? "m" : "ft";
  const Value title = get(opts, "title");
  auto q = [](const std::string& s) {
    if (s.empty() || (s[0] >= '0' && s[0] <= '9') || (s.size() > 1 && s[0] == '-' && s[1] >= '0' && s[1] <= '9')) {
      return s;
    }
    if (s[0] == '=' || s[0] == '+' || s[0] == '-' || s[0] == '@' || s[0] == '\t' || s[0] == '\r') {
      return "'" + s;
    }
    return s;
  };
  std::vector<std::vector<std::string>> out;
  if (title.truthy()) {
    out.push_back({base::js_string(title)});
  }
  out.push_back({w.t("sk.c.point"), w.t("sk.c.sta"), w.t("sk.c.off", {{"u", u}}), w.t("sk.c.n"), w.t("sk.c.e"),
                 w.t("sk.c.design"), w.t("sk.c.ground"), w.t("sk.c.cf", {{"u", u}}), w.t("sk.c.cfn")});
  for (const Value& r : arr(rows)) {
    out.push_back({base::js_string(get(r, "id")), format_station(num(get(r, "sta")), units),
                   offset_text(num(get(r, "off"))), fixed_num(get(r, "y"), 3), fixed_num(get(r, "x"), 3),
                   fixed_num(get(r, "design"), 3), fixed_num(get(r, "ground"), 3), cut_fill_text(get(r, "cf")),
                   fixed_num(get(r, "cf"), 3)});
  }
  for (auto& row : out) {
    for (auto& cell : row) {
      cell = q(cell);
    }
  }
  return rows_csv(out);
}

}  // namespace cc::civil
