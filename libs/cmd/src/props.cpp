// SPDX-License-Identifier: Apache-2.0
// propdefs.js (CadCraft 0.5) in C++: what the Properties panel shows for each kind of object and how a value changes
// it — every descriptor with its section, kind, getter and setter, the JS's defaults and checks.
#include "cc/cmd/props.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/cad/dims.hpp"
#include "cc/cad/edit.hpp"

namespace cc::cmd::props {

namespace {

using base::Value;
using Got = std::optional<Value>;  // nothing: varies

constexpr double kPi = std::numbers::pi;
constexpr double kTau = kPi * 2;

double norm_angle(double a) {
  a = std::fmod(a, kTau);
  return a < 0 ? a + kTau : a;
}
double deg(double r) {
  return r * 180 / kPi;
}
double rad(double d) {
  return d * kPi / 180;
}
double num(const Value& e, std::string_view k) {
  return base::js_to_number(e.get(k));
}
/// num0(v): finite numbers, else 0
double num0(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number()) ? v.as_number() : 0;
}
std::string type_of(const Value& e) {
  return base::js_string(e.get("type"));
}
/// put(e, k, v, dflt): the field written back, absent when it equals its default
Value put(const Value& e, std::string_view k, const Value& v, const Value& dflt = Value(0)) {
  Value n = e;
  if (v == dflt || v.is_undefined() || v.is_null()) {
    n.erase(k);
  } else {
    n.set(k, v);
  }
  return n;
}
SetResult ok(Value e) {
  return {.e = std::move(e), .error = {}};
}
SetResult err(const Ctx& c, std::string_view key,
              std::initializer_list<std::pair<std::string_view, std::string>> f = {}) {
  return {.e = {}, .error = c.words->t(key, f)};
}
std::string tr(const Ctx& c, std::string_view key) {
  return c.words->t(key);
}

Def def(std::string key, std::string sec, std::string kind) {
  Def d;
  d.key = std::move(key);
  d.sec = std::move(sec);
  d.kind = std::move(kind);
  return d;
}

// ---- general (every type)

std::vector<Def> general() {
  std::vector<Def> out;
  {
    Def d = def("layer", "general", "layer");
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value l = e.get("layer");
      return l.is_undefined() || l.is_null() ? Value("0") : l;
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      Value n = e;
      n.set("layer", v);
      return ok(n);
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("color", "general", "color");
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value c = e.get("color");
      return c.is_undefined() ? Value(nullptr) : c;
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      Value n = e;
      n.set("color", v);
      return ok(n);
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("lt", "general", "lt");
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value l = e.get("lt");
      return l.is_undefined() ? Value(nullptr) : l;
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "lt", v, Value(nullptr))); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("lts", "general", "num");
    d.gt0 = true;
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value l = e.get("lts");
      return l.is_number() && l.as_number() > 0 ? l : Value(1);
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "lts", v, Value(1))); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("lw", "general", "lw");
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value l = e.get("lw");
      return l.is_undefined() ? Value(nullptr) : l;
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "lw", v, Value(nullptr))); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("tr", "general", "tr");
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value l = e.get("tr");
      return l.is_undefined() ? Value(nullptr) : l;
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "tr", v, Value(nullptr))); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("th", "general", "len");
    d.types = {"line", "circle", "arc", "poly", "point", "text"};
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(num0(e.get("th"))); };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "th", v)); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("mat", "general", "mat");
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value l = e.get("mat");
      return l.is_undefined() || l.is_null() ? Value(nullptr) : l;
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "mat", v, Value(nullptr))); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("ps", "general", "ro");
    d.title = "props.ps.title";
    d.get = [](const Value&, const Ctx& c) -> Got { return Value(tr(c, "props.ps.bycolor")); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("url", "general", "text");
    d.max = 2048;
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value u = e.get("url");
      return u.truthy() ? u : Value("");
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      std::string s(base::js_trim(base::js_string(v)));
      if (base::utf16_length(s) > 2048) {
        s = s.substr(0, 2048);  // .slice(0, 2048): code units; ASCII in practice
      }
      return ok(put(e, "url", Value(s), Value("")));
    };
    out.push_back(std::move(d));
  }
  return out;
}

// ---- points (P.3)

/// point(name, keys): X, Y (and Z) of one point; a key "" leaves that axis out
std::vector<Def> point(const std::string& name, const std::string& kx, const std::string& ky, const std::string& kz,
                       const std::function<bool(const Value&, const Ctx&)>& when = {}) {
  std::vector<Def> out;
  for (const auto& [a, k] :
       {std::pair{std::string("x"), kx}, std::pair{std::string("y"), ky}, std::pair{std::string("z"), kz}}) {
    if (k.empty()) {
      continue;
    }
    Def d = def(std::string(name).append(".").append(a), "geometry", "len");
    d.pt_name = name;
    d.pt_axis = a;
    d.label = "props.f." + a;  // translated in defs_for
    d.when = when;
    const bool z = a == "z";
    d.get = [k_ = k, z](const Value& e, const Ctx&) -> Got { return z ? Value(num0(e.get(k_))) : e.get(k_); };
    d.set = [k_ = k, z](const Value& e, const Value& v, const Ctx&) {
      if (z) {
        return ok(put(e, k_, v));
      }
      Value n = e;
      n.set(k_, v);
      return ok(n);
    };
    out.push_back(std::move(d));
  }
  return out;
}

// ---- small helpers of the JS

/// polyCentre(e): the centroid of a closed polyline's corners
std::pair<double, double> poly_centre(const Value& e) {
  const base::Array& p = e.get("pts").as_array();
  const std::size_t n = p.size();
  double a = 0;
  double cx = 0;
  double cy = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const Value& u = p[i];
    const Value& v = p[(i + 1) % n];
    const double c = num(u, "x") * num(v, "y") - num(v, "x") * num(u, "y");
    a += c;
    cx += (num(u, "x") + num(v, "x")) * c;
    cy += (num(u, "y") + num(v, "y")) * c;
  }
  if (std::abs(a) < 1e-12) {
    double sx = 0;
    double sy = 0;
    for (const Value& q : p) {
      sx += num(q, "x");
      sy += num(q, "y");
    }
    return {sx / static_cast<double>(n), sy / static_cast<double>(n)};
  }
  return {cx / (3 * a), cy / (3 * a)};
}
double area_of(const Value& e) {
  const auto a = cad::entity_area(e);
  return a ? std::abs(a->area) : 0;
}

std::size_t vtx(const Value& e, const Ctx& c) {
  const std::size_t n = e.get("pts").as_array().size();
  if (n == 0) {
    return 0;
  }
  return static_cast<std::size_t>(std::max(0, std::min(static_cast<int>(n) - 1, c.vertex)));
}
SetResult vset(const Value& e, const Ctx& c, const std::string& k, const Value& v) {
  const std::size_t i = vtx(e, c);
  base::Array pts = e.get("pts").as_array();
  Value& q = pts[i];
  if (v == Value(0) && k != "b") {
    q.erase(k);
  } else {
    q.set(k, v);
  }
  Value n = e;
  n.set("pts", std::move(pts));
  return ok(n);
}

/// paramToAngle / angleToParam (ellipse)
double param_to_angle(double tp, double ratio) {
  return norm_angle(base::js_atan2(ratio * base::js_sin(tp), base::js_cos(tp)));
}
double angle_to_param(double a, double ratio) {
  return norm_angle(base::js_atan2(base::js_sin(a) / (ratio != 0 && !std::isnan(ratio) ? ratio : 1), base::js_cos(a)));
}
Value mst(const Value& e) {  // { asz: 2.5, dogleg: 2.5, gap: 1, ...(e.st || {}) }
  Value s = base::make_object({{"asz", 2.5}, {"dogleg", 2.5}, {"gap", 1}});
  const Value st = e.get("st");
  if (st.is_object()) {
    for (const base::Member* m : base::js_members(st.as_object())) {
      s.set(m->key, m->value);
    }
  }
  return s;
}

std::vector<std::pair<Value, std::string>> text_styles(const Ctx& c) {
  std::vector<std::string> names{"Standard"};
  const auto add = [&names](const std::string& n) {
    if (std::ranges::find(names, n) == names.end()) {
      names.push_back(n);
    }
  };
  if (c.doc != nullptr) {
    c.doc->text_styles.each([&](const std::string& n, const Value&) { add(n); });
  }
  for (const Value& e : c.ents) {
    if (e.get("style").truthy()) {
      add(base::js_string(e.get("style")));
    }
  }
  std::vector<std::pair<Value, std::string>> out;
  out.reserve(names.size());
  for (const std::string& n : names) {
    out.emplace_back(Value(n), n);
  }
  return out;
}

// ---- per type

std::vector<Def> line_defs() {
  std::vector<Def> out = point("start", "x1", "y1", "z1");
  for (Def& d : point("end", "x2", "y2", "z2")) {
    out.push_back(std::move(d));
  }
  const auto ro_len = [&out](std::string key, std::function<double(const Value&)> f) {
    Def d = def(std::move(key), "geometry", "ro-len");
    d.get = [fn = std::move(f)](const Value& e, const Ctx&) -> Got { return Value(fn(e)); };
    out.push_back(std::move(d));
  };
  ro_len("dx", [](const Value& e) { return num(e, "x2") - num(e, "x1"); });
  ro_len("dy", [](const Value& e) { return num(e, "y2") - num(e, "y1"); });
  ro_len("dz", [](const Value& e) { return num0(e.get("z2")) - num0(e.get("z1")); });
  {
    Def d = def("len", "geometry", "len");
    d.gt0 = true;
    d.get = [](const Value& e, const Ctx&) -> Got {
      return Value(base::js_hypot(num(e, "x2") - num(e, "x1"), num(e, "y2") - num(e, "y1")));
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      const double a = base::js_atan2(num(e, "y2") - num(e, "y1"), num(e, "x2") - num(e, "x1"));
      Value n = e;
      n.set("x2", num(e, "x1") + base::js_cos(a) * base::js_to_number(v));
      n.set("y2", num(e, "y1") + base::js_sin(a) * base::js_to_number(v));
      return ok(n);
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("ang", "geometry", "ang");
    d.get = [](const Value& e, const Ctx&) -> Got {
      return Value(deg(norm_angle(base::js_atan2(num(e, "y2") - num(e, "y1"), num(e, "x2") - num(e, "x1")))));
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      const double l = base::js_hypot(num(e, "x2") - num(e, "x1"), num(e, "y2") - num(e, "y1"));
      const double r = rad(base::js_to_number(v));
      Value n = e;
      n.set("x2", num(e, "x1") + base::js_cos(r) * l);
      n.set("y2", num(e, "y1") + base::js_sin(r) * l);
      return ok(n);
    };
    out.push_back(std::move(d));
  }
  return out;
}

Def simple_len(std::string key, std::string kind, std::function<double(const Value&)> get,
               std::function<Value(const Value&, double)> set, bool gt0 = true) {
  Def d = def(std::move(key), "geometry", std::move(kind));
  d.gt0 = gt0;
  d.get = [getter = std::move(get)](const Value& e, const Ctx&) -> Got { return Value(getter(e)); };
  if (set) {
    d.set = [setter = std::move(set)](const Value& e, const Value& v, const Ctx&) {
      return ok(setter(e, base::js_to_number(v)));
    };
  }
  return d;
}
Value with(const Value& e, std::string_view k, const Value& v) {
  Value n = e;
  n.set(k, v);
  return n;
}

std::vector<Def> circle_defs() {
  std::vector<Def> out = point("center", "cx", "cy", "z");
  out.push_back(simple_len(
      "r", "len", [](const Value& e) { return num(e, "r"); },
      [](const Value& e, double v) { return with(e, "r", v); }));
  out.push_back(simple_len(
      "d", "len", [](const Value& e) { return num(e, "r") * 2; },
      [](const Value& e, double v) { return with(e, "r", v / 2); }));
  out.push_back(simple_len(
      "circ", "len", [](const Value& e) { return kTau * num(e, "r"); },
      [](const Value& e, double v) { return with(e, "r", v / kTau); }));
  out.push_back(simple_len(
      "area", "area", [](const Value& e) { return kPi * num(e, "r") * num(e, "r"); },
      [](const Value& e, double v) { return with(e, "r", std::sqrt(v / kPi)); }));
  return out;
}

double sweep_or_tau(const Value& e) {
  const double s = norm_angle(num(e, "a1") - num(e, "a0"));
  return s != 0 && !std::isnan(s) ? s : kTau;
}

std::vector<Def> arc_defs() {
  std::vector<Def> out = point("center", "cx", "cy", "z");
  out.push_back(simple_len(
      "r", "len", [](const Value& e) { return num(e, "r"); },
      [](const Value& e, double v) { return with(e, "r", v); }));
  for (const char* k : {"a0", "a1"}) {
    const std::string key = k;
    Def d = def(key, "geometry", "ang");
    d.get = [key_ = key](const Value& e, const Ctx&) -> Got { return Value(deg(norm_angle(num(e, key_)))); };
    d.set = [key_ = key](const Value& e, const Value& v, const Ctx&) {
      return ok(with(e, key_, norm_angle(rad(base::js_to_number(v)))));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("sweep", "geometry", "ang");
    d.gt0 = true;
    d.max = 360;
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(deg(sweep_or_tau(e))); };
    d.set = [](const Value& e, const Value& v, const Ctx& c) {
      const double x = base::js_to_number(v);
      if (x >= 360 - 1e-9) {
        return err(c, "props.err.fullarc");
      }
      return ok(with(e, "a1", norm_angle(num(e, "a0") + rad(x))));
    };
    out.push_back(std::move(d));
  }
  out.push_back(
      simple_len("arclen", "ro-len", [](const Value& e) { return num(e, "r") * sweep_or_tau(e); }, {}, false));
  {
    Def d = def("chord", "geometry", "len");
    d.gt0 = true;
    d.get = [](const Value& e, const Ctx&) -> Got {
      return Value(2 * num(e, "r") * base::js_sin(sweep_or_tau(e) / 2));
    };
    d.set = [](const Value& e, const Value& v, const Ctx& c) {
      const double s = base::js_sin(sweep_or_tau(e) / 2);
      if (!(std::abs(s) > 1e-9)) {
        return err(c, "props.err.nochord");
      }
      return ok(with(e, "r", std::abs(base::js_to_number(v) / (2 * s))));
    };
    out.push_back(std::move(d));
  }
  out.push_back(simple_len(
      "area", "ro-area",
      [](const Value& e) {
        const double th = sweep_or_tau(e);
        return num(e, "r") * num(e, "r") * (th - base::js_sin(th)) / 2;
      },
      {}, false));
  return out;
}

std::vector<Def> ellipse_defs() {
  std::vector<Def> out = point("center", "cx", "cy", "z");
  out.push_back(simple_len(
      "major", "len", [](const Value& e) { return base::js_hypot(num(e, "mx"), num(e, "my")); },
      [](const Value& e, double v) {
        double a = base::js_hypot(num(e, "mx"), num(e, "my"));
        a = a != 0 && !std::isnan(a) ? a : 1;
        Value n = with(e, "mx", num(e, "mx") / a * v);
        n.set("my", num(e, "my") / a * v);
        return n;
      }));
  {
    Def d = def("minor", "geometry", "len");
    d.gt0 = true;
    d.get = [](const Value& e, const Ctx&) -> Got {
      return Value(base::js_hypot(num(e, "mx"), num(e, "my")) * num(e, "ratio"));
    };
    d.set = [](const Value& e, const Value& v, const Ctx& c) {
      const double a = base::js_hypot(num(e, "mx"), num(e, "my"));
      const double x = base::js_to_number(v);
      if (x > a + 1e-12) {
        return err(c, "props.err.minor");
      }
      return ok(with(e, "ratio", x / a));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = simple_len(
        "ratio", "num", [](const Value& e) { return num(e, "ratio"); },
        [](const Value& e, double v) { return with(e, "ratio", v); });
    d.max = 1;
    out.push_back(std::move(d));
  }
  {
    Def d = def("axisang", "geometry", "ang");
    d.get = [](const Value& e, const Ctx&) -> Got {
      return Value(deg(norm_angle(base::js_atan2(num(e, "my"), num(e, "mx")))));
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      const double a = base::js_hypot(num(e, "mx"), num(e, "my"));
      const double r = rad(base::js_to_number(v));
      Value n = with(e, "mx", base::js_cos(r) * a);
      n.set("my", base::js_sin(r) * a);
      return ok(n);
    };
    out.push_back(std::move(d));
  }
  const auto t0 = [](const Value& e) {
    const Value v = e.get("t0");
    return v.is_undefined() || v.is_null() ? 0 : base::js_to_number(v);
  };
  const auto t1 = [](const Value& e) {
    const Value v = e.get("t1");
    return v.is_undefined() || v.is_null() ? kTau : base::js_to_number(v);
  };
  {
    Def d = def("ea0", "geometry", "ang");
    d.get = [t0](const Value& e, const Ctx&) -> Got { return Value(deg(param_to_angle(t0(e), num(e, "ratio")))); };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      return ok(with(e, "t0", angle_to_param(rad(base::js_to_number(v)), num(e, "ratio"))));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("ea1", "geometry", "ang");
    d.get = [t0, t1](const Value& e, const Ctx&) -> Got {
      const double a = deg(param_to_angle(t1(e), num(e, "ratio")));
      if (a != 0 && !std::isnan(a)) {
        return Value(a);
      }
      return Value(t1(e) - t0(e) >= kTau - 1e-9 ? 360 : 0);
    };
    d.set = [t0](const Value& e, const Value& v, const Ctx&) {
      const double tp = angle_to_param(rad(base::js_to_number(v)), num(e, "ratio"));
      return ok(with(e, "t1", tp <= t0(e) + 1e-12 ? tp + kTau : tp));
    };
    out.push_back(std::move(d));
  }
  out.push_back(simple_len("area", "ro-area", area_of, {}, false));
  return out;
}

std::vector<Def> poly_defs() {
  std::vector<Def> out;
  {
    Def d = def("vertex", "geometry", "vertex");
    d.one = true;
    d.get = [](const Value& e, const Ctx& c) -> Got { return Value(static_cast<double>(vtx(e, c) + 1)); };
    out.push_back(std::move(d));
  }
  for (const char* a : {"x", "y"}) {
    const std::string ax = a;
    Def d = def("vertex." + ax, "geometry", "len");
    d.one = true;
    d.pt_name = "vertex";
    d.pt_axis = ax;
    d.label = "props.f." + ax;
    d.get = [ax_ = ax](const Value& e, const Ctx& c) -> Got { return e.get("pts").as_array()[vtx(e, c)].get(ax_); };
    d.set = [ax_ = ax](const Value& e, const Value& v, const Ctx& c) {
      base::Array pts = e.get("pts").as_array();
      pts[vtx(e, c)].set(ax_, v);
      return ok(with(e, "pts", Value(std::move(pts))));
    };
    out.push_back(std::move(d));
  }
  for (const char* k : {"sw", "ew"}) {
    const std::string key = k;
    Def d = def(std::string("v") + k, "geometry", "len");
    d.one = true;
    d.min = 0;
    d.get = [key_ = key](const Value& e, const Ctx& c) -> Got {
      const Value q = e.get("pts").as_array()[vtx(e, c)].get(key_);
      return Value(num0(q.is_undefined() || q.is_null() ? e.get("w") : q));
    };
    d.set = [key_ = key](const Value& e, const Value& v, const Ctx& c) { return vset(e, c, key_, v); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("vb", "geometry", "num");
    d.one = true;
    d.title = "props.f.vb.title";
    d.get = [](const Value& e, const Ctx& c) -> Got {
      return Value(num0(e.get("pts").as_array()[vtx(e, c)].get("b")));
    };
    d.set = [](const Value& e, const Value& v, const Ctx& c) { return vset(e, c, "b", v); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("gw", "geometry", "len");
    d.min = 0;
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(num0(e.get("w"))); };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      Value n = put(e, "w", v);
      base::Array pts = e.get("pts").as_array();
      for (Value& q : pts) {
        q.erase("sw");
        q.erase("ew");
      }
      n.set("pts", Value(std::move(pts)));
      return ok(n);
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("z", "geometry", "len");
    d.label = "props.f.elev";
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(num0(e.get("z"))); };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "z", v)); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("closed", "geometry", "bool");
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(e.get("closed").truthy()); };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(with(e, "closed", v.truthy())); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("count", "geometry", "ro");
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(std::to_string(e.get("pts").as_array().size())); };
    out.push_back(std::move(d));
  }
  out.push_back(
      simple_len("plen", "ro-len", [](const Value& e) { return cad::curve_length(e).value_or(0); }, {}, false));
  {
    Def d = def("area", "geometry", "area");
    d.gt0 = true;
    d.when = [](const Value& e, const Ctx&) { return e.get("closed").truthy(); };
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(area_of(e)); };
    d.set = [](const Value& e, const Value& v, const Ctx& c) {
      const double a0 = area_of(e);
      if (!(a0 > 0)) {
        return err(c, "props.err.noarea");
      }
      const double k = std::sqrt(base::js_to_number(v) / a0);
      const auto [cx, cy] = poly_centre(e);
      base::Array pts = e.get("pts").as_array();
      for (Value& q : pts) {
        q.set("x", cx + (num(q, "x") - cx) * k);
        q.set("y", cy + (num(q, "y") - cy) * k);
        if (q.get("sw").truthy()) {
          q.set("sw", num(q, "sw") * k);
        }
        if (q.get("ew").truthy()) {
          q.set("ew", num(q, "ew") * k);
        }
      }
      return ok(with(e, "pts", Value(std::move(pts))));
    };
    out.push_back(std::move(d));
  }
  return out;
}

constexpr std::array<std::pair<const char*, const char*>, 12> kJust{{{"left", "baseline"},
                                                                     {"center", "baseline"},
                                                                     {"right", "baseline"},
                                                                     {"left", "top"},
                                                                     {"center", "top"},
                                                                     {"right", "top"},
                                                                     {"left", "middle"},
                                                                     {"center", "middle"},
                                                                     {"right", "middle"},
                                                                     {"left", "bottom"},
                                                                     {"center", "bottom"},
                                                                     {"right", "bottom"}}};

Def text_body(bool mleader) {
  Def d = def("s", "text", "textarea");
  d.get = [](const Value& e, const Ctx&) -> Got {
    const Value s = e.get("s");
    return Value(s.is_undefined() || s.is_null() ? std::string() : base::js_string(s));
  };
  d.set = [mleader](const Value& e, const Value& v, const Ctx& c) {
    const std::string s = base::js_string(v);
    if (!mleader && base::js_blank(s)) {
      return err(c, "props.err.notext");
    }
    return ok(with(e, "s", s));
  };
  return d;
}

std::vector<Def> text_defs() {
  std::vector<Def> out;
  out.push_back(text_body(false));
  {
    Def d = def("style", "text", "choice");
    d.choices = text_styles;
    d.get = [](const Value& e, const Ctx&) -> Got {
      return e.get("style").truthy() ? e.get("style") : Value("Standard");
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "style", v, Value("Standard"))); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("just", "text", "choice");
    d.choices = [](const Ctx& c) {
      std::vector<std::pair<Value, std::string>> out2;
      out2.reserve(kJust.size());
      for (const auto& [h, v] : kJust) {
        out2.emplace_back(Value(std::string(h) + "|" + v), c.words->t(std::string("props.just.") + h + "." + v));
      }
      return out2;
    };
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value h = e.get("ha");
      const Value v = e.get("va");
      return Value((h.truthy() ? base::js_string(h) : "left") + "|" + (v.truthy() ? base::js_string(v) : "baseline"));
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      const std::string s = base::js_string(v);
      const std::size_t bar = s.find('|');
      Value n = with(e, "ha", s.substr(0, bar));
      n.set("va", bar == std::string::npos ? Value() : Value(s.substr(bar + 1)));
      return ok(n);
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("h", "text", "len");
    d.gt0 = true;
    d.get = [](const Value& e, const Ctx&) -> Got { return e.get("h"); };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(with(e, "h", v)); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("rot", "text", "ang");
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value r = e.get("rot");
      return Value(deg(norm_angle(r.truthy() ? base::js_to_number(r) : 0)));
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      return ok(put(e, "rot", Value(norm_angle(rad(base::js_to_number(v))))));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("wf", "text", "num");
    d.gt0 = true;
    d.max = 100;
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value w = e.get("wf");
      return w.is_number() && w.as_number() > 0 ? w : Value(1);
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "wf", v, Value(1))); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("obl", "text", "ang");
    d.min = -85;
    d.max = 85;
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value o = e.get("obl");
      return Value(deg(o.truthy() ? base::js_to_number(o) : 0));
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      return ok(put(e, "obl", Value(rad(base::js_to_number(v)))));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("ww", "text", "len");
    d.min = 0;
    d.title = "props.ww.title";
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(num0(e.get("ww"))); };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "ww", v)); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("ls", "text", "num");
    d.min = 0.25;
    d.max = 4;
    d.get = [](const Value& e, const Ctx&) -> Got { return e.get("ls").truthy() ? e.get("ls") : Value(1); };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "ls", v, Value(1))); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("ann", "text", "bool");
    d.title = "props.f.ann.title";
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(e.get("ann").truthy()); };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      return ok(put(e, "ann", v.truthy() ? Value(true) : Value()));
    };
    out.push_back(std::move(d));
  }
  for (Def& d : point("pos", "x", "y", "z")) {
    out.push_back(std::move(d));
  }
  return out;
}

/// dynDef(doc, e): the dynamic block a reference is of (its own, or the one it is a copy of), or none
bool is_dynamic(const Ctx& c, const Value& e) {
  if (c.doc == nullptr) {
    return false;
  }
  const Value* b = c.doc->blocks().get(base::js_string(e.get("block")));
  if (b == nullptr || !b->get("dyn").truthy()) {
    return false;
  }
  const Value dyn = b->get("dyn");
  return dyn.get("def").truthy() || c.doc->blocks().get(base::js_string(dyn.get("of"))) != nullptr;
}

std::vector<Def> insert_defs() {
  std::vector<Def> out;
  {
    Def d = def("block", "geometry", "ro");
    d.get = [](const Value& e, const Ctx& c) -> Got {
      return Value(base::js_string(e.get("block")) + (e.get("dim").truthy() ? " " + tr(c, "props.block.dim") : ""));
    };
    out.push_back(std::move(d));
  }
  for (Def& d : point("ins", "x", "y", "z")) {
    out.push_back(std::move(d));
  }
  {
    Def d = def("rot", "geometry", "ang");
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value r = e.get("rot");
      return Value(deg(norm_angle(r.truthy() ? base::js_to_number(r) : 0)));
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      return ok(put(e, "rot", Value(norm_angle(rad(base::js_to_number(v))))));
    };
    out.push_back(std::move(d));
  }
  const auto scale = [](const Value& e, std::string_view k) {
    const Value v = e.get(k);
    return v.is_undefined() || v.is_null() ? Value(1) : v;
  };
  for (const char* k : {"sx", "sy", "sz"}) {
    const std::string key = k;
    Def d = def(key, "geometry", "num");
    d.get = [key_ = key, scale](const Value& e, const Ctx&) -> Got { return scale(e, key_); };
    d.set = [key_ = key](const Value& e, const Value& v, const Ctx& c) {
      if (!v.truthy()) {
        return err(c, "props.err.zero");
      }
      return ok(key_ == "sz" ? put(e, "sz", v, Value(1)) : with(e, key_, v));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("us", "geometry", "num");
    d.title = "props.f.us.title";
    d.get = [scale](const Value& e, const Ctx&) -> Got {
      const double x = base::js_to_number(scale(e, "sx"));
      const bool uniform = std::abs(base::js_to_number(scale(e, "sy")) - x) < 1e-12 &&
                           std::abs(base::js_to_number(scale(e, "sz")) - x) < 1e-12;
      return uniform ? Got(scale(e, "sx")) : std::nullopt;
    };
    d.set = [](const Value& e, const Value& v, const Ctx& c) {
      if (!v.truthy()) {
        return err(c, "props.err.zero");
      }
      Value n = with(e, "sx", v);
      n.set("sy", v);
      return ok(put(n, "sz", v, Value(1)));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("nents", "geometry", "ro");
    d.get = [](const Value& e, const Ctx& c) -> Got {
      const Value* b = c.doc != nullptr ? c.doc->blocks().get(base::js_string(e.get("block"))) : nullptr;
      const Value ents = b != nullptr ? b->get("entities") : Value();
      return Value(std::to_string(ents.is_array() ? ents.as_array().size() : 0));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("dyn", "misc", "ro");
    d.title = "props.dyn.title";
    d.when = [](const Value& e, const Ctx& c) { return !is_dynamic(c, e); };
    d.get = [](const Value&, const Ctx& c) -> Got { return Value(tr(c, "props.dyn.none")); };
    out.push_back(std::move(d));
  }
  return out;
}

constexpr std::array<const char*, 16> kPatterns{"ANSI31", "ANSI32", "ANSI33", "ANSI34", "ANSI35", "ANSI36",
                                                "ANSI37", "ANSI38", "LINE",   "NET",    "NET3",   "DASH",
                                                "SQUARE", "BRICK",  "DOTS",   "EARTH"};
bool known_pattern(const Value& p) {
  const std::string up = base::js_to_upper(base::js_string(p));
  return std::ranges::any_of(kPatterns, [&up](const char* n) { return up == n; });
}
bool has_patlines(const Value& e) {
  const Value pl = e.get("patlines");
  return pl.is_array() && !pl.as_array().empty();
}

std::vector<Def> hatch_defs() {
  std::vector<Def> out;
  const auto not_solid = [](const Value& e, const Ctx&) { return !e.get("solid").truthy(); };
  {
    Def d = def("pattern", "pattern", "choice");
    d.when = [](const Value& e, const Ctx&) {
      return !has_patlines(e) &&
             (e.get("solid").truthy() || !e.get("pattern").truthy() || known_pattern(e.get("pattern")));
    };
    d.choices = [](const Ctx&) {
      std::vector<std::pair<Value, std::string>> c{{Value("SOLID"), "SOLID"}};
      for (const char* n : kPatterns) {
        c.emplace_back(Value(n), n);
      }
      return c;
    };
    d.get = [](const Value& e, const Ctx&) -> Got {
      if (e.get("solid").truthy()) {
        return Value("SOLID");
      }
      const Value p = e.get("pattern");
      return Value(base::js_to_upper(p.truthy() ? base::js_string(p) : "ANSI31"));
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      Value n = with(e, "solid", v == Value("SOLID"));
      if (v == Value("SOLID")) {
        n.erase("pattern");
      } else {
        n.set("pattern", v);
        if (n.get("pscale").is_undefined() || n.get("pscale").is_null()) {
          n.set("pscale", 1);
        }
        if (n.get("pangle").is_undefined() || n.get("pangle").is_null()) {
          n.set("pangle", 0);
        }
      }
      return ok(n);
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("patname", "pattern", "ro");
    d.when = [](const Value& e, const Ctx&) {
      return has_patlines(e) ||
             (!e.get("solid").truthy() && e.get("pattern").truthy() && !known_pattern(e.get("pattern")));
    };
    d.get = [](const Value& e, const Ctx& c) -> Got {
      return e.get("pattern").truthy() ? e.get("pattern") : Value(tr(c, "props.hatch.custom"));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("pscale", "pattern", "num");
    d.gt0 = true;
    d.when = not_solid;
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value p = e.get("pscale");
      return p.is_number() && p.as_number() > 0 ? p : Value(1);
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(with(e, "pscale", v)); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("pangle", "pattern", "ang");
    d.when = not_solid;
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value p = e.get("pangle");
      return Value(deg(p.truthy() ? base::js_to_number(p) : 0));
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      return ok(with(e, "pangle", norm_angle(rad(base::js_to_number(v)))));
    };
    out.push_back(std::move(d));
  }
  for (const char* a : {"x", "y"}) {
    const std::string ax = a;
    Def d = def("origin." + ax, "pattern", "len");
    d.when = not_solid;
    d.pt_name = "origin";
    d.pt_axis = ax;
    d.label = "props.f." + ax;
    d.get = [ax_ = ax](const Value& e, const Ctx&) -> Got { return Value(num0(e.get("o" + ax_))); };
    d.set = [ax_ = ax](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "o" + ax_, v)); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("assoc", "pattern", "bool");
    d.title = "props.f.assoc.title";
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(e.get("assoc").truthy()); };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      return ok(put(e, "assoc", v.truthy() ? Value(true) : Value()));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("island", "pattern", "choice");
    d.choices = [](const Ctx& c) {
      std::vector<std::pair<Value, std::string>> o;
      for (const char* k : {"normal", "outer", "ignore"}) {
        o.emplace_back(Value(k), c.words->t(std::string("props.island.") + k));
      }
      return o;
    };
    d.get = [](const Value& e, const Ctx&) -> Got {
      return e.get("island").truthy() ? e.get("island") : Value("normal");
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "island", v, Value("normal"))); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("bg", "pattern", "color");
    d.none = true;
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value b = e.get("bg");
      return b.is_undefined() ? Value("none") : b;
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      return ok(put(e, "bg", v == Value("none") ? Value() : v));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("z", "geometry", "len");
    d.label = "props.f.elev";
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(num0(e.get("z"))); };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "z", v)); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("loops", "geometry", "ro");
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value l = e.get("loops");
      return Value(std::to_string(l.is_array() ? l.as_array().size() : 0));
    };
    out.push_back(std::move(d));
  }
  out.push_back(simple_len("area", "ro-area", area_of, {}, false));
  return out;
}

std::vector<Def> mleader_defs() {
  std::vector<Def> out;
  out.push_back(text_body(true));
  {
    Def d = def("h", "text", "len");
    d.gt0 = true;
    d.get = [](const Value& e, const Ctx&) -> Got { return e.get("h").truthy() ? e.get("h") : mst(e).get("asz"); };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(with(e, "h", v)); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("ww", "text", "len");
    d.min = 0;
    d.title = "props.ww.title";
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(num0(e.get("ww"))); };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "ww", v)); };
    out.push_back(std::move(d));
  }
  {
    Def d = def("dir", "text", "choice");
    d.choices = [](const Ctx& c) {
      return std::vector<std::pair<Value, std::string>>{{Value("1"), c.words->t("props.dir.right")},
                                                        {Value("-1"), c.words->t("props.dir.left")}};
    };
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(e.get("dir") == Value(-1) ? "-1" : "1"); };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      return ok(with(e, "dir", base::js_to_number(v) == -1 ? -1 : 1));
    };
    out.push_back(std::move(d));
  }
  for (const char* k : {"asz", "dogleg"}) {
    const std::string key = k;
    Def d = def(key == "asz" ? "masz" : "dogleg", "lines", "len");
    d.min = 0;
    d.get = [key_ = key](const Value& e, const Ctx&) -> Got { return mst(e).get(key_); };
    d.set = [key_ = key](const Value& e, const Value& v, const Ctx&) {
      Value st = mst(e);
      st.set(key_, v);
      return ok(with(e, "st", st));
    };
    out.push_back(std::move(d));
  }
  for (const char* a : {"x", "y"}) {
    const std::string ax = a;
    Def d = def("arrow." + ax, "geometry", "len");
    d.pt_name = "arrow";
    d.pt_axis = ax;
    d.label = "props.f." + ax;
    d.when = [](const Value& e, const Ctx&) {
      const Value p = e.get("pts");
      return p.is_array() && !p.as_array().empty();
    };
    d.get = [ax_ = ax](const Value& e, const Ctx&) -> Got { return e.get("pts").as_array()[0].get(ax_); };
    d.set = [ax_ = ax](const Value& e, const Value& v, const Ctx&) {
      base::Array pts = e.get("pts").as_array();
      pts[0].set(ax_, v);
      return ok(with(e, "pts", Value(std::move(pts))));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("z", "geometry", "len");
    d.label = "props.f.elev";
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(num0(e.get("z"))); };
    d.set = [](const Value& e, const Value& v, const Ctx&) { return ok(put(e, "z", v)); };
    out.push_back(std::move(d));
  }
  return out;
}

std::vector<Def> viewport_defs() {
  std::vector<Def> out;
  for (const char* a : {"x", "y"}) {
    const std::string ax = a;
    Def d = def("vcenter." + ax, "geometry", "len");
    d.paper = true;
    d.pt_name = "vcenter";
    d.pt_axis = ax;
    d.label = "props.f." + ax;
    d.get = [ax_ = ax](const Value& e, const Ctx&) -> Got { return e.get(ax_); };
    d.set = [ax_ = ax](const Value& e, const Value& v, const Ctx&) { return ok(with(e, ax_, v)); };
    out.push_back(std::move(d));
  }
  for (const auto& [key, k] : {std::pair{"vw", "w"}, std::pair{"vh", "h"}}) {
    const std::string fk = k;
    Def d = def(key, "geometry", "len");
    d.paper = true;
    d.gt0 = true;
    d.get = [fk_ = fk](const Value& e, const Ctx&) -> Got { return e.get(fk_); };
    d.set = [fk_ = fk](const Value& e, const Value& v, const Ctx&) { return ok(with(e, fk_, v)); };
    out.push_back(std::move(d));
  }
  for (const auto& [a, k] : {std::pair{"x", "cx"}, std::pair{"y", "cy"}}) {
    const std::string fk = k;
    Def d = def(std::string("vview.") + a, "misc", "len");
    d.model = true;
    d.label = std::string("props.f.") + a;
    d.get = [fk_ = fk](const Value& e, const Ctx&) -> Got { return e.get(fk_); };
    d.set = [fk_ = fk](const Value& e, const Value& v, const Ctx& c) {
      if (e.get("locked").truthy()) {
        return err(c, "props.err.vplocked");
      }
      return ok(with(e, fk_, v));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("on", "misc", "bool");
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(!(e.get("on") == Value(false))); };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      return ok(put(e, "on", v.truthy() ? Value() : Value(false)));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("locked", "misc", "bool");
    d.title = "props.f.locked.title";
    d.get = [](const Value& e, const Ctx&) -> Got { return Value(e.get("locked").truthy()); };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      return ok(put(e, "locked", v.truthy() ? Value(true) : Value()));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("vname", "misc", "text");
    d.title = "props.f.vname.title";
    d.get = [](const Value& e, const Ctx&) -> Got { return e.get("vname").truthy() ? e.get("vname") : Value(""); };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      std::string s(base::js_trim(v.truthy() ? base::js_string(v) : std::string()));
      s = s.substr(0, 255);
      return ok(put(e, "vname", s.empty() ? Value() : Value(s), Value()));
    };
    out.push_back(std::move(d));
  }
  return out;
}

// ---- dimensions (P.6)

bool has_kind(const Value& e) {
  return e.get("kind").truthy();
}
bool is_ang(const Value& e) {
  return e.get("kind") == Value("angular");
}
/// S(key, sec, kind, k): a style key of the dimension, written back with its own drawing dropped
Def style_def(const std::string& key, const std::string& sec, const std::string& kind, const std::string& k,
              std::function<bool(const Value&)> when, bool to_number = false) {
  Def d = def("dim." + key, sec, kind);
  d.label = "props.dim." + key;
  d.when = [pred = std::move(when)](const Value& e, const Ctx&) { return pred(e); };
  d.get = [k_ = k](const Value& e, const Ctx&) -> Got { return cad::dim_style_full(e).get(k_); };
  d.set = [k_ = k, to_number](const Value& e, const Value& v, const Ctx&) {
    Value st = e.get("st").is_object() ? e.get("st") : Value::object();
    st.set(k_, to_number ? Value(base::js_to_number(v)) : v);
    Value n = with(e, "st", st);
    n.erase("block");
    return ok(n);
  };
  return d;
}
std::vector<std::pair<Value, std::string>> keys_of(const Ctx& c, std::string_view prefix,
                                                   std::initializer_list<int> keys) {
  std::vector<std::pair<Value, std::string>> out;
  for (const int k : keys) {
    out.emplace_back(Value(k), c.words->t(std::string(prefix) + std::to_string(k)));
  }
  return out;
}

/// the zin / azin bit of leading (lead) or trailing zeros: 4 / 8 for lengths, 1 / 2 for angles
std::uint32_t zin_bit(bool lead, bool ang) {
  if (ang) {
    return lead ? 1U : 2U;
  }
  return lead ? 4U : 8U;
}

std::vector<Def> dim_defs() {
  std::vector<Def> out;
  const auto kind = [](const Value& e) { return has_kind(e); };
  const auto lin = [](const Value& e) { return has_kind(e) && !is_ang(e); };
  {
    Def d = def("dimstyle", "general", "choice");
    d.when = [](const Value& e, const Ctx&) { return has_kind(e); };
    d.choices = [](const Ctx& c) {
      std::vector<std::string> names{"Standard"};
      const auto add = [&names](const std::string& n) {
        if (std::ranges::find(names, n) == names.end()) {
          names.push_back(n);
        }
      };
      if (c.doc != nullptr) {
        c.doc->dim_styles.each([&](const std::string& n, const Value&) { add(n); });
      }
      for (const Value& e : c.ents) {
        if (e.get("dimstyle").truthy()) {
          add(base::js_string(e.get("dimstyle")));
        }
      }
      std::vector<std::pair<Value, std::string>> o;
      o.reserve(names.size());
      for (const std::string& n : names) {
        o.emplace_back(Value(n), n);
      }
      return o;
    };
    d.get = [](const Value& e, const Ctx&) -> Got {
      return e.get("dimstyle").truthy() ? e.get("dimstyle") : Value("Standard");
    };
    d.set = [](const Value& e, const Value& v, const Ctx& c) {
      const Value* st = c.doc != nullptr ? c.doc->dim_styles.get(base::js_string(v)) : nullptr;
      const Value own = e.get("st").is_object() ? e.get("st") : Value::object();
      Value n = with(e, "st", st != nullptr ? *st : own);
      if (v == Value("Standard")) {
        n.erase("dimstyle");
      } else {
        n.set("dimstyle", v);
      }
      n.erase("block");
      return ok(n);
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("dimkind", "geometry", "ro");
    d.get = [](const Value& e, const Ctx& c) -> Got {
      if (has_kind(e)) {
        return Value(c.words->t("dimkind." + base::js_string(e.get("kind"))));
      }
      return Value(c.words->t("props.dim.fromblock", {{"block", base::js_string(e.get("block"))}}));
    };
    out.push_back(std::move(d));
  }
  {
    Def d = def("measured", "geometry", "ro");
    d.when = [](const Value& e, const Ctx&) { return has_kind(e); };
    d.get = [](const Value& e, const Ctx&) -> Got {
      return Value(cad::dim_style_text(with(e, "text", ""), cad::dim_measure(e)));
    };
    out.push_back(std::move(d));
  }
  // the points
  const auto pts = [&out](const std::string& name, const std::string& k,
                          const std::function<bool(const Value&)>& when) {
    for (const char* a : {"x", "y"}) {
      const std::string ax = a;
      Def d = def(std::string(name).append(".").append(ax), "geometry", "len");
      d.pt_name = name;
      d.pt_axis = ax;
      d.label = "props.f." + ax;
      d.when = [when](const Value& e, const Ctx&) { return when(e); };
      d.get = [k_ = k, ax_ = ax](const Value& e, const Ctx&) -> Got {
        const Value p = e.get(k_);
        return p.is_object() || p.is_point() ? p.get(ax_) : Value();
      };
      d.set = [k_ = k, ax_ = ax](const Value& e, const Value& v, const Ctx&) {
        Value p = e.get(k_).is_undefined() ? Value::object() : e.get(k_);
        p.set(ax_, v);
        Value n = with(e, k_, p);
        n.erase("block");
        return ok(n);
      };
      out.push_back(std::move(d));
    }
  };
  const auto kind_in = [](const Value& e, std::initializer_list<const char*> ks) {
    const std::string k = base::js_string(e.get("kind"));
    return std::ranges::any_of(ks, [&k](const char* x) { return k == x; });
  };
  pts("dimp1", "p1", [](const Value& e) { return has_kind(e) && e.get("p1").truthy(); });
  pts("dimp2", "p2", [kind_in](const Value& e) {
    return has_kind(e) && e.get("p2").truthy() && kind_in(e, {"linear", "aligned", "angular"});
  });
  pts("dimc", "c", [kind_in](const Value& e) {
    return has_kind(e) && e.get("c").truthy() && kind_in(e, {"angular", "radius", "diameter"});
  });
  pts("dimloc", "loc", [](const Value& e) { return has_kind(e) && e.get("loc").truthy(); });
  // Lines & arrows
  const std::vector<std::string> arrows{"",         "CLOSEDBLANK", "CLOSED",  "OPEN",   "OPEN30", "DOT",
                                        "DOTSMALL", "ARCHTICK",    "OBLIQUE", "ORIGIN", "OPEN90", "NONE"};
  const auto arrow_choices = [arrows_ = arrows](const Ctx& c) {
    std::vector<std::pair<Value, std::string>> o;
    o.reserve(arrows_.size());
    for (const std::string& k : arrows_) {
      o.emplace_back(Value(k), c.words->t("props.arrow." + base::js_to_lower(k.empty() ? "closedfilled" : k)));
    }
    return o;
  };
  for (const char* k : {"blk1", "blk2"}) {
    Def d = style_def(k, "lines", "choice", k, kind);
    d.choices = arrow_choices;
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("asz", "lines", "len", "asz", kind);
    d.min = 0;
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("tsz", "lines", "len", "tsz", kind);
    d.min = 0;
    d.title = "props.dim.tsz.title";
    out.push_back(std::move(d));
  }
  out.push_back(style_def("clrd", "lines", "color", "clrd", kind));
  out.push_back(style_def("lwd", "lines", "lw", "lwd", kind));
  out.push_back(style_def("sd1", "lines", "bool", "sd1", kind));
  out.push_back(style_def("sd2", "lines", "bool", "sd2", kind));
  out.push_back(style_def("clre", "lines", "color", "clre", lin));
  out.push_back(style_def("lwe", "lines", "lw", "lwe", lin));
  out.push_back(style_def("se1", "lines", "bool", "se1", kind));
  out.push_back(style_def("se2", "lines", "bool", "se2", kind));
  for (const char* k : {"exo", "exe"}) {
    Def d = style_def(k, "lines", "len", k, kind);
    d.min = 0;
    out.push_back(std::move(d));
  }
  // Text
  {
    Def d = def("dim.text", "text", "text");
    d.label = "props.dim.text";
    d.title = "props.dim.text.title";
    d.when = [](const Value& e, const Ctx&) { return has_kind(e); };
    d.get = [](const Value& e, const Ctx&) -> Got { return e.get("text").truthy() ? e.get("text") : Value(""); };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      const std::string s = base::js_string(v);
      Value n = with(e, "text", std::string(base::js_trim(s)) == "<>" ? std::string() : s);
      n.erase("block");
      return ok(n);
    };
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("txsty", "text", "choice", "txsty", kind);
    d.choices = text_styles;
    out.push_back(std::move(d));
  }
  out.push_back(style_def("clrt", "text", "color", "clrt", kind));
  {
    Def d = style_def("txt", "text", "len", "txt", kind);
    d.gt0 = true;
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("gap", "text", "len", "gap", kind);
    d.min = 0;
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("tad", "text", "choice", "tad", kind, true);
    d.choices = [](const Ctx& c) { return keys_of(c, "props.tad.", {0, 1, 2, 4}); };
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("just", "text", "choice", "just", lin, true);
    d.choices = [](const Ctx& c) { return keys_of(c, "props.djust.", {0, 1, 2}); };
    out.push_back(std::move(d));
  }
  out.push_back(style_def("tih", "text", "bool", "tih", kind));
  {
    Def d = def("dim.trot", "text", "ang");
    d.label = "props.dim.trot";
    d.when = [](const Value& e, const Ctx&) { return has_kind(e); };
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value r = e.get("trot");
      return Value(deg(r.truthy() ? base::js_to_number(r) : 0));
    };
    d.set = [](const Value& e, const Value& v, const Ctx&) {
      const double x = base::js_to_number(v);
      Value n = put(e, "trot", x != 0 && !std::isnan(x) ? Value(norm_angle(rad(x))) : Value());
      n.erase("block");
      return ok(n);
    };
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("tfill", "text", "choice", "tfill", kind, true);
    d.choices = [](const Ctx& c) { return keys_of(c, "props.tfill.", {0, 1, 2}); };
    out.push_back(std::move(d));
  }
  out.push_back(style_def("tfillclr", "text", "color", "tfillclr", [](const Value& e) {
    return has_kind(e) && base::js_to_int32(base::js_to_number(cad::dim_style_full(e).get("tfill"))) == 2;
  }));
  // Fit
  {
    Def d = style_def("scale", "fit", "num", "scale", kind);
    d.gt0 = true;
    d.title = "props.dim.scale.title";
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("dli", "fit", "len", "dli", kind);
    d.min = 0;
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("atfit", "fit", "choice", "atfit", kind, true);
    d.choices = [](const Ctx& c) { return keys_of(c, "props.atfit.", {0, 1, 2, 3}); };
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("tmove", "fit", "choice", "tmove", kind, true);
    d.choices = [](const Ctx& c) { return keys_of(c, "props.tmove.", {0, 1, 2}); };
    out.push_back(std::move(d));
  }
  out.push_back(style_def("tix", "fit", "bool", "tix", kind));
  out.push_back(style_def("tofl", "fit", "bool", "tofl", kind));
  // Primary units
  {
    Def d = style_def("lunit", "units", "choice", "lunit", lin, true);
    d.choices = [](const Ctx& c) { return keys_of(c, "props.lunit.", {1, 2, 3, 4, 5}); };
    d.get = [](const Value& e, const Ctx&) -> Got {
      const Value l = cad::dim_style_full(e).get("lunit");
      return l.truthy() ? l : Value(2);
    };
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("dec", "units", "int", "dec", lin);
    d.min = 0;
    d.max = 8;
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("rnd", "units", "len", "rnd", lin);
    d.min = 0;
    out.push_back(std::move(d));
  }
  for (const char* k : {"pre", "suf"}) {
    Def d = style_def(k, "units", "text", k, kind);
    d.max = 40;
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("lfac", "units", "num", "lfac", lin);
    d.gt0 = true;
    d.title = "props.dim.lfac.title";
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("dsep", "units", "choice", "dsep", kind);
    d.choices = [](const Ctx& c) {
      return std::vector<std::pair<Value, std::string>>{{Value("."), c.words->t("props.dsep.point")},
                                                        {Value(","), c.words->t("props.dsep.comma")}};
    };
    out.push_back(std::move(d));
  }
  for (const char* which : {"lead", "trail"}) {
    const bool lead = std::string(which) == "lead";
    Def d = def(std::string("dim.z") + which, "units", "bool");
    d.label = std::string("props.dim.z") + which;
    d.when = [](const Value& e, const Ctx&) { return has_kind(e); };
    d.get = [lead](const Value& e, const Ctx&) -> Got {
      const bool ang = is_ang(e);
      const auto bits = static_cast<std::uint32_t>(
          base::js_to_int32(base::js_to_number(cad::dim_style_full(e).get(ang ? "azin" : "zin"))));
      const std::uint32_t bit = zin_bit(lead, ang);
      return Value((bits & bit) != 0);
    };
    d.set = [lead](const Value& e, const Value& v, const Ctx&) {
      const bool ang = is_ang(e);
      const char* k = ang ? "azin" : "zin";
      const std::uint32_t bit = zin_bit(lead, ang);
      const auto cur = static_cast<std::uint32_t>(base::js_to_int32(base::js_to_number(cad::dim_style_full(e).get(k))));
      Value st = e.get("st").is_object() ? e.get("st") : Value::object();
      st.set(k, static_cast<double>(static_cast<std::int32_t>(v.truthy() ? (cur | bit) : (cur & ~bit))));
      Value n = with(e, "st", st);
      n.erase("block");
      return ok(n);
    };
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("aunit", "units", "choice", "aunit", is_ang, true);
    d.choices = [](const Ctx& c) { return keys_of(c, "props.aunit.", {0, 1, 2, 3}); };
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("adec", "units", "int", "adec", is_ang);
    d.min = 0;
    d.max = 8;
    out.push_back(std::move(d));
  }
  // Alternate units
  const auto alt_on = [](const Value& e) {
    return has_kind(e) && !is_ang(e) && cad::dim_style_full(e).get("alt").truthy();
  };
  out.push_back(style_def("alt", "alt", "bool", "alt", lin));
  {
    Def d = style_def("altu", "alt", "choice", "altu", alt_on, true);
    d.choices = [](const Ctx& c) { return keys_of(c, "props.lunit.", {1, 2, 3, 4, 5}); };
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("altd", "alt", "int", "altd", alt_on);
    d.min = 0;
    d.max = 8;
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("altf", "alt", "num", "altf", alt_on);
    d.gt0 = true;
    d.title = "props.dim.altf.title";
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("altrnd", "alt", "num", "altrnd", alt_on);
    d.min = 0;
    out.push_back(std::move(d));
  }
  for (const char* k : {"apre", "asuf"}) {
    Def d = style_def(k, "alt", "text", k, alt_on);
    d.max = 40;
    out.push_back(std::move(d));
  }
  // Tolerances
  const auto tol_is = [](const Value& e, std::initializer_list<const char*> ks) {
    const std::string t = base::js_string(cad::dim_style_full(e).get("tol"));
    return std::ranges::any_of(ks, [&t](const char* x) { return t == x; });
  };
  {
    Def d = style_def("tol", "tol", "choice", "tol", kind);
    d.choices = [](const Ctx& c) {
      std::vector<std::pair<Value, std::string>> o;
      for (const char* k : {"none", "sym", "dev", "lim", "basic"}) {
        o.emplace_back(Value(k), c.words->t(std::string("props.tol.") + k));
      }
      return o;
    };
    out.push_back(std::move(d));
  }
  out.push_back(style_def("tp", "tol", "len", "tp", [tol_is](const Value& e) {
    return has_kind(e) && !is_ang(e) && tol_is(e, {"sym", "dev", "lim"});
  }));
  out.push_back(style_def("tp", "tol", "ang", "tp",
                          [tol_is](const Value& e) { return is_ang(e) && tol_is(e, {"sym", "dev", "lim"}); }));
  out.push_back(style_def("tm", "tol", "len", "tm",
                          [tol_is](const Value& e) { return has_kind(e) && !is_ang(e) && tol_is(e, {"dev", "lim"}); }));
  out.push_back(
      style_def("tm", "tol", "ang", "tm", [tol_is](const Value& e) { return is_ang(e) && tol_is(e, {"dev", "lim"}); }));
  {
    Def d = style_def("tdec", "tol", "int", "tdec",
                      [tol_is](const Value& e) { return has_kind(e) && !tol_is(e, {"none", "basic"}); });
    d.min = 0;
    d.max = 8;
    out.push_back(std::move(d));
  }
  {
    Def d = style_def("tfac", "tol", "num", "tfac",
                      [tol_is](const Value& e) { return has_kind(e) && tol_is(e, {"dev", "lim"}); });
    d.gt0 = true;
    d.max = 2;
    out.push_back(std::move(d));
  }
  {
    Def d = style_def(
        "tolj", "tol", "choice", "tolj", [tol_is](const Value& e) { return has_kind(e) && tol_is(e, {"dev", "lim"}); },
        true);
    d.choices = [](const Ctx& c) { return keys_of(c, "props.tolj.", {0, 1, 2}); };
    out.push_back(std::move(d));
  }
  return out;
}

std::vector<Def> type_defs(const std::string& type) {
  if (type == "line") {
    return line_defs();
  }
  if (type == "circle") {
    return circle_defs();
  }
  if (type == "arc") {
    return arc_defs();
  }
  if (type == "ellipse") {
    return ellipse_defs();
  }
  if (type == "point") {
    return point("pos", "x", "y", "z");
  }
  if (type == "poly") {
    return poly_defs();
  }
  if (type == "text") {
    return text_defs();
  }
  if (type == "insert") {
    return insert_defs();
  }
  if (type == "hatch") {
    return hatch_defs();
  }
  if (type == "mleader") {
    return mleader_defs();
  }
  if (type == "viewport") {
    return viewport_defs();
  }
  if (type == "dim") {
    return dim_defs();
  }
  return {};
}

int section_order(const std::string& s) {
  for (std::size_t i = 0; i < kSections.size(); ++i) {
    if (kSections[i] == s) {
      return static_cast<int>(i);
    }
  }
  return static_cast<int>(kSections.size()) + 1;
}

/// near(a, b): numbers within 1e-9 (relative), else the same value (colours '#…' in any case)
bool near(const Value& a, const Value& b) {
  if (a.is_number() && b.is_number()) {
    const double x = a.as_number();
    const double y = b.as_number();
    return std::abs(x - y) <= 1e-9 * std::max({1.0, std::abs(x), std::abs(y)});
  }
  if (a.is_string() && b.is_string() && !a.as_string().empty() && a.as_string()[0] == '#') {
    return base::js_to_lower(a.as_string()) == base::js_to_lower(b.as_string());
  }
  if (a.is_object() || a.is_array() || a.is_point()) {
    return false;  // a === b of two objects read separately
  }
  return a == b && a.is_string() == b.is_string() && a.is_number() == b.is_number();
}

}  // namespace

std::vector<Group> groups_of(const std::vector<Value>& ents, const i18n::Catalog& words) {
  std::vector<Group> groups;
  for (const Value& e : ents) {
    const std::string t = type_of(e);
    auto it = std::ranges::find_if(groups, [&t](const Group& g) { return g.key == t; });
    if (it == groups.end()) {
      static constexpr std::array<const char*, 14> kTypes{"line",     "circle",  "arc",    "poly",  "ellipse",
                                                          "text",     "point",   "insert", "hatch", "dim",
                                                          "viewport", "mleader", "xline",  "ray"};
      const bool named = std::ranges::any_of(kTypes, [&t](const char* k) { return t == k; });
      groups.push_back({.key = t, .n = 0, .ids = {}, .label = named ? words.t("type." + t) : t});
      it = groups.end() - 1;
    }
    ++it->n;
    it->ids.push_back(static_cast<doc::Id>(base::js_to_number(e.get("id"))));
  }
  if (groups.size() > 1) {
    Group all{.key = "all", .n = ents.size(), .ids = {}, .label = words.t("props.group.all")};
    for (const Value& e : ents) {
      all.ids.push_back(static_cast<doc::Id>(base::js_to_number(e.get("id"))));
    }
    groups.insert(groups.begin(), std::move(all));
  }
  return groups;
}

std::vector<Def> defs_for(const std::string& key, const Ctx& ctx) {
  std::vector<std::string> types;
  for (const Value& e : ctx.ents) {
    const std::string t = type_of(e);
    if (std::ranges::find(types, t) == types.end()) {
      types.push_back(t);
    }
  }
  std::vector<Def> list;
  for (Def& d : general()) {
    if (d.types.empty() || std::ranges::all_of(types, [&d](const std::string& t) {
          return std::ranges::find(d.types, t) != d.types.end();
        })) {
      list.push_back(std::move(d));
    }
  }
  if (key != "all" && types.size() == 1) {
    for (Def& d : type_defs(key)) {
      list.push_back(std::move(d));
    }
  }
  std::vector<std::pair<Def, std::size_t>> kept;
  for (std::size_t i = 0; i < list.size(); ++i) {
    Def& d = list[i];
    if (d.one && ctx.ents.size() != 1) {
      continue;
    }
    if (d.when && !std::ranges::all_of(ctx.ents, [&d, &ctx](const Value& e) { return d.when(e, ctx); })) {
      continue;
    }
    d.label = ctx.words->t(d.label.empty() ? "props.f." + d.key : d.label);
    kept.emplace_back(std::move(d), i);
  }
  std::ranges::stable_sort(
      kept, [](const auto& a, const auto& b) { return section_order(a.first.sec) < section_order(b.first.sec); });
  std::vector<Def> out;
  out.reserve(kept.size());
  for (auto& [d, i] : kept) {
    out.push_back(std::move(d));
  }
  return out;
}

std::optional<Value> value_of(const Def& def, const Ctx& ctx) {
  std::optional<Value> first;
  for (std::size_t i = 0; i < ctx.ents.size(); ++i) {
    const Got v = def.get(ctx.ents[i], ctx);
    if (!v) {
      return std::nullopt;
    }
    if (!first) {
      first = *v;
    } else if (!near(*first, *v)) {
      return std::nullopt;
    }
  }
  return first ? first : Got(Value());
}

Applied apply_value(doc::Doc& doc, const Def& def, const Value& v, const Ctx& ctx) {
  Applied out;
  if (!def.set) {
    out.error = ctx.words->t("props.err.readonly");
    return out;
  }
  // every new object first: an error from any of them refuses the whole change (the JS rolls the change back)
  std::vector<std::pair<doc::Id, Value>> next;
  for (const Value& e : ctx.ents) {
    const auto id = static_cast<doc::Id>(base::js_to_number(e.get("id")));
    const Value* cur = doc.entities().get(id);
    if (cur == nullptr) {
      continue;
    }
    if (!doc.editable(*cur)) {
      ++out.locked;
      continue;
    }
    SetResult r = def.set(*cur, v, ctx);
    if (!r.error.empty()) {
      out.error = std::move(r.error);
      return out;
    }
    next.emplace_back(id, std::move(r.e));
  }
  doc.change("PROPERTIES", [&] {
    for (const auto& [id, e] : next) {
      (void)doc.update(id, e);
      ++out.changed;
    }
  });
  out.ok = true;
  return out;
}

Measure measure(const Value& e) {
  Measure m;
  if (const auto l = cad::curve_length(e)) {
    m.length = l;
  }
  const std::string t = type_of(e);
  if ((t == "circle" || t == "poly" || t == "ellipse" || t == "hatch") && (t != "poly" || e.get("closed").truthy())) {
    if (const auto a = cad::entity_area(e)) {
      m.area = std::abs(a->area);
      m.perimeter = a->perimeter;
    }
  }
  return m;
}

}  // namespace cc::cmd::props
