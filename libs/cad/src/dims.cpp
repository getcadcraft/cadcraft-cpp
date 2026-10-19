// SPDX-License-Identifier: Apache-2.0
// dims.js, dimstyle.js and units.js formatLength from CadCraft 0.5, with the JS's order of operations.
#include "cc/cad/dims.hpp"

#include <array>
#include <cmath>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/cad/geom.hpp"
#include "props.hpp"
#include "util.hpp"

namespace cc::cad {

using base::Array;
using base::Value;
using detail::g;
using detail::kTau;
using detail::norm_angle;
using detail::num;
using detail::str;

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kD2r = kPi / 180;

struct P {
  double x = 0, y = 0;
};
P pt(const Value& v) {
  return {num(g(v, "x")), num(g(v, "y"))};
}
Value val(P p) {
  return base::pt2(p.x, p.y);
}
double dist(P a, P b) {
  return base::js_hypot(b.x - a.x, b.y - a.y);
}
double angle_of(P a, P b) {
  return base::js_atan2(b.y - a.y, b.x - a.x);
}
P polar(P p, double ang, double d) {
  return {p.x + cc::base::js_cos(ang) * d, p.y + cc::base::js_sin(ang) * d};
}
bool on_sweep(double a, double a0, double a1, double tol = 1e-9) {
  const double sweep = a1 - a0;
  if (sweep >= kTau - tol) {
    return true;
  }
  const double d = norm_angle(a - a0);
  return d <= sweep + tol || d >= kTau - tol;
}
/// a ToInt32 of a value, as `v | 0`
std::int32_t i32(const Value& v) {
  return base::js_to_int32(num(v));
}
bool is_num(const Value& v, double n) {
  return v.is_number() && v.as_number() == n;
}
/// { ...a, ...b } for plain objects
Value merged(const Value& a, const Value& b) {
  Value out = a;
  if (b.is_object() && !b.is_point()) {
    for (const base::Member* m : base::js_members(b.as_object())) {
      out.set(m->key, m->value);
    }
  }
  return out;
}
/// dims.js style(e): DIM_DEFAULTS with e.st over it
Value style(const Value& e) {
  return merged(detail::dim_defaults(), g(e, "st"));
}
/// dimstyle.js full(e): DIM_EXTRA, then DIM_DEFAULTS' values, then e.st
Value full(const Value& e) {
  return merged(merged(detail::dim_extra(), detail::dim_defaults()), g(e, "st"));
}
/// s.replace(/<>/g, v) (v has no $ patterns in practice: digits, units, signs)
std::string replace_all(std::string s, std::string_view from, std::string_view to) {
  for (std::size_t k = s.find(from); k != std::string::npos; k = s.find(from, k + to.size())) {
    s.replace(k, from.size(), to);
  }
  return s;
}
double pow10(double n) {
  return std::pow(10.0, n);
}

// ---------------------------------------------------------------- units.js

int lunit_code(const Value& v) {
  // LUNITS[v | 0] && Number(v) === (v | 0) ? v | 0 : 2
  const std::int32_t k = i32(v);
  return k >= 1 && k <= 5 && num(v) == k ? k : 2;
}
int prec_of(const Value& prec) {
  const double p = num(prec);
  if (std::isfinite(p) && !prec.is_null() && !(prec.is_string() && prec.as_string().empty())) {
    return static_cast<int>(std::max(0.0, std::min(8.0, base::js_round(p))));
  }
  return 4;
}
std::string decimals(double v, int prec) {
  const double a = std::abs(v) < 0.5 * pow10(-prec) ? 0 : v;
  return str(Value(base::js_number(base::js_to_fixed(a, prec))));
}
struct Frac {
  double whole, num, den;
};
Frac fraction(double v, double den) {
  const double n = base::js_round(std::abs(v) * den);
  const double whole = std::floor(n / den);
  double nu = n - whole * den;
  double d = den;
  while (nu != 0 && std::fmod(nu, 2) == 0 && std::fmod(d, 2) == 0) {
    nu /= 2;
    d /= 2;
  }
  return {whole, nu, d};
}
std::string n2s(double v) {
  return str(Value(v));
}

// ---------------------------------------------------------------- dimstyle.js values

std::string decimal(double v, const Value& dec, bool lead, bool trail, std::string_view sep) {
  const double rd = base::js_round(num(dec));
  const int d = std::max(0, std::min(8, static_cast<int>(base::js_to_int32(rd))));
  std::string s = base::js_to_fixed(std::abs(v) < 0.5 * pow10(-d) ? 0 : v, d);
  if (trail && s.contains('.')) {
    while (!s.empty() && s.back() == '0') {
      s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
      s.pop_back();
    }
  }
  if (lead) {  // /^(-?)0\./ → '$1.'
    const std::size_t k = !s.empty() && s[0] == '-' ? 1 : 0;
    if (s.size() > k + 1 && s[k] == '0' && s[k + 1] == '.') {
      s.erase(k, 1);
    }
  }
  if (sep != ".") {
    if (const std::size_t k = s.find('.'); k != std::string::npos) {
      s.replace(k, 1, sep);
    }
  }
  return s;
}
double round_to(double v, double r) {
  return r > 0 ? base::js_round(v / r) * r : v;
}
std::string linear(double v, const Value& unit, const Value& dec, const Value& zin, std::string_view sep) {
  const int code = lunit_code(unit.truthy() ? unit : Value(2));
  if (code != 2) {
    return format_length(v, Value(code), dec);
  }
  const std::int32_t z = i32(zin);
  return decimal(v, dec, (z & 4) != 0, (z & 8) != 0, sep);
}
std::string angle(double rads, const Value& aunit, const Value& adec, const Value& azin, std::string_view sep) {
  const std::int32_t az = i32(azin);
  const bool lead = (az & 1) != 0;
  const bool trail = (az & 2) != 0;
  const double d = rads / kD2r;
  switch (i32(aunit)) {
    case 1: {
      const double n = std::max(0, i32(adec));
      const std::string sgn = d < 0 ? "-" : "";
      const double a = std::abs(d);
      if (n == 0) {
        return sgn + n2s(base::js_round(a)) + "°";
      }
      double deg = std::floor(a);
      double min = (a - deg) * 60;
      if (n <= 2) {
        min = base::js_round(min);
        if (min == 60) {
          ++deg;
          min = 0;
        }
        return sgn + n2s(deg) + "°" + n2s(min) + "'";
      }
      double m = std::floor(min);
      double sec = (min - m) * 60;
      const int sd = static_cast<int>(std::max(0.0, n - 4));
      sec = base::js_number(base::js_to_fixed(sec, sd));
      if (sec >= 60) {
        sec = 0;
        ++m;
      }
      if (m >= 60) {
        m = 0;
        ++deg;
      }
      return sgn + n2s(deg) + "°" + n2s(m) + "'" + decimal(sec, Value(sd), false, trail, sep) + "\"";
    }
    case 2: return decimal(d / 0.9, adec, lead, trail, sep) + "g";
    case 3: return decimal(rads, adec, lead, trail, sep) + "r";
    default: return decimal(d, adec, lead, trail, sep) + "°";
  }
}

struct Tol {
  std::string kind;  // "" none, sym, dev, lim, basic
  std::string text, upper, lower;
};
struct DimVal {
  std::string main;
  Tol tol;
};

DimVal dim_value(const Value& e, double v) {
  const Value st = full(e);
  const std::string sep = st["dsep"] == Value(",") ? "," : ".";
  const bool angular = g(e, "kind") == Value("angular");
  DimVal out;
  if (angular) {
    out.main = angle(v, st["aunit"], st["adec"], st["azin"], sep);
  } else {
    const double lfac = num(st["lfac"]);
    const double m = round_to(v * (std::isfinite(lfac) && lfac != 0 ? std::abs(lfac) : 1), num(st["rnd"]));
    out.main = linear(m, st["lunit"], st["dec"], st["zin"], sep);
    if (st["alt"].truthy()) {
      const Value& altf = st["altf"];
      const double a = round_to(m * (altf.truthy() ? num(altf) : 25.4), num(st["altrnd"]));
      out.main += " [" + str(detail::or_(st["apre"], "")) + linear(a, st["altu"], st["altd"], st["altz"], sep) +
                  str(detail::or_(st["asuf"], "")) + "]";
    }
  }
  const Value kind = g(e, "kind");
  std::string sym;
  if (kind == Value("radius")) {
    sym = "R";
  } else if (kind == Value("diameter")) {
    sym = "⌀";
  }
  out.main = (st["pre"].truthy() ? str(st["pre"]) : sym) + out.main + str(detail::or_(st["suf"], ""));
  const auto tfmt = [&](double x) {
    return angular ? angle(x * kD2r, st["aunit"], st["tdec"], st["tzin"], sep)
                   : linear(x, st["lunit"], st["tdec"], st["tzin"], sep);
  };
  const Value& tol = st["tol"];
  const double tp = num(st["tp"]);
  const double tm = num(st["tm"]);
  if (tol == Value("sym")) {
    out.tol = {.kind = "sym", .text = "±" + tfmt(std::abs(tp)), .upper = {}, .lower = {}};
  } else if (tol == Value("dev")) {
    std::string lower;
    if (tm > 0) {
      lower = "-";
    } else if (tm < 0) {
      lower = "+";
    }
    out.tol = {.kind = "dev", .text = {}, .upper = "+" + tfmt(tp), .lower = lower + tfmt(std::abs(tm))};
  } else if (tol == Value("lim")) {
    const Value& lf = st["lfac"];
    const double b = angular ? v / kD2r : round_to(v * (lf.truthy() ? num(lf) : 1), num(st["rnd"]));
    out.tol = {.kind = "lim", .text = {}, .upper = tfmt(b + tp), .lower = tfmt(b - tm)};
    out.main.clear();
  } else if (tol == Value("basic")) {
    out.tol.kind = "basic";
  }
  return out;
}

// ---------------------------------------------------------------- dimstyle.js parts

Value seg(P a, P b, const Value& base_fields) {
  Value o = base_fields;
  o.set("type", "line");
  o.set("x1", a.x);
  o.set("y1", a.y);
  o.set("x2", b.x);
  o.set("y2", b.y);
  return o;
}
P at(P tip, double ang, double along, double across) {
  return {tip.x - cc::base::js_cos(ang) * along - cc::base::js_sin(ang) * across,
          tip.y - cc::base::js_sin(ang) * along + cc::base::js_cos(ang) * across};
}
Value hatch_of(const Value& base_fields, Array ring) {
  Value o = base_fields;
  o.set("type", "hatch");
  o.set("solid", true);
  o.set("loops", base::array_of(std::move(ring)));
  return o;
}

Array arrow_parts(const std::string& kind, P tip, double ang, double size, const Value& b) {
  const double s = size;
  const double w = s / 6;
  const auto tri = [&](double half) { return std::array<P, 3>{tip, at(tip, ang, s, half), at(tip, ang, s, -half)}; };
  if (kind == "NONE") {
    return {};
  }
  if (kind == "CLOSEDBLANK" || kind == "CLOSED") {
    const auto [a, bb, c] = tri(w);
    Array out = {seg(a, bb, b), seg(bb, c, b), seg(c, a, b)};
    if (kind == "CLOSED") {
      out.push_back(seg(tip, at(tip, ang, s, 0), b));
    }
    return out;
  }
  if (kind == "OPEN" || kind == "OPEN30" || kind == "OPEN90") {
    double half = w;
    if (kind == "OPEN30") {
      half = std::tan(15 * kD2r) * s;
    } else if (kind == "OPEN90") {
      half = s;
    }
    const auto [a, bb, c] = tri(half);
    return {seg(a, bb, b), seg(a, c, b)};
  }
  if (kind == "DOT" || kind == "DOTSMALL") {
    const double r = kind == "DOT" ? s / 4 : s / 8;
    constexpr double n = 16;
    Array ring;
    for (int k = 0; k <= 16; ++k) {  // JS's i (a number) is exact as an int
      const auto i = static_cast<double>(k);
      ring.emplace_back(tip.x + r * cc::base::js_cos(i * 2 * kPi / n));
      ring.emplace_back(tip.y + r * cc::base::js_sin(i * 2 * kPi / n));
    }
    return {hatch_of(b, std::move(ring))};
  }
  if (kind == "ORIGIN") {
    Value o = b;
    o.set("type", "circle");
    o.set("cx", tip.x);
    o.set("cy", tip.y);
    o.set("r", s / 4);
    return {o};
  }
  if (kind == "ARCHTICK" || kind == "OBLIQUE") {
    const double a = ang + kPi / 4;
    const double h = s / 2;
    Value o = seg({tip.x - cc::base::js_cos(a) * h, tip.y - cc::base::js_sin(a) * h},
                  {tip.x + cc::base::js_cos(a) * h, tip.y + cc::base::js_sin(a) * h}, b);
    if (kind == "ARCHTICK") {
      o.set("lw", 0.5);
    }
    return {o};
  }
  const auto [a, bb, c] = tri(w);
  return {hatch_of(b, Array{a.x, a.y, bb.x, bb.y, c.x, c.y, a.x, a.y})};
}

std::array<P, 4> grow(const std::array<P2, 4>& k, double gg) {
  const double ux = k[1].x - k[0].x;
  const double uy = k[1].y - k[0].y;
  double L = base::js_hypot(ux, uy);
  if (L == 0 || std::isnan(L)) {
    L = 1;
  }
  const double ax = ux / L * gg;
  const double ay = uy / L * gg;
  const double vx = k[3].x - k[0].x;
  const double vy = k[3].y - k[0].y;
  double M = base::js_hypot(vx, vy);
  if (M == 0 || std::isnan(M)) {
    M = 1;
  }
  const double bx = vx / M * gg;
  const double by = vy / M * gg;
  return {P{k[0].x - ax - bx, k[0].y - ay - by}, P{k[1].x + ax - bx, k[1].y + ay - by},
          P{k[2].x + ax + bx, k[2].y + ay + by}, P{k[3].x - ax + bx, k[3].y - ay + by}};
}

Array style_text(const Value& e, const Value& st, const Value& p) {
  Array out;
  Value tx = p;
  tx.set("color", st["clrt"].is_undefined() ? Value(0) : st["clrt"]);
  const Value trot = g(e, "trot");
  if (detail::finite(trot) && trot.as_number() != 0) {
    tx.set("rot", trot);
  } else if (st["tih"].truthy()) {
    tx.set("rot", 0);
  }
  const Value v = g(p, "measure");
  Array parts;
  bool parts_is_tx = true;  // parts = [tx], tx changed in place below
  Array stacked;
  if (detail::finite(v)) {
    const DimVal dv = dim_value(e, v.as_number());
    const std::string over = str(detail::nn(g(e, "text"), ""));
    const std::string body = over.empty() || over == "<>" ? dv.main : replace_all(over, "<>", dv.main);
    const Tol& tol = dv.tol;
    if (tol.kind == "sym") {
      tx.set("s", body + tol.text);
    } else if (tol.kind == "dev" || tol.kind == "lim") {
      const double tfac = num(st["tfac"]);
      const double h = num(tx["h"]);
      const double h2 = h * (tfac > 0 ? tfac : 1);
      const double gap = h * 0.25;
      const Value r = tx["rot"];
      const double rot = r.truthy() ? num(r) : 0;
      const double c = cc::base::js_cos(rot);
      const double s = cc::base::js_sin(rot);
      const auto stack = [&](P o, const char* ha, double shift) {
        Value up = tx;
        up.set("s", tol.upper);
        up.set("h", h2);
        up.set("ha", ha);
        up.set("va", "bottom");
        up.set("x", o.x - s * (gap / 2 + shift));
        up.set("y", o.y + c * (gap / 2 + shift));
        Value lo = tx;
        lo.set("s", tol.lower);
        lo.set("h", h2);
        lo.set("ha", ha);
        lo.set("va", "top");
        lo.set("x", o.x + s * (gap / 2 - shift));
        lo.set("y", o.y - c * (gap / 2 - shift));
        stacked.push_back(std::move(up));
        stacked.push_back(std::move(lo));
      };
      if (tol.kind == "lim" && body.empty()) {
        Value x = tx;
        x.set("s", "X");
        const auto k = text_corners(x);
        const P mid{(k[0].x + k[1].x + k[2].x + k[3].x) / 4, (k[0].y + k[1].y + k[2].y + k[3].y) / 4};
        parts_is_tx = false;
        stack(mid, "center", 0);
      } else {
        tx.set("s", body);
        const auto k = text_corners(tx);
        const P right{(k[1].x + k[2].x) / 2 + c * gap, (k[1].y + k[2].y) / 2 + s * gap};
        const double half = (k[2].x - k[1].x) * -s + (k[2].y - k[1].y) * c;
        const std::int32_t tolj = i32(st["tolj"]);
        double shift = 0;
        if (tolj == 0) {
          shift = h2 + gap / 2 - half / 2;
        } else if (tolj == 2) {
          shift = -(h2 + gap / 2 - half / 2);
        }
        stack(right, "left", shift);
      }
    } else {
      tx.set("s", body);
    }
    if (tol.kind == "basic") {
      const auto k = text_corners(tx);
      const Value& gv = st["gap"];
      const Value& sc = st["scale"];
      const double gg = std::abs(gv.truthy() ? num(gv) : 0.625) * (sc.truthy() ? num(sc) : 1);
      const auto r = grow(k, gg);
      const Value b =
          base::make_object({{"layer", detail::nn(tx["layer"], "0")}, {"color", tx["color"]}, {"role", "box"}});
      for (std::size_t i = 0; i < 4; ++i) {
        out.push_back(seg(r.at(i), r.at((i + 1) % 4), b));
      }
    }
  }
  if (parts_is_tx) {
    parts.push_back(tx);
  }
  for (auto& x : stacked) {
    parts.push_back(std::move(x));
  }
  const std::int32_t fill = i32(st["tfill"]);
  if (fill != 0 && !parts.empty()) {
    const auto k = text_corners(parts[0]);
    const Value& gv = st["gap"];
    const Value& sc = st["scale"];
    const double gg = (gv.truthy() ? num(gv) : 0.625) * (sc.truthy() ? num(sc) : 1);
    const auto r = grow(k, gg);
    const Value& fc = st["tfillclr"];
    Value fill_color("bg");
    if (fill == 2) {
      fill_color = fc.truthy() ? fc : Value(0);
    }
    Value h = base::make_object({{"layer", detail::nn(p["layer"], "0")},
                                 {"color", fill_color},
                                 {"type", "hatch"},
                                 {"solid", true},
                                 {"wipe", true},
                                 {"loops", base::array_of(Array{r[0].x, r[0].y, r[1].x, r[1].y, r[2].x, r[2].y, r[3].x,
                                                                r[3].y, r[0].x, r[0].y})},
                                 {"role", "fill"}});
    out.insert(out.begin(), std::move(h));
  }
  for (auto& x : parts) {
    out.push_back(std::move(x));
  }
  return out;
}

Array style_dim_parts(const Value& e, Array parts) {
  bool roles = false;
  for (const Value& p : parts) {
    roles = roles || p["role"].truthy();
  }
  if (!roles) {
    return parts;
  }
  const Value st = full(e);
  const Value& sv = st["scale"];
  const double S = sv.truthy() ? num(sv) : 1;
  const auto col = [](const Value& c) { return c.is_undefined() ? Value(0) : c; };
  const auto lw_of = [](Value& o, const Value& w) {
    if (is_num(w, -2) || w.is_undefined()) {
      return;
    }
    o.set("lw", is_num(w, -1) ? Value(nullptr) : w);
  };
  Array out;
  const Value* text = nullptr;
  for (const Value& p : parts) {
    const std::string role = p["role"].is_string() ? p["role"].as_string() : std::string();
    if (role == "ext1" || role == "ext2") {
      if ((role == "ext1" && st["se1"].truthy()) || (role == "ext2" && st["se2"].truthy())) {
        continue;
      }
      Value o = p;
      o.set("color", col(st["clre"]));
      lw_of(o, st["lwe"]);
      out.push_back(std::move(o));
    } else if (role == "dim1" || role == "dim2" || role == "dim") {
      if ((role == "dim1" && st["sd1"].truthy()) || (role == "dim2" && st["sd2"].truthy())) {
        continue;
      }
      Value o = p;
      o.set("color", col(st["clrd"]));
      lw_of(o, st["lwd"]);
      out.push_back(std::move(o));
    } else if (role == "arrow1" || role == "arrow2") {
      if ((role == "arrow1" && st["sd1"].truthy()) || (role == "arrow2" && st["sd2"].truthy())) {
        continue;
      }
      const double tsz = num(st["tsz"]);
      std::string kind = "ARCHTICK";
      if (!(tsz > 0)) {
        const Value k = role == "arrow1" ? st["blk1"] : st["blk2"];
        kind = k.truthy() ? str(k) : std::string();
      }
      const double size = tsz > 0 ? tsz * S * 2 : num(st["asz"]) * S;
      const Value tip = p["tip"];
      if (kind.empty() || !tip.truthy() || !detail::finite(p["ang"])) {
        Value o = p;
        o.set("color", col(st["clrd"]));
        out.push_back(std::move(o));
        continue;
      }
      const Value b =
          base::make_object({{"layer", detail::nn(p["layer"], "0")}, {"color", col(st["clrd"])}, {"role", role}});
      for (auto& x : arrow_parts(kind, pt(tip), p["ang"].as_number(), size, b)) {
        out.push_back(std::move(x));
      }
    } else if (role == "text") {
      text = &p;
    } else {
      out.push_back(p);
    }
  }
  if (text != nullptr) {
    for (auto& x : style_text(e, st, *text)) {
      out.push_back(std::move(x));
    }
  }
  return out;
}

// ---------------------------------------------------------------- dims.js

struct Sweep {
  double s, e, sweep;
};
Sweep angular_sweep(const Value& e) {
  const P c = pt(g(e, "c"));
  const double a1 = angle_of(c, pt(g(e, "p1")));
  const double a2 = angle_of(c, pt(g(e, "p2")));
  const double a_loc = angle_of(c, pt(g(e, "loc")));
  const bool first = on_sweep(norm_angle(a_loc), norm_angle(a1), norm_angle(a1) + norm_angle(a2 - a1));
  const double s = first ? a1 : a2;
  const double end = first ? a2 : a1;
  double sweep = norm_angle(end - s);
  if (sweep < 1e-12) {
    sweep = kTau;
  }
  return {norm_angle(s), norm_angle(s) + sweep, sweep};
}

double readable(double a) {
  a = norm_angle(a);
  if (a > kPi / 2 + 1e-9 && a <= 1.5 * kPi + 1e-9) {
    a -= kPi;
  }
  return norm_angle(a);
}

P nearest_on_ray(const Value& e, double a) {
  const P c = pt(g(e, "c"));
  const auto d = [&](P p) { return std::abs(norm_angle(angle_of(c, p) - a + kPi) - kPi); };
  const P p1 = pt(g(e, "p1"));
  const P p2 = pt(g(e, "p2"));
  return d(p1) <= d(p2) ? p1 : p2;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) — dims.js build, one switch
Array build(const Value& e) {
  const Value st = style(e);
  const Value& sv = st["scale"];
  const double S = sv.truthy() ? num(sv) : 1;
  const double txt = num(st["txt"]) * S;
  const double asz = num(st["asz"]) * S;
  const double exe = num(st["exe"]) * S;
  const double exo = num(st["exo"]) * S;
  const double gap = num(st["gap"]) * S;
  Array out;
  const auto line = [&](P a, P b, const char* role) {
    if (dist(a, b) > 1e-12) {
      Value o = base::make_object(
          {{"type", "line"}, {"layer", "0"}, {"color", 0}, {"x1", a.x}, {"y1", a.y}, {"x2", b.x}, {"y2", b.y}});
      if (role != nullptr) {
        o.set("role", role);
      }
      out.push_back(std::move(o));
    }
  };
  const auto arrow = [&](P tip, double ang, const char* role) {
    const P bp = polar(tip, ang + kPi, asz);
    const double w = asz / 6;
    const P l = polar(bp, ang + kPi / 2, w);
    const P r = polar(bp, ang - kPi / 2, w);
    out.push_back(base::make_object({{"type", "hatch"},
                                     {"layer", "0"},
                                     {"color", 0},
                                     {"solid", true},
                                     {"loops", base::array_of(Array{tip.x, tip.y, l.x, l.y, r.x, r.y, tip.x, tip.y})},
                                     {"role", role},
                                     {"tip", val(tip)},
                                     {"ang", norm_angle(ang)}}));
  };
  const double measure = dim_measure(e);
  const std::string s = dim_text(e);
  const auto text = [&](P p, double rot, const char* ha, const char* va) {
    out.push_back(base::make_object({{"type", "text"},
                                     {"layer", "0"},
                                     {"color", 0},
                                     {"x", p.x},
                                     {"y", p.y},
                                     {"h", txt},
                                     {"rot", norm_angle(rot)},
                                     {"s", s},
                                     {"ha", ha},
                                     {"va", va},
                                     {"wf", 1},
                                     {"role", "text"},
                                     {"measure", measure}}));
  };
  const double tw = text_advance(s) / 1000 * txt * kFontEm;
  const Value kind = g(e, "kind");
  if (kind == Value("linear") || kind == Value("aligned")) {
    const P p1 = pt(g(e, "p1"));
    const P p2 = pt(g(e, "p2"));
    const P loc = pt(g(e, "loc"));
    const Value rv = g(e, "rot");
    const double rot = rv.truthy() ? num(rv) : 0;
    const double r = kind == Value("aligned") ? angle_of(p1, p2) : rot;
    const P u{cc::base::js_cos(r), cc::base::js_sin(r)};
    const double d1 = (p1.x - loc.x) * u.x + (p1.y - loc.y) * u.y;
    const double d2 = (p2.x - loc.x) * u.x + (p2.y - loc.y) * u.y;
    const P q1{loc.x + u.x * d1, loc.y + u.y * d1};
    const P q2{loc.x + u.x * d2, loc.y + u.y * d2};
    const std::array<std::pair<P, P>, 2> ext = {std::pair{p1, q1}, std::pair{p2, q2}};
    for (std::size_t k = 0; k < 2; ++k) {
      const auto& [p, q] = ext.at(k);
      const double L = dist(p, q);
      if (L < 1e-12) {
        continue;
      }
      const double a = angle_of(p, q);
      if (exo < L + exe) {
        line(polar(p, a, std::min(exo, L)), polar(q, a, exe), k == 0 ? "ext1" : "ext2");
      }
    }
    const double len = dist(q1, q2);
    const bool inside = len >= 2 * asz + 1e-12;
    const double a12 = len > 1e-12 ? angle_of(q1, q2) : r;
    const P mid{(q1.x + q2.x) / 2, (q1.y + q2.y) / 2};
    const double ta = readable(a12);
    const double up = ta + kPi / 2;
    const double off = std::min(len / 2, 2 * asz + tw / 2 + gap);
    const Value& just = st["just"];
    P tm = mid;
    if (is_num(just, 1) && len > 1e-12) {
      tm = polar(q1, a12, off);
    } else if (is_num(just, 2) && len > 1e-12) {
      tm = polar(q2, a12 + kPi, off);
    }
    const bool fits = is_num(st["tad"], 0) && len > tw + 2 * gap + (inside ? 2 * asz : 0);
    if (inside) {
      if (fits) {
        line(q1, polar(tm, a12 + kPi, tw / 2 + gap), "dim1");
        line(polar(tm, a12, tw / 2 + gap), q2, "dim2");
      } else {
        line(q1, q2, "dim");
      }
      arrow(q1, a12 + kPi, "arrow1");
      arrow(q2, a12, "arrow2");
    } else {
      line(polar(q1, a12 + kPi, 2 * asz), polar(q2, a12, 2 * asz), "dim");
      arrow(q1, a12, "arrow1");
      arrow(q2, a12 + kPi, "arrow2");
    }
    if (fits) {
      text(tm, ta, "center", "middle");
    } else if (is_num(st["tad"], 4)) {
      text(polar(tm, up, -gap), ta, "center", "top");
    } else {
      text(polar(tm, up, gap), ta, "center", "bottom");
    }
  } else if (kind == Value("angular")) {
    const Sweep sw = angular_sweep(e);
    const double a0 = sw.s;
    const double a1 = sw.e;
    const P c = pt(g(e, "c"));
    const double R = dist(c, pt(g(e, "loc")));
    if (!(R < 1e-12)) {
      out.push_back(base::make_object({{"type", "arc"},
                                       {"layer", "0"},
                                       {"color", 0},
                                       {"cx", c.x},
                                       {"cy", c.y},
                                       {"r", R},
                                       {"a0", norm_angle(a0)},
                                       {"a1", norm_angle(a1)},
                                       {"role", "dim"}}));
      const std::array<std::pair<double, P>, 2> legs = {std::pair{a0, nearest_on_ray(e, a0)},
                                                        std::pair{a1, nearest_on_ray(e, a1)}};
      for (std::size_t k = 0; k < 2; ++k) {
        const auto& [ang, Pp] = legs.at(k);
        const double dP = dist(c, Pp);
        const char* role = k == 0 ? "ext1" : "ext2";
        if (R > dP + 1e-9) {
          line(polar(c, ang, dP + exo), polar(c, ang, R + exe), role);
        } else if (R < dP - 1e-9 && dP - exo > R - exe) {
          line(polar(c, ang, dP - exo), polar(c, ang, std::max(0.0, R - exe)), role);
        }
      }
      const P start = polar(c, a0, R);
      const P end = polar(c, a1, R);
      if (R * sw.sweep >= 2 * asz) {
        arrow(start, a0 - kPi / 2, "arrow1");
        arrow(end, a1 + kPi / 2, "arrow2");
      } else {
        arrow(start, a0 + kPi / 2, "arrow1");
        arrow(end, a1 - kPi / 2, "arrow2");
      }
      const double m = a0 + sw.sweep / 2;
      const double ta = readable(m - kPi / 2);
      const bool outward = cc::base::js_cos(ta + kPi / 2 - m) > 0;
      text(polar(c, m, R + gap), ta, "center", outward ? "bottom" : "top");
    }
  } else if (kind == Value("radius") || kind == Value("diameter")) {
    const P c = pt(g(e, "c"));
    const P p1 = pt(g(e, "p1"));
    const P loc = pt(g(e, "loc"));
    const double r = dist(c, p1);
    if (!(r < 1e-12)) {
      const double d_loc = dist(c, loc);
      const double u = d_loc > 1e-12 ? angle_of(c, loc) : angle_of(c, p1);
      const P Pp = polar(c, u, r);
      const double cm = asz / 2;
      line(polar(c, 0, -cm), polar(c, 0, cm), nullptr);
      line(polar(c, kPi / 2, -cm), polar(c, kPi / 2, cm), nullptr);
      if (d_loc > r + 1e-9) {
        line(Pp, loc, "dim");
        arrow(Pp, u + kPi, "arrow1");
        const double sx = cc::base::js_cos(u) >= 0 ? 1 : -1;
        const P land{loc.x + sx * asz, loc.y};
        line(loc, land, "dim");
        text({land.x + sx * gap, land.y}, 0, sx > 0 ? "left" : "right", "middle");
      } else if (kind == Value("radius")) {
        line(c, Pp, "dim");
        arrow(Pp, u, "arrow1");
        const double ta = readable(u);
        text(polar(polar(c, u, r / 2), ta + kPi / 2, gap), ta, "center", "bottom");
      } else {
        const P Q = polar(c, u + kPi, r);
        line(Q, Pp, "dim");
        arrow(Pp, u, "arrow1");
        arrow(Q, u + kPi, "arrow2");
        const double ta = readable(u);
        text(polar(c, ta + kPi / 2, gap), ta, "center", "bottom");
      }
    }
  }
  return style_dim_parts(e, std::move(out));
}

}  // namespace

Value dim_style(const Value& e) {
  return style(e);
}

double dim_measure(const Value& e) {
  const Value kind = g(e, "kind");
  if (kind == Value("linear")) {
    const Value rv = g(e, "rot");
    const double r = rv.truthy() ? num(rv) : 0;
    const P u{cc::base::js_cos(r), cc::base::js_sin(r)};
    const P p1 = pt(g(e, "p1"));
    const P p2 = pt(g(e, "p2"));
    return std::abs((p2.x - p1.x) * u.x + (p2.y - p1.y) * u.y);
  }
  if (kind == Value("aligned")) {
    return dist(pt(g(e, "p1")), pt(g(e, "p2")));
  }
  if (kind == Value("radius")) {
    return dist(pt(g(e, "c")), pt(g(e, "p1")));
  }
  if (kind == Value("diameter")) {
    return 2 * dist(pt(g(e, "c")), pt(g(e, "p1")));
  }
  if (kind == Value("angular")) {
    return angular_sweep(e).sweep;
  }
  return 0;
}

std::string dim_value_main(const Value& e, double v) {
  return dim_value(e, v).main;
}

std::string dim_text(const Value& e) {
  const Value tv = detail::nn(g(e, "text"), "");
  const std::string t = str(tv);
  std::string v = dim_value(e, dim_measure(e)).main;
  if (t.empty() || t == "<>") {
    return v;
  }
  return replace_all(t, "<>", v);
}

Array dim_geometry(const Value& e) {
  return build(e);
}

Value dim_text_point(const Value& e) {
  for (const Value& p : build(e)) {
    if (p["type"] == Value("text")) {
      return base::pt2(num(p["x"]), num(p["y"]));
    }
  }
  return g(e, "loc");  // { ...e.loc }
}

std::string format_length(double v, const Value& lunits, const Value& prec) {
  if (!std::isfinite(v)) {
    return {};
  }
  const int code = lunit_code(lunits);
  const int p = prec_of(prec);
  const std::string neg = v < 0 ? "-" : "";
  switch (code) {
    case 1: {
      std::string s = base::js_to_exponential(std::abs(v) < 1e-300 ? 0 : v, p);
      for (char& c : s) {
        if (c == 'e') {
          c = 'E';
        }
      }
      // /E([+-])(\d)$/ → E$10$2
      if (s.size() >= 3 && s[s.size() - 3] == 'E' && (s[s.size() - 2] == '+' || s[s.size() - 2] == '-')) {
        s.insert(s.size() - 1, "0");
      }
      return s;
    }
    case 3: {
      const double r = base::js_round(std::abs(v) * pow10(p)) / pow10(p);
      const double ft = std::floor(r / 12 + 1e-12);
      const double inch = r - ft * 12;
      if (r == 0) {
        return "0'-0\"";
      }
      return neg + n2s(ft) + "'-" + decimals(inch, p) + "\"";
    }
    case 4: {
      const double den = std::pow(2.0, p);
      const Frac f = fraction(v, den);
      const double ft = std::floor(f.whole / 12);
      const double inch = f.whole - ft * 12;
      if (f.whole == 0 && f.num == 0) {
        return "0'-0\"";
      }
      return neg + n2s(ft) + "'-" + n2s(inch) + (f.num != 0 ? " " + n2s(f.num) + "/" + n2s(f.den) : std::string()) +
             "\"";
    }
    case 5: {
      const double den = std::pow(2.0, p);
      const Frac f = fraction(v, den);
      if (f.whole == 0 && f.num == 0) {
        return "0";
      }
      std::string out = neg + (f.whole != 0 || f.num == 0 ? n2s(f.whole) : std::string());
      if (f.num != 0) {
        out += (f.whole != 0 ? " " : "") + n2s(f.num) + "/" + n2s(f.den);
      }
      return out;
    }
    default: return neg + decimals(std::abs(v), p);
  }
}

int length_code(const Value& lunits) {
  return lunit_code(lunits);
}
int length_prec(const Value& prec) {
  return prec_of(prec);
}
std::string format_decimals(double v, int prec) {
  return decimals(v, prec);
}

Value dim_style_full(const Value& e) {
  return merged(merged(detail::dim_defaults(), detail::dim_extra()), g(e, "st"));
}
std::string dim_style_text(const Value& e, double v) {
  const DimVal dv = dim_value(e, v);
  const Value over = g(e, "text");
  const std::string o = over.is_undefined() || over.is_null() ? std::string() : str(over);
  std::string body = o.empty() || o == "<>" ? dv.main : replace_all(o, "<>", dv.main);
  if (dv.tol.kind.empty() || dv.tol.kind == "none") {
    return body;
  }
  if (dv.tol.kind == "sym") {
    return body + dv.tol.text;
  }
  if (dv.tol.kind == "dev") {
    return body + " " + dv.tol.upper + "/" + dv.tol.lower;
  }
  if (dv.tol.kind == "lim") {
    return dv.tol.upper + "/" + dv.tol.lower;
  }
  return "[" + body + "]";
}

}  // namespace cc::cad
