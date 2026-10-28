// SPDX-License-Identifier: Apache-2.0
// sections.js (CivilCraft 0.5), ported function by function.
#include "cc/civil/sections.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>

#include "cc/appkit/jsops.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::make_object;
using base::Value;

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
double jmax(double a, double b) {
  return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::max(a, b);
}
double jmin(double a, double b) {
  return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::min(a, b);
}
bool finite_number(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number());
}

/// A JS Set of numbers in insertion order; sorted() then sorts them ascending (stable).
class NumSet {
 public:
  void add(double s) {
    if (s == 0) {
      s = 0;
    }
    if (std::isnan(s)) {
      if (!nan_) {
        nan_ = true;
        order_.push_back(s);
      }
      return;
    }
    if (seen_.insert(s).second) {
      order_.push_back(s);
    }
  }
  [[nodiscard]] std::size_t size() const { return order_.size(); }
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

std::optional<double> seg_cross(double px, double py, double rx, double ry, double qx, double qy, double sx,
                                double sy) {
  const double den = rx * sy - ry * sx;
  if (std::abs(den) < 1e-15) {
    return std::nullopt;
  }
  const double t0 = ((qx - px) * sy - (qy - py) * sx) / den;
  const double u = ((qx - px) * ry - (qy - py) * rx) / den;
  if (t0 >= -1e-12 && t0 <= 1 + 1e-12 && u >= -1e-12 && u <= 1 + 1e-12) {
    return jmax(0, jmin(1, t0));
  }
  return std::nullopt;
}

bool seg_hits_box(double ax, double ay, double bx, double by, double x0, double y0, double x1, double y1) {
  double t0 = 0;
  double t1 = 1;
  const double dx = bx - ax;
  const double dy = by - ay;
  const std::array<std::pair<double, double>, 4> pq{{{-dx, ax - x0}, {dx, x1 - ax}, {-dy, ay - y0}, {dy, y1 - ay}}};
  for (const auto& [p, q] : pq) {
    if (p == 0) {
      if (q < 0) {
        return false;
      }
      continue;
    }
    const double r = q / p;
    if (p < 0) {
      if (r > t1) {
        return false;
      }
      t0 = std::max(t0, r);  // (r > t0 ? r : t0, as the JS's if)
    } else {
      if (r < t0) {
        return false;
      }
      t1 = std::min(t1, r);
    }
  }
  return true;
}

/// clampI of surfaceSection: Math.max(0, Math.min(n − 1, Math.floor(v))); NaN stays NaN (nullopt).
std::optional<std::size_t> clamp_cell(double v, std::size_t n) {
  const double f = std::floor(v);
  if (std::isnan(f)) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(std::max(0.0, std::min(static_cast<double>(n) - 1, f)));
}

double zv(const OZ& p) {  // a null z in arithmetic: 0
  return p.z.value_or(0);
}

std::string csv_cell(const std::string& s) {
  if (s.find_first_of("\",\r\n") == std::string::npos) {
    return s;
  }
  std::string o = "\"";
  for (const char c : s) {
    if (c == '"') {
      o += "\"\"";
    } else {
      o.push_back(c);
    }
  }
  return o + "\"";
}

}  // namespace

Value sample_defaults(std::string_view units) {
  if (units == "m") {
    return make_object({{"step", 20}, {"left", 30}, {"right", 30}});
  }
  return make_object({{"step", 50}, {"left", 100}, {"right", 100}});
}

Value clean_sampling(const Value& o, std::string_view units) {
  if (!o.is_object() && !o.is_array()) {
    return nullptr;
  }
  const Value d = sample_defaults(units);
  auto pos = [&](std::string_view k, double max) {
    const Value v = get(o, k);
    return finite_number(v) && v.as_number() > 0 && v.as_number() <= max ? v : d.get(k);
  };
  NumSet extra;
  const Value ex = get(o, "extra");
  std::size_t taken = 0;
  for (const Value& v : arr(ex)) {
    if (taken >= 500) {
      break;
    }
    if (finite_number(v) && std::abs(v.as_number()) < 1e8) {
      extra.add(v.as_number());
      ++taken;
    }
  }
  Array ev;
  for (const double v : extra.sorted()) {
    ev.emplace_back(v);
  }
  return make_object(
      {{"step", pos("step", 1e5)}, {"left", pos("left", 1e4)}, {"right", pos("right", 1e4)}, {"extra", std::move(ev)}});
}

std::vector<double> sample_stations(const Alignment* al, double step, const std::vector<double>& extra,
                                    std::optional<double> from, std::optional<double> to) {
  if (al == nullptr || !al->ok()) {
    return {};
  }
  const double lo = jmax(al->start, (from && std::isfinite(*from)) ? *from : al->start);
  const double hi = jmin(al->end, (to && std::isfinite(*to)) ? *to : al->end);
  if (!(hi >= lo)) {
    return {};
  }
  NumSet st;
  st.add(lo);
  st.add(hi);
  if (step > 0) {
    const double k0 = std::ceil(lo / step - 1e-9);
    for (std::int64_t q = 0; (k0 + static_cast<double>(q)) * step < hi - 1e-9 && st.size() < kMaxSections; ++q) {
      st.add(base::js_number(base::js_to_fixed((k0 + static_cast<double>(q)) * step, 6)));
    }
  }
  for (const auto& k : al->key_stations()) {
    if (k.sta > lo && k.sta < hi) {
      st.add(k.sta);
    }
  }
  for (const double s : extra) {
    if (s >= lo && s <= hi) {
      st.add(s);
    }
  }
  std::vector<double> out;
  for (const double s : st.sorted()) {
    if (out.empty() || s - out.back() > 1e-6) {
      out.push_back(s);
    }
  }
  if (out.size() > kMaxSections) {
    out.resize(kMaxSections);
  }
  return out;
}

std::optional<SampleLine> sample_line(const Alignment& al, double sta, const Value& left, const Value& right) {
  const auto p = al.point_at(sta);
  if (!p) {
    return std::nullopt;
  }
  const double rx = base::js_sin(p->dir);
  const double ry = -base::js_cos(p->dir);
  const double l = num(left);
  const double r = num(right);
  return SampleLine{sta,  {p->x, p->y}, {p->x - rx * l, p->y - ry * l}, {p->x + rx * r, p->y + ry * r}, p->dir,
                    left, right};
}

Runs surface_section(const Surface* s, P2 a, P2 b, double off_a, double off_b) {
  if (s == nullptr) {
    return {};
  }
  const auto& X = s->X();
  const auto& Y = s->Y();
  const auto& tris = s->tris();
  const double ax = a.x - s->ox();
  const double ay = a.y - s->oy();
  const double bx = b.x - s->ox();
  const double by = b.y - s->oy();
  const double dx = bx - ax;
  const double dy = by - ay;
  const double L = base::js_hypot(dx, dy);
  if (!(L > 0)) {
    return {};
  }
  std::vector<double> ts{0, 1};
  const Surface::GridView g = s->grid_view();
  const double x0 = jmin(ax, bx);
  const double x1 = jmax(ax, bx);
  const double y0 = jmin(ay, by);
  const double y1 = jmax(ay, by);
  const bool outside = x1 < g.min_x || x0 > g.max_x || y1 < g.min_y || y0 > g.max_y;  // (NaN: inside)
  if (!outside) {
    const auto i0 = clamp_cell((x0 - g.min_x) / g.cw, g.nx);
    const auto i1 = clamp_cell((x1 - g.min_x) / g.cw, g.nx);
    const auto j0 = clamp_cell((y0 - g.min_y) / g.ch, g.ny);
    const auto j1 = clamp_cell((y1 - g.min_y) / g.ch, g.ny);
    std::set<std::int32_t> seen;
    if (i0 && i1 && j0 && j1) {
      for (std::size_t j = *j0; j <= *j1; ++j) {
        for (std::size_t i = *i0; i <= *i1; ++i) {
          const double cx0 = g.min_x + static_cast<double>(i) * g.cw;
          const double cy0 = g.min_y + static_cast<double>(j) * g.ch;
          if (!seg_hits_box(ax, ay, bx, by, cx0, cy0, cx0 + g.cw, cy0 + g.ch)) {
            continue;
          }
          const std::size_t k = j * g.nx + i;
          for (auto q = g.start[k]; q < g.start[k + 1]; ++q) {
            const std::int32_t tri = g.items[static_cast<std::size_t>(q)];
            if (!seen.insert(tri).second) {
              continue;
            }
            for (std::size_t e = 0; e < 3; ++e) {
              const auto p = static_cast<std::size_t>(tris[3 * static_cast<std::size_t>(tri) + e]);
              const auto r = static_cast<std::size_t>(tris[3 * static_cast<std::size_t>(tri) + ((e + 1) % 3)]);
              const auto tt = seg_cross(ax, ay, dx, dy, X[p], Y[p], X[r] - X[p], Y[r] - Y[p]);
              if (tt) {
                ts.push_back(*tt);
              }
            }
          }
        }
      }
    }
  }
  appkit::js_sort(ts, [](double u, double v) { return u - v; });
  Runs runs;
  bool open = false;
  double last = -1;
  for (const double tt : ts) {
    if (tt - last < 1e-12) {
      continue;
    }
    last = tt;
    const auto z = s->elevation_at(a.x + (b.x - a.x) * tt, a.y + (b.y - a.y) * tt, 1e-7);
    const double o = off_a + (off_b - off_a) * tt;
    if (!z) {
      open = false;
      continue;
    }
    if (!open) {
      runs.emplace_back();
      open = true;
    }
    runs.back().push_back({o, z});
  }
  std::erase_if(runs, [](const Run& r) { return r.size() < 2; });
  return runs;
}

std::optional<double> z_at(const Runs& runs, double o) {
  for (const Run& r : runs) {
    if (r.empty() || o < r.front().o - 1e-9 || o > r.back().o + 1e-9) {
      continue;
    }
    for (std::size_t i = 1; i < r.size(); ++i) {
      if (o <= r[i].o + 1e-9) {
        const OZ& p = r[i - 1];
        const OZ& q = r[i];
        const double d = q.o - p.o;
        if (d > 0) {
          return zv(p) + ((zv(q) - zv(p)) * (o - p.o)) / d;
        }
        return q.z;
      }
    }
  }
  return std::nullopt;
}

CutFill cut_fill(const Runs& ground, const Runs& design) {
  NumSet os;
  for (const Runs* rs : {&ground, &design}) {
    for (const Run& r : *rs) {
      for (const OZ& p : r) {
        os.add(p.o);
      }
    }
  }
  const std::vector<double> xs = os.sorted();
  CutFill out;
  auto push = [&](double o0, double o1, bool cut, double area) {
    if (!(area > 0) || o1 - o0 <= 0) {
      return;
    }
    if (!out.parts.empty() && out.parts.back().cut == cut && std::abs(out.parts.back().o1 - o0) < 1e-9) {
      out.parts.back().o1 = o1;
    } else {
      out.parts.push_back({o0, o1, cut});
    }
    (cut ? out.cut : out.fill) += area;
  };
  for (std::size_t i = 1; i < xs.size(); ++i) {
    const double o0 = xs[i - 1];
    const double o1 = xs[i];
    const double om = (o0 + o1) / 2;
    if (!z_at(ground, om) || !z_at(design, om)) {
      continue;
    }
    const auto g0 = z_at(ground, o0);
    const auto g1 = z_at(ground, o1);
    const auto d0 = z_at(design, o0);
    const auto d1 = z_at(design, o1);
    if (!g0 || !g1 || !d0 || !d1) {
      continue;
    }
    const double e0 = *d0 - *g0;
    const double e1 = *d1 - *g1;
    const double w = o1 - o0;
    if (e0 >= 0 && e1 >= 0) {
      push(o0, o1, false, ((e0 + e1) / 2) * w);
    } else if (e0 <= 0 && e1 <= 0) {
      push(o0, o1, true, ((-e0 - e1) / 2) * w);
    } else {
      const double om2 = o0 + (w * e0) / (e0 - e1);
      const double a0 = (std::abs(e0) / 2) * (om2 - o0);
      const double a1 = (std::abs(e1) / 2) * (o1 - om2);
      push(o0, om2, !(e0 > 0), a0);
      push(om2, o1, !(e1 > 0), a1);
    }
  }
  return out;
}

Value feature_marks(const Value& lines, const SampleLine& line) {
  struct Mark {
    std::string code;
    double o;
    Value z;
  };
  std::vector<Mark> out;
  const double left = num(line.left);
  const double right = num(line.right);
  const double dx = line.b.x - line.a.x;
  const double dy = line.b.y - line.a.y;
  if (lines.is_object() && !lines.is_point()) {
    for (const base::Member* m : base::js_members(lines.as_object())) {
      if (!m->value.is_array()) {
        continue;
      }
      const Array& pts = m->value.as_array();
      for (std::size_t i = 1; i < pts.size(); ++i) {
        const Value& p = pts[i - 1];
        const Value& q = pts[i];
        const double px = num(get(p, "x"));
        const double py = num(get(p, "y"));
        const double qx = num(get(q, "x"));
        const double qy = num(get(q, "y"));
        const auto tt = seg_cross(line.a.x, line.a.y, dx, dy, px, py, qx - px, qy - py);
        if (!tt) {
          continue;
        }
        const auto u = seg_cross(px, py, qx - px, qy - py, line.a.x, line.a.y, dx, dy);
        const Value pz = get(p, "z");
        const Value qz = get(q, "z");
        Value z = nullptr;
        if (finite_number(pz) && finite_number(qz) && u) {
          z = pz.as_number() + (qz.as_number() - pz.as_number()) * *u;
        }
        out.push_back({m->key, -left + (left + right) * *tt, z});
        break;
      }
    }
  }
  appkit::js_sort(out, [](const Mark& a, const Mark& b) { return a.o - b.o; });
  Array v;
  for (const Mark& m : out) {
    v.push_back(make_object({{"code", m.code}, {"o", m.o}, {"z", m.z}}));
  }
  return v;
}

std::vector<std::pair<double, double>> region_spans(const SampleLine& line,
                                                    const std::vector<std::vector<P2>>& regions) {
  const double left = num(line.left);
  const double right = num(line.right);
  const P2 a = line.a;
  const double dx = line.b.x - a.x;
  const double dy = line.b.y - a.y;
  NumSet ts;
  ts.add(0);
  ts.add(1);
  for (const auto& r : regions) {
    for (std::size_t i = 0; i < r.size(); ++i) {
      const P2 p = r[i];
      const P2 q = r[(i + 1) % r.size()];
      const auto tt = seg_cross(a.x, a.y, dx, dy, p.x, p.y, q.x - p.x, q.y - p.y);
      if (tt) {
        ts.add(*tt);
      }
    }
  }
  const std::vector<double> xs = ts.sorted();
  auto on_edge = [](double x, double y, const std::vector<P2>& r) {
    for (std::size_t i = 0; i < r.size(); ++i) {
      const P2 p = r[i];
      const P2 q = r[(i + 1) % r.size()];
      const double ex = q.x - p.x;
      const double ey = q.y - p.y;
      const double L = ex * ex + ey * ey;
      const double u = (L != 0 && !std::isnan(L)) ? jmax(0, jmin(1, ((x - p.x) * ex + (y - p.y) * ey) / L)) : 0;
      if (base::js_hypot(x - p.x - u * ex, y - p.y - u * ey) <= 1e-6 * jmax(1, std::sqrt(L))) {
        return true;
      }
    }
    return false;
  };
  auto inside = [&](double x, double y) {
    return std::ranges::any_of(regions,
                               [&](const std::vector<P2>& r) { return point_in_poly(x, y, r) || on_edge(x, y, r); });
  };
  std::vector<std::pair<double, double>> out;
  for (std::size_t i = 1; i < xs.size(); ++i) {
    const double tm = (xs[i - 1] + xs[i]) / 2;
    if (xs[i] - xs[i - 1] < 1e-12 || !inside(a.x + dx * tm, a.y + dy * tm)) {
      continue;
    }
    const double o0 = -left + (left + right) * xs[i - 1];
    const double o1 = -left + (left + right) * xs[i];
    if (!out.empty() && std::abs(out.back().second - o0) < 1e-9) {
      out.back().second = o1;
    } else {
      out.emplace_back(o0, o1);
    }
  }
  return out;
}

Runs clip_runs(const Runs& runs, const std::vector<std::pair<double, double>>& spans) {
  Runs out;
  for (const auto& [o0, o1] : spans) {
    for (const Run& r : runs) {
      if (r.empty() || r.back().o <= o0 || r.front().o >= o1) {
        continue;
      }
      Run pts;
      const double lo = jmax(o0, r.front().o);
      const double hi = jmin(o1, r.back().o);
      const Runs one{r};
      pts.push_back({lo, z_at(one, lo)});
      for (const OZ& q : r) {
        if (q.o > o0 + 1e-12 && q.o < o1 - 1e-12) {
          pts.push_back(q);
        }
      }
      pts.push_back({hi, z_at(one, hi)});
      if (pts.size() >= 2 && pts.back().o - pts.front().o > 1e-12) {
        out.push_back(std::move(pts));
      }
    }
  }
  return out;
}

Runs runs_from(const Value& v) {
  Runs out;
  for (const Value& r : arr(v)) {
    Run run;
    for (const Value& p : arr(r)) {
      const Value z = get(p, "z");
      run.push_back({num(get(p, "o")), z.is_null() || z.is_undefined() ? std::nullopt : std::optional(num(z))});
    }
    out.push_back(std::move(run));
  }
  return out;
}

Value runs_value(const Runs& r) {
  Array out;
  for (const Run& run : r) {
    Array a;
    for (const OZ& p : run) {
      a.push_back(make_object({{"o", p.o}, {"z", p.z ? Value(*p.z) : Value(nullptr)}}));
    }
    out.emplace_back(std::move(a));
  }
  return out;
}

Value line_value(const SampleLine& l) {
  return make_object({{"sta", l.sta},
                      {"c", base::pt2(l.c.x, l.c.y)},
                      {"a", base::pt2(l.a.x, l.a.y)},
                      {"b", base::pt2(l.b.x, l.b.y)},
                      {"dir", l.dir},
                      {"left", l.left},
                      {"right", l.right}});
}

SampleLine line_from(const Value& v) {
  auto xy = [&](std::string_view k) {
    const Value p = get(v, k);
    return P2{num(get(p, "x")), num(get(p, "y"))};
  };
  return {num(get(v, "sta")), xy("c"), xy("a"), xy("b"), num(get(v, "dir")), get(v, "left"), get(v, "right")};
}

namespace {
Value parts_value(const std::vector<Part>& parts) {
  Array a;
  for (const Part& p : parts) {
    a.push_back(make_object({{"o0", p.o0}, {"o1", p.o1}, {"kind", p.cut ? "cut" : "fill"}}));
  }
  return a;
}
}  // namespace

Value build_sections(const SectionInput& in) {
  const Value smp = in.sampling.truthy() ? in.sampling : sample_defaults("ft");
  std::vector<double> extra;
  for (const Value& e : arr(get(smp, "extra"))) {
    extra.push_back(num(e));
  }
  const std::vector<double> stations = sample_stations(in.alignment, num(get(smp, "step")), extra, in.from, in.to);
  Array sections;
  struct Row {
    double sta, cut, fill;
  };
  std::vector<Row> cf_rows;
  const Value left = get(smp, "left");
  const Value right = get(smp, "right");
  for (const double sta : stations) {
    const auto line = sample_line(*in.alignment, sta, left, right);
    if (!line) {
      continue;
    }
    const Runs g = surface_section(in.ground, line->a, line->b, -num(left), num(right));
    Runs d = g;
    const bool separate = in.design != nullptr && in.design != in.ground;
    if (separate) {
      d = surface_section(in.design, line->a, line->b, -num(left), num(right));
      if (in.regions) {
        d = clip_runs(d, region_spans(*line, *in.regions));
      }
    }
    const CutFill cf = cut_fill(g, d);
    cf_rows.push_back({sta, cf.cut, cf.fill});
    sections.push_back(
        make_object({{"sta", sta},
                     {"a", base::pt2(line->a.x, line->a.y)},
                     {"b", base::pt2(line->b.x, line->b.y)},
                     {"c", base::pt2(line->c.x, line->c.y)},
                     {"left", left},
                     {"right", right},
                     {"ground", runs_value(g)},
                     {"design", runs_value(d)},
                     {"cut", cf.cut},
                     {"fill", cf.fill},
                     {"parts", parts_value(cf.parts)},
                     {"marks", in.corridor.truthy() ? feature_marks(in.corridor, *line) : Value::array()}}));
  }
  Array rows;
  double cum_cut = 0;
  double cum_fill = 0;
  for (std::size_t i = 0; i < cf_rows.size(); ++i) {
    double cut_vol = 0;
    double fill_vol = 0;
    if (i != 0) {
      const Row& p = cf_rows[i - 1];
      const double L = cf_rows[i].sta - p.sta;
      cut_vol = ((p.cut + cf_rows[i].cut) / 2) * L;
      fill_vol = ((p.fill + cf_rows[i].fill) / 2) * L;
    }
    cum_cut += cut_vol;
    cum_fill += fill_vol;
    rows.push_back(make_object({{"sta", cf_rows[i].sta},
                                {"cut", cf_rows[i].cut},
                                {"fill", cf_rows[i].fill},
                                {"cutVol", cut_vol},
                                {"fillVol", fill_vol},
                                {"cumCut", cum_cut},
                                {"cumFill", cum_fill}}));
  }
  return make_object({{"sections", std::move(sections)},
                      {"rows", std::move(rows)},
                      {"totals", make_object({{"cut", cum_cut}, {"fill", cum_fill}})}});
}

std::string sections_csv(const Value& set, std::string_view units, const i18n::Catalog& words) {
  const bool ft = units != "m";
  const double vol_per = ft ? 27 : 1;
  std::vector<std::string> head{words.t("sec.csv.station")};
  for (const char* k : {"cut.area", "fill.area", "cut.vol", "fill.vol", "cum.cut", "cum.fill"}) {
    head.push_back(words.t("sec.csv." + std::string(k) + (ft ? ".ft" : ".m")));
  }
  std::string out;
  for (std::size_t i = 0; i < head.size(); ++i) {
    out += (i != 0 ? "," : "") + csv_cell(head[i]);
  }
  for (const Value& r : arr(get(set, "rows"))) {
    const std::vector<std::string> cells{appkit::num_str(num(r.get("sta"))),
                                         base::js_to_fixed(num(r.get("cut")), 2),
                                         base::js_to_fixed(num(r.get("fill")), 2),
                                         base::js_to_fixed(num(r.get("cutVol")) / vol_per, 1),
                                         base::js_to_fixed(num(r.get("fillVol")) / vol_per, 1),
                                         base::js_to_fixed(num(r.get("cumCut")) / vol_per, 1),
                                         base::js_to_fixed(num(r.get("cumFill")) / vol_per, 1)};
    out += "\r\n";
    for (std::size_t i = 0; i < cells.size(); ++i) {
      out += (i != 0 ? "," : "") + csv_cell(cells[i]);
    }
  }
  return out + "\r\n";
}

}  // namespace cc::civil
