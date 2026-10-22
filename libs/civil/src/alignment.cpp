// SPDX-License-Identifier: Apache-2.0
// alignment.js and profile.js (CivilCraft 0.5), ported function by function; V8's sin, cos and atan2 (libs/base).
#include "cc/civil/alignment.hpp"

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

constexpr double kPi = std::numbers::pi;
constexpr double kTau = kPi * 2;
using base::js_atan2;
using base::js_cos;
using base::js_sin;
/// What point_at gives where there are elements always; never used.
constexpr AlPoint kNoPoint{std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN(), 0};
double hyp(double a, double b) {
  return base::js_hypot(a, b);
}
double jmax(double a, double b) {
  return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::max(a, b);
}
double jmin(double a, double b) {
  return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::min(a, b);
}
double number_or_zero(const base::Value& v) {  // Number(v) || 0
  const double n = base::js_to_number(v);
  return (std::isnan(n) || n == 0) ? 0 : n;
}

/// Unique stations in insertion order (a JS Set of numbers), then sorted ascending (stable).
class StaSet {
 public:
  void add(double s) {
    if (s == 0) {
      s = 0;  // SameValueZero: -0 is 0
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

}  // namespace

double norm_ang(double a) {
  a = std::fmod(a, kTau);
  if (a <= -kPi) {
    a += kTau;
  }
  if (a > kPi) {
    a -= kTau;
  }
  return a;
}

std::shared_ptr<Alignment> build_alignment(const std::vector<Pi>& pis, double start, const i18n::Catalog& words) {
  std::vector<std::string> errors;
  std::vector<Element> elements;
  std::vector<Pi> P;
  for (const Pi& p : pis) {
    if (std::isfinite(p.x) && std::isfinite(p.y)) {
      P.push_back(p);
    }
  }
  std::vector<Pi> Q;
  for (std::size_t i = 0; i < P.size(); ++i) {
    if (i == 0 || hyp(P[i].x - P[i - 1].x, P[i].y - P[i - 1].y) > 1e-9) {
      Q.push_back(P[i]);
    }
  }
  if (Q.size() < 2) {
    return std::make_shared<Alignment>(std::vector<Element>{}, start, std::vector<std::string>{words.t("al.err.two")},
                                       std::vector<Pi>{});
  }
  const std::size_t n = Q.size();
  struct Curve {
    double r, d, T, a1, a2;
  };
  std::vector<std::optional<Curve>> curves(n);
  for (std::size_t i = 1; i + 1 < n; ++i) {
    const double r = number_or_zero(Q[i].r);
    const double a1 = js_atan2(Q[i].y - Q[i - 1].y, Q[i].x - Q[i - 1].x);
    const double a2 = js_atan2(Q[i + 1].y - Q[i].y, Q[i + 1].x - Q[i].x);
    const double d = norm_ang(a2 - a1);
    if (r <= 0 || std::abs(d) < 1e-9) {
      curves[i] = Curve{0, d, 0, a1, a2};
      continue;
    }
    if (std::abs(std::abs(d) - kPi) < 1e-6) {
      errors.push_back(words.t("al.err.back", {{"pi", appkit::num_str(static_cast<double>(i + 1))}}));
      curves[i] = Curve{0, d, 0, a1, a2};
      continue;
    }
    curves[i] = Curve{r, d, r * base::js_tan(std::abs(d) / 2), a1, a2};
  }
  auto T_of = [&](std::size_t i) {
    if (i >= n || !curves[i]) {
      return 0.0;
    }
    const double t = curves[i]->T;
    return std::isnan(t) ? 0.0 : t;  // `T || 0`
  };
  for (std::size_t i = 0; i + 1 < n; ++i) {
    const double L = hyp(Q[i + 1].x - Q[i].x, Q[i + 1].y - Q[i].y);
    const double need = T_of(i) + T_of(i + 1);
    if (need > L + 1e-9) {
      errors.push_back(words.t("al.err.overlap", {{"a", appkit::num_str(static_cast<double>(i + 1))},
                                                  {"b", appkit::num_str(static_cast<double>(i + 2))},
                                                  {"need", base::js_to_fixed(need, 2)},
                                                  {"dist", base::js_to_fixed(L, 2)}}));
    }
  }
  if (!errors.empty()) {
    return std::make_shared<Alignment>(std::vector<Element>{}, start, std::move(errors), std::move(Q));
  }
  double sta = start;
  P2 cur{Q[0].x, Q[0].y};
  for (std::size_t i = 1; i < n; ++i) {
    const std::optional<Curve> c = i + 1 < n ? curves[i] : std::nullopt;
    const double dir = js_atan2(Q[i].y - Q[i - 1].y, Q[i].x - Q[i - 1].x);
    const double ux = js_cos(dir);
    const double uy = js_sin(dir);
    double T = 0;  // `c?.T || 0`
    if (c && !std::isnan(c->T)) {
      T = c->T;
    }
    const P2 end{Q[i].x - ux * T, Q[i].y - uy * T};
    const double len = hyp(end.x - cur.x, end.y - cur.y);
    if (len > 1e-9) {
      Element e;
      e.p0 = cur;
      e.p1 = end;
      e.dir = dir;
      e.sta = sta;
      e.len = len;
      elements.push_back(e);
      sta += len;
    }
    if (c && c->r > 0) {
      const bool ccw = c->d > 0;
      const double r = c->r;
      const double delta = std::abs(c->d);
      const double nx = ccw ? -uy : uy;
      const double ny = ccw ? ux : -ux;
      const P2 ctr{end.x + nx * r, end.y + ny * r};
      const double dir2 = c->a2;
      const P2 pt{Q[i].x + js_cos(dir2) * T, Q[i].y + js_sin(dir2) * T};
      const double a0 = js_atan2(end.y - ctr.y, end.x - ctr.x);
      const double L = r * delta;
      Element e;
      e.arc = true;
      e.p0 = end;
      e.p1 = pt;
      e.c = ctr;
      e.r = r;
      e.ccw = ccw;
      e.delta = delta;
      e.a0 = a0;
      e.sta = sta;
      e.len = L;
      e.pi = {Q[i].x, Q[i].y};
      e.pi_index = static_cast<double>(i);
      e.T = T;
      e.E = r * (1 / js_cos(delta / 2) - 1);
      e.M = r * (1 - js_cos(delta / 2));
      e.chord = 2 * r * js_sin(delta / 2);
      e.dir_in = dir;
      e.dir_out = dir2;
      elements.push_back(e);
      sta += L;
      cur = pt;
    } else {
      cur = {Q[i].x, Q[i].y};
    }
  }
  return std::make_shared<Alignment>(std::move(elements), start, std::vector<std::string>{}, std::move(Q));
}

namespace {
double total_length(const std::vector<Element>& els) {
  double sum = 0;
  for (const Element& e : els) {
    sum += e.len;
  }
  return sum;
}
}  // namespace

Alignment::Alignment(std::vector<Element> elements_in, double start_in, std::vector<std::string> errors_in,
                     std::vector<Pi> pis_in)
    : elements(std::move(elements_in)),
      start(start_in),
      errors(std::move(errors_in)),
      pis(std::move(pis_in)),
      length(total_length(elements)),
      end(start + length) {}

const Element* Alignment::element_at(double sta) const {
  if (elements.empty()) {
    return nullptr;
  }
  for (const Element& e : elements) {
    if (sta <= e.sta + e.len + 1e-9) {
      return &e;
    }
  }
  return &elements.back();
}

std::optional<AlPoint> Alignment::point_at(double sta) const {
  const Element* e = element_at(sta);
  if (e == nullptr) {
    return std::nullopt;
  }
  const double s = sta - e->sta;
  if (!e->arc) {
    return AlPoint{e->p0.x + js_cos(e->dir) * s, e->p0.y + js_sin(e->dir) * s, e->dir};
  }
  const double sw = (e->ccw ? 1 : -1) * s / e->r;
  const double a = e->a0 + sw;
  return AlPoint{e->c.x + js_cos(a) * e->r, e->c.y + js_sin(a) * e->r, a + (e->ccw ? kPi / 2 : -kPi / 2)};
}

std::optional<StaOff> Alignment::station_offset(double x, double y) const {
  struct Best {
    double d, sta, off, x, y, dir;
  };
  std::optional<Best> best;
  for (const Element& e : elements) {
    double s = 0;
    double px = 0;
    double py = 0;
    double dir = 0;
    if (!e.arc) {
      const double ux = js_cos(e.dir);
      const double uy = js_sin(e.dir);
      s = jmax(0, jmin(e.len, (x - e.p0.x) * ux + (y - e.p0.y) * uy));
      px = e.p0.x + ux * s;
      py = e.p0.y + uy * s;
      dir = e.dir;
    } else {
      const double a = js_atan2(y - e.c.y, x - e.c.x);
      double sw = std::fmod((a - e.a0) * (e.ccw ? 1 : -1), kTau);
      if (sw < 0) {
        sw += kTau;
      }
      if (sw > e.delta) {
        sw = sw - e.delta < kTau - sw ? e.delta : 0;
      }
      s = sw * e.r;
      const double aa = e.a0 + (e.ccw ? sw : -sw);
      px = e.c.x + js_cos(aa) * e.r;
      py = e.c.y + js_sin(aa) * e.r;
      dir = aa + (e.ccw ? kPi / 2 : -kPi / 2);
    }
    const double d = hyp(x - px, y - py);
    if (!best || d < best->d - 1e-12) {
      const double off = (x - px) * js_sin(dir) - (y - py) * js_cos(dir);
      best = Best{d, e.sta + s, off, px, py, dir};
    }
  }
  if (!best) {
    return std::nullopt;
  }
  auto along = [&](const AlPoint& p) { return (x - p.x) * js_cos(p.dir) + (y - p.y) * js_sin(p.dir); };
  const AlPoint p0 = point_at(start).value_or(kNoPoint);  // elements exist here
  const AlPoint p1 = point_at(end).value_or(kNoPoint);
  const bool outside =
      (best->sta <= start + 1e-9 && along(p0) < -1e-9) || (best->sta >= end - 1e-9 && along(p1) > 1e-9);
  return StaOff{best->sta, best->off, best->x, best->y, outside, best->d};
}

std::vector<Alignment::PolyPt> Alignment::polyline(double max_seg, double max_turn) const {
  std::vector<PolyPt> out;
  for (const Element& e : elements) {
    if (out.empty()) {
      out.push_back({e.p0.x, e.p0.y, e.sta});
    }
    if (!e.arc) {
      out.push_back({e.p1.x, e.p1.y, e.sta + e.len});
      continue;
    }
    double n =
        std::max({1.0, std::ceil(e.delta / max_turn), std::isfinite(max_seg) ? std::ceil(e.len / max_seg) : 1.0});
    if (std::isnan(std::ceil(e.delta / max_turn)) ||
        (std::isfinite(max_seg) && std::isnan(std::ceil(e.len / max_seg)))) {
      n = std::nan("");  // Math.max with NaN is NaN: the JS loop `k <= NaN` adds nothing
    }
    if (!(n >= 1)) {
      continue;
    }
    n = std::min(n, kMaxPolylinePieces);  // hostile input only (docs/gaps.md)
    for (std::int64_t q = 1; static_cast<double>(q) <= n; ++q) {
      const auto k = static_cast<double>(q);
      const double s = e.sta + (e.len * k) / n;
      const AlPoint p = point_at(s).value_or(kNoPoint);
      out.push_back({p.x, p.y, s});
    }
  }
  return out;
}

std::vector<Alignment::Tick> Alignment::ticks(double step) const {
  std::vector<Tick> out;
  if (!(step > 0) || elements.empty()) {
    return out;
  }
  const double k0 = std::ceil(start / step - 1e-9);
  const double k1 = std::floor(end / step + 1e-9);
  if (k1 - k0 > 20000) {
    return out;
  }
  for (std::int64_t q = 0; k0 + static_cast<double>(q) <= k1; ++q) {  // k1 − k0 ≤ 20000: exact
    const double sta = (k0 + static_cast<double>(q)) * step;
    const AlPoint p = point_at(sta).value_or(kNoPoint);
    out.push_back({sta, p.x, p.y, p.dir});
  }
  return out;
}

std::vector<Alignment::Key> Alignment::key_stations() const {
  std::vector<Key> out{{start, "Start"}};
  for (const Element& e : elements) {
    if (e.arc) {
      out.push_back({e.sta, "PC"});
      out.push_back({e.sta + e.len, "PT"});
    }
  }
  out.push_back({end, "End"});
  return out;
}

// ---------------------------------------------------------------------------------------------- profiles

std::vector<GroundSample> sample_surface(const Surface* surface, const Alignment& al, std::optional<double> step) {
  std::vector<GroundSample> out;
  if (surface == nullptr || !al.ok()) {
    return out;
  }
  const double st = (step && *step > 0) ? *step : std::max(0.5, al.length / 1000);
  StaSet stas;
  double s = al.start;  // the JS's `s += st` (a float sum, kept as is)
  for (std::int64_t count = 0; s < al.end && static_cast<double>(count) < kMaxSamples; ++count) {
    stas.add(base::js_number(base::js_to_fixed(s, 6)));
    s += st;
  }
  stas.add(al.end);
  for (const auto& k : al.key_stations()) {
    stas.add(k.sta);
  }
  for (const double sta : stas.sorted()) {
    const AlPoint p = al.point_at(sta).value_or(kNoPoint);
    out.push_back({sta, surface->elevation_at(p.x, p.y, 1e-6)});
  }
  return out;
}

std::shared_ptr<Profile> build_profile(const base::Value& pvis_in, const i18n::Catalog& words) {
  auto pr = std::make_shared<Profile>();
  std::vector<base::Value> pv;
  if (pvis_in.is_array()) {
    for (const base::Value& p : pvis_in.as_array()) {
      if (!p.is_object()) {
        continue;
      }
      const base::Value s = p.get("sta");
      const base::Value z = p.get("z");
      if (s.is_number() && std::isfinite(s.as_number()) && z.is_number() && std::isfinite(z.as_number())) {
        pv.push_back(p);
      }
    }
  }
  appkit::js_sort(pv, [](const base::Value& a, const base::Value& b) {
    return a.get("sta").as_number() - b.get("sta").as_number();
  });
  for (const base::Value& p : pv) {
    pr->sta.push_back(p.get("sta").as_number());
    pr->z.push_back(p.get("z").as_number());
  }
  const std::size_t n = pv.size();
  for (std::size_t i = 1; i < n; ++i) {
    if (pr->sta[i] - pr->sta[i - 1] < 1e-6) {
      pr->errors.push_back(words.t("prof.err.two", {{"sta", appkit::num_str(pr->sta[i])}}));
    }
  }
  for (std::size_t i = 0; i + 1 < n; ++i) {
    pr->grades.push_back({pr->sta[i], pr->sta[i + 1], (pr->z[i + 1] - pr->z[i]) / (pr->sta[i + 1] - pr->sta[i])});
  }
  for (std::size_t i = 1; i + 1 < n; ++i) {
    base::Value& v = pv[i];
    const double g1 = pr->grades[i - 1].g;
    const double g2 = pr->grades[i].g;
    const double A = (g2 - g1) * 100;
    double L = number_or_zero(v.get("L"));
    const double K = base::js_to_number(v.get("K"));
    if (L == 0 && K > 0) {
      L = K * std::abs(A);
    }
    v.set("L", L);
    if (L <= 0 || std::abs(A) < 1e-9) {
      continue;
    }
    ProfileCurve c;
    c.i = static_cast<double>(i);
    c.pvi = pr->sta[i];
    c.z_pvi = pr->z[i];
    c.bvc = pr->sta[i] - L / 2;
    c.evc = pr->sta[i] + L / 2;
    c.L = L;
    c.g1 = g1;
    c.g2 = g2;
    c.A = A;
    c.K = L / std::abs(A);
    c.z_bvc = pr->z[i] - g1 * L / 2;
    c.z_evc = pr->z[i] + g2 * L / 2;
    c.crest = g2 < g1;
    if (g1 * g2 < 0) {
      const double x = -g1 * L / (g2 - g1);
      c.extreme = std::pair{c.bvc + x, c.z_bvc + g1 * x + (g2 - g1) * x * x / (2 * L)};
    }
    pr->curves.push_back(c);
  }
  for (std::size_t k = 0; k < pr->curves.size(); ++k) {
    const ProfileCurve& c = pr->curves[k];
    const double prev_end = k != 0 ? pr->curves[k - 1].evc : pr->sta[0];
    const auto ci = static_cast<std::size_t>(c.i);
    const double left = pr->sta[ci - 1];
    const double right = pr->sta[ci + 1];
    if (c.bvc < left - 1e-9 || c.bvc < prev_end - 1e-9) {
      pr->errors.push_back(
          words.t("prof.err.before", {{"pvi", appkit::num_str(c.i + 1)}, {"len", base::js_to_fixed(c.L, 2)}}));
    }
    if (c.evc > right + 1e-9) {
      pr->errors.push_back(
          words.t("prof.err.past", {{"pvi", appkit::num_str(c.i + 1)}, {"len", base::js_to_fixed(c.L, 2)}}));
    }
  }
  pr->pvis = std::move(pv);
  pr->start = n != 0 ? pr->sta[0] : 0;
  pr->end = n != 0 ? pr->sta[n - 1] : 0;
  return pr;
}

std::optional<double> Profile::elevation_at(double s) const {
  const std::size_t n = sta.size();
  if (n < 2 || s < start - 1e-9 || s > end + 1e-9) {
    return std::nullopt;
  }
  for (const ProfileCurve& c : curves) {
    if (s >= c.bvc && s <= c.evc) {
      const double x = s - c.bvc;
      return c.z_bvc + c.g1 * x + (c.g2 - c.g1) * x * x / (2 * c.L);
    }
  }
  for (std::size_t i = 0; i + 1 < n; ++i) {
    if (s <= sta[i + 1] + 1e-9) {
      return z[i] + grades[i].g * (s - sta[i]);
    }
  }
  return std::nullopt;
}

std::optional<double> Profile::grade_at(double s) const {
  for (const ProfileCurve& c : curves) {
    if (s >= c.bvc && s <= c.evc) {
      return c.g1 + (c.g2 - c.g1) * (s - c.bvc) / c.L;
    }
  }
  for (const Grade& g : grades) {
    if (s <= g.to + 1e-9) {
      return g.g;
    }
  }
  return std::nullopt;
}

std::vector<Profile::Sample> Profile::sample(std::optional<double> step) const {
  StaSet s;
  for (const double v : sta) {
    s.add(v);
  }
  for (const ProfileCurve& c : curves) {
    const double div = (step && *step != 0 && !std::isnan(*step)) ? *step : c.L / 32;
    double n = std::max(8.0, std::ceil(c.L / div));
    if (std::isnan(std::ceil(c.L / div))) {
      continue;  // Math.max(8, NaN) is NaN: no points
    }
    n = std::min(n, kMaxSamples);  // hostile input only (docs/gaps.md)
    for (std::int64_t q = 0; static_cast<double>(q) <= n; ++q) {
      s.add(c.bvc + (c.L * static_cast<double>(q)) / n);
    }
  }
  std::vector<Sample> out;
  for (const double v : s.sorted()) {
    out.push_back({v, elevation_at(v)});
  }
  return out;
}

}  // namespace cc::civil
