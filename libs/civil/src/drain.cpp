// SPDX-License-Identifier: Apache-2.0
// pipes.js and hydro.js (CivilCraft 0.5), ported function by function.
#include "cc/civil/drain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
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
constexpr int kMaxDepth = 10000;  // the JS's recursion runs out of stack about here: RangeError

struct StackOverflow {};

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
double or0(double v) {  // `x || 0` for a number
  return (std::isnan(v) || v == 0) ? 0 : v;
}
double jmax(double a, double b) {
  return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::max(a, b);
}
double jmin(double a, double b) {
  return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::min(a, b);
}
/// A fill value: String(v), or the fill's own "{name}" when v is null or undefined (i18n.js leaves it as written).
std::string fv(const Value& v, std::string_view name) {
  if (v.is_null() || v.is_undefined()) {
    return "{" + std::string(name) + "}";
  }
  return base::js_string(v);
}
std::string pct_txt(double g) {
  return base::js_to_fixed(g * 100, 2) + "%";
}
double per_unit(std::string_view units) {
  return units == "m" ? 1000 : 12;
}
Value problem(const Value& id, const char* kind, std::string msg) {
  return make_object({{"id", id}, {"kind", kind}, {"msg", std::move(msg)}});
}
template <class T, class F>
Value map_obj(const JsMap<T>& m, F f) {
  Value o = Value::object();
  for (const auto& [k, v] : m.entries()) {
    o.set(prop_key(k), f(v));
  }
  return o;
}

}  // namespace

Value default_network(std::string_view units, const i18n::Catalog& words) {
  return make_object({{"name", words.t("pipes.default")},
                      {"type", "storm"},
                      {"rules", units == "m" ? make_object({{"minSlope", 0.005}, {"minCover", 0.6}, {"minDrop", 0.03}})
                                             : make_object({{"minSlope", 0.005}, {"minCover", 2}, {"minDrop", 0.1}})},
                      {"structures", Array{}},
                      {"pipes", Array{}}});
}

Value analyze_network(const Value& net, const Surface* surface, std::string_view units, const i18n::Catalog& w) {
  Array problems;
  struct St {
    Value s;
    std::optional<double> rim;
    Value rim_from;
    Array ins, outs;
    std::optional<double> sump;
  };
  struct Gr {
    double t;
    std::optional<double> g;
    double crown;
  };
  struct Pp {
    Value p;
    double len;
    std::optional<double> slope, cover, cover_at;
    std::vector<Gr> ground;
  };
  JsMap<St> structs;
  JsMap<Pp> pipes;
  const double per = per_unit(units);
  auto D = [&](const Value& dia) { return or0(num(dia)) / per; };
  const Value R = get(net, "rules").truthy() ? get(net, "rules") : Value::object();
  const double min_slope = num(get(R, "minSlope"));
  const double min_cover = num(get(R, "minCover"));
  for (const Value& s : arr(get(net, "structures"))) {
    const std::optional<double> g =
        surface != nullptr ? surface->elevation_at(num(get(s, "x")), num(get(s, "y")), 1e-6) : std::nullopt;
    const Value rim_v = get(s, "rim");
    const std::optional<double> rim = fin(rim_v) ? std::optional(rim_v.as_number()) : g;
    Value rim_from = nullptr;
    if (fin(rim_v)) {
      rim_from = "set";
    } else if (g) {
      rim_from = "surface";
    }
    structs.set(get(s, "id"), St{s, rim, rim_from, {}, {}, {}});
    if (!rim) {
      problems.push_back(
          problem(get(s, "id"), "structure", w.t("pipes.p.offsurf", {{"name", fv(get(s, "name"), "name")}})));
    }
  }
  for (const Value& p : arr(get(net, "pipes"))) {
    St* a = structs.get(get(p, "from"));
    St* b = structs.get(get(p, "to"));
    const Value name = get(p, "name");
    if (a == nullptr || b == nullptr || strict_equal(get(p, "from"), get(p, "to"))) {
      problems.push_back(problem(get(p, "id"), "pipe", w.t("pipes.p.unconnected", {{"name", fv(name, "name")}})));
      continue;
    }
    a->outs.push_back(get(p, "id"));
    b->ins.push_back(get(p, "id"));
    const double ax = num(get(a->s, "x"));
    const double ay = num(get(a->s, "y"));
    const double bx = num(get(b->s, "x"));
    const double by = num(get(b->s, "y"));
    const double len = base::js_hypot(bx - ax, by - ay);
    pipes.set(get(p, "id"), Pp{p, len, std::nullopt, std::nullopt, std::nullopt, {}});
    Pp& info = *pipes.get(get(p, "id"));
    const Value up_v = get(p, "invUp");
    const Value dn_v = get(p, "invDn");
    if (!fin(up_v) || !fin(dn_v)) {
      problems.push_back(problem(get(p, "id"), "pipe", w.t("pipes.p.noinv", {{"name", fv(name, "name")}})));
      continue;
    }
    const double inv_up = up_v.as_number();
    const double inv_dn = dn_v.as_number();
    info.slope = len > 0 ? (inv_up - inv_dn) / len : 0;
    if (*info.slope <= 0) {
      problems.push_back(problem(get(p, "id"), "pipe",
                                 w.t(*info.slope < 0 ? "pipes.p.uphill" : "pipes.p.flat",
                                     {{"name", fv(name, "name")}, {"slope", pct_txt(*info.slope)}})));
    } else if (min_slope > 0 && *info.slope < min_slope - 1e-9) {
      problems.push_back(
          problem(get(p, "id"), "pipe",
                  w.t("pipes.p.flatter",
                      {{"name", fv(name, "name")}, {"slope", pct_txt(*info.slope)}, {"min", pct_txt(min_slope)}})));
    }
    const double nn = jmax(2, jmin(200, std::ceil(len / 2)));
    double min_c = kInf;
    std::optional<double> at;
    double off = 0;
    const double dd = D(get(p, "dia"));
    for (std::int64_t k = 0; static_cast<double>(k) <= nn; ++k) {
      const double t = static_cast<double>(k) / nn;
      const double x = ax + (bx - ax) * t;
      const double y = ay + (by - ay) * t;
      std::optional<double> g = surface != nullptr ? surface->elevation_at(x, y, 1e-6) : std::nullopt;
      if (k == 0 && a->rim_from.is_string() && a->rim_from.as_string() == "set") {
        g = a->rim;
      }
      if (static_cast<double>(k) == nn && b->rim_from.is_string() && b->rim_from.as_string() == "set") {
        g = b->rim;
      }
      const double crown = inv_up + (inv_dn - inv_up) * t + dd;
      info.ground.push_back({t, g, crown});
      if (!g) {
        off++;
        continue;
      }
      if (*g - crown < min_c) {
        min_c = *g - crown;
        at = t;
      }
    }
    if (std::isfinite(min_c)) {
      info.cover = min_c;
      info.cover_at = at;
    }
    if (off != 0) {
      problems.push_back(problem(get(p, "id"), "pipe", w.t("pipes.p.partoff", {{"name", fv(name, "name")}})));
    }
    if (std::isfinite(min_c) && min_cover > 0 && min_c < min_cover - 1e-9) {
      const std::string cov = base::js_to_fixed(min_c, 2);
      const std::string mn = fv(get(R, "minCover"), "min");
      std::string msg;
      const double atv = at.value_or(std::nan(""));
      if (atv <= 0) {
        msg = w.t("pipes.p.cover.up", {{"name", fv(name, "name")}, {"cover", cov}, {"min", mn}});
      } else if (atv >= 1) {
        msg = w.t("pipes.p.cover.down", {{"name", fv(name, "name")}, {"cover", cov}, {"min", mn}});
      } else {
        msg = w.t("pipes.p.cover.along", {{"name", fv(name, "name")},
                                          {"cover", cov},
                                          {"at", appkit::num_str(base::js_round(atv * len))},
                                          {"min", mn}});
      }
      problems.push_back(problem(get(p, "id"), "pipe", std::move(msg)));
    }
  }
  const Value min_drop_v = get(R, "minDrop");
  const double min_drop = or0(num(min_drop_v));
  for (auto& [id, st] : structs.entries()) {
    std::vector<double> inv;
    for (const Value& pid : st.ins) {
      const Pp* pp = pipes.get(pid);
      if (pp != nullptr && fin(get(pp->p, "invDn"))) {
        inv.push_back(get(pp->p, "invDn").as_number());
      }
    }
    for (const Value& pid : st.outs) {
      const Pp* pp = pipes.get(pid);
      if (pp != nullptr && fin(get(pp->p, "invUp"))) {
        inv.push_back(get(pp->p, "invUp").as_number());
      }
    }
    if (!inv.empty()) {
      double m = kInf;
      for (const double v : inv) {
        m = jmin(m, v);
      }
      st.sump = m - or0(num(get(st.s, "sumpDepth")));
    }
    const Value sname = get(st.s, "name");
    if (st.outs.size() > 1) {
      problems.push_back(problem(id, "structure",
                                 w.t("pipes.s.outs", {{"name", fv(sname, "name")},
                                                      {"n", appkit::num_str(static_cast<double>(st.outs.size()))}})));
    }
    for (const Value& o : st.outs) {
      const Pp* po = pipes.get(o);
      if (po == nullptr || !fin(get(po->p, "invUp"))) {
        continue;
      }
      const double ou = get(po->p, "invUp").as_number();
      for (const Value& i : st.ins) {
        const Pp* pi = pipes.get(i);
        if (pi == nullptr || !fin(get(pi->p, "invDn"))) {
          continue;
        }
        const double id_ = get(pi->p, "invDn").as_number();
        if (ou > id_ - min_drop + 1e-9) {
          const std::string out_inv = base::js_to_fixed(ou, 2);
          const std::string in_inv = base::js_to_fixed(id_, 2);
          if (ou > id_) {
            problems.push_back(problem(id, "structure",
                                       w.t("pipes.s.drop.above", {{"name", fv(sname, "name")},
                                                                  {"out", fv(get(po->p, "name"), "out")},
                                                                  {"outInv", out_inv},
                                                                  {"in", fv(get(pi->p, "name"), "in")},
                                                                  {"inInv", in_inv}})));
          } else {
            problems.push_back(problem(id, "structure",
                                       w.t("pipes.s.drop.less", {{"name", fv(sname, "name")},
                                                                 {"out", fv(get(po->p, "name"), "out")},
                                                                 {"outInv", out_inv},
                                                                 {"drop", fv(min_drop_v, "drop")},
                                                                 {"in", fv(get(pi->p, "name"), "in")},
                                                                 {"inInv", in_inv}})));
          }
        }
      }
    }
    if (st.rim) {
      Array all = st.ins;
      all.insert(all.end(), st.outs.begin(), st.outs.end());
      for (const Value& pid : all) {
        const Pp* pp = pipes.get(pid);
        if (pp == nullptr) {
          continue;
        }
        const bool is_in = std::ranges::any_of(st.ins, [&](const Value& x) { return strict_equal(x, pid); });
        const Value inv_v = get(pp->p, is_in ? "invDn" : "invUp");
        if (fin(inv_v) && inv_v.as_number() + D(get(pp->p, "dia")) > *st.rim + 1e-9) {
          problems.push_back(
              problem(id, "structure",
                      w.t("pipes.s.crown", {{"pipe", fv(get(pp->p, "name"), "pipe")}, {"name", fv(sname, "name")}})));
        }
      }
    }
  }
  // a loop: depth-first from every structure
  {
    JsMap<Array> out;
    for (const Value& p : arr(get(net, "pipes"))) {
      if (!out.has(get(p, "from"))) {
        out.set(get(p, "from"), Array{});
      }
      out.get(get(p, "from"))->push_back(get(p, "to"));
    }
    JsMap<int> state;
    // NOLINTNEXTLINE(misc-no-recursion) — depth-capped as the JS stack is
    std::function<bool(const Value&, int)> visit = [&](const Value& v, int depth) -> bool {
      if (depth > kMaxDepth) {
        throw StackOverflow{};
      }
      const int* s = state.get(v);
      if (s != nullptr && *s == 1) {
        return true;
      }
      if (s != nullptr && *s == 2) {
        return false;
      }
      state.set(v, 1);
      if (const Array* next = out.get(v); next != nullptr) {
        const Array copy = *next;
        for (const Value& x : copy) {
          if (visit(x, depth + 1)) {
            return true;
          }
        }
      }
      state.set(v, 2);
      return false;
    };
    bool cycle = false;
    try {
      for (const Value& s : arr(get(net, "structures"))) {
        if (visit(get(s, "id"), 0)) {
          cycle = true;
          break;
        }
      }
    } catch (const StackOverflow&) {
      throw std::runtime_error("Maximum call stack size exceeded");
    }
    if (cycle) {
      problems.push_back(problem(nullptr, "network", w.t("pipes.loop")));
    }
  }
  auto opt = [](const std::optional<double>& v) { return v ? Value(*v) : Value(nullptr); };
  return make_object({{"structs", map_obj(structs,
                                          [&](const St& s) {
                                            return make_object({{"rim", opt(s.rim)},
                                                                {"rimFrom", s.rim_from},
                                                                {"ins", s.ins},
                                                                {"outs", s.outs},
                                                                {"sump", opt(s.sump)}});
                                          })},
                      {"pipes", map_obj(pipes,
                                        [&](const Pp& p) {
                                          Array ground;
                                          for (const Gr& g : p.ground) {
                                            ground.push_back(
                                                make_object({{"t", g.t}, {"g", opt(g.g)}, {"crown", g.crown}}));
                                          }
                                          return make_object({{"len", p.len},
                                                              {"slope", opt(p.slope)},
                                                              {"cover", opt(p.cover)},
                                                              {"coverAt", opt(p.cover_at)},
                                                              {"ground", std::move(ground)}});
                                        })},
                      {"problems", std::move(problems)}});
}

std::optional<std::vector<Value>> flow_order(const Value& net) {
  const Array& ps = arr(get(net, "pipes"));
  JsMap<std::vector<std::size_t>> into;
  for (std::size_t i = 0; i < ps.size(); ++i) {
    if (!into.has(get(ps[i], "to"))) {
      into.set(get(ps[i], "to"), {});
    }
    into.get(get(ps[i], "to"))->push_back(i);
  }
  std::set<std::string> done;
  std::set<std::string> busy;
  std::vector<Value> order;
  // NOLINTNEXTLINE(misc-no-recursion) — depth-capped as the JS stack is
  std::function<bool(std::size_t, int)> visit = [&](std::size_t pi, int depth) -> bool {
    if (depth > kMaxDepth) {
      throw StackOverflow{};
    }
    const std::string id = map_key(get(ps[pi], "id"));
    if (done.contains(id)) {
      return true;
    }
    if (busy.contains(id)) {
      return false;
    }
    busy.insert(id);
    if (const auto* list = into.get(get(ps[pi], "from")); list != nullptr) {
      const std::vector<std::size_t> copy = *list;
      for (const std::size_t q : copy) {
        if (!visit(q, depth + 1)) {
          return false;
        }
      }
    }
    busy.erase(id);
    done.insert(id);
    order.push_back(ps[pi]);
    return true;
  };
  try {
    for (std::size_t i = 0; i < ps.size(); ++i) {
      if (!visit(i, 0)) {
        return std::nullopt;
      }
    }
  } catch (const StackOverflow&) {
    throw std::runtime_error("Maximum call stack size exceeded");
  }
  return order;
}

Value auto_inverts(const Value& net, const Surface* surface, std::string_view units,
                   const std::optional<std::vector<Value>>& only, const i18n::Catalog& w) {
  const auto order = flow_order(net);
  if (!order) {
    return make_object({{"pipes", get(net, "pipes")}, {"problems", Array{Value(w.t("pipes.loop.inv"))}}});
  }
  const Value R = get(net, "rules").truthy() ? get(net, "rules") : Value::object();
  const double c = or0(num(get(R, "minCover")));
  const double s = or0(num(get(R, "minSlope")));
  const double drop = or0(num(get(R, "minDrop")));
  const double per = per_unit(units);
  JsMap<Value> by_id;
  for (const Value& x : arr(get(net, "structures"))) {
    by_id.set(get(x, "id"), x);
  }
  auto ground = [&](double x, double y, const Value* st) -> std::optional<double> {
    if (st != nullptr && fin(get(*st, "rim"))) {
      return get(*st, "rim").as_number();
    }
    return surface != nullptr ? surface->elevation_at(x, y, 1e-6) : std::nullopt;
  };
  JsMap<Value> res;
  for (const Value& p : arr(get(net, "pipes"))) {
    res.set(get(p, "id"), p.is_object() ? p : Value::object());
  }
  std::set<std::string> only_set;
  if (only) {
    for (const Value& v : *only) {
      only_set.insert(map_key(v));
    }
  }
  auto round2 = [](double v) { return std::floor(v * 100 + 1e-7) / 100; };
  Array problems;
  for (const Value& p0 : *order) {
    Value* pp = res.get(get(p0, "id"));
    if (pp == nullptr) {
      continue;
    }
    Value& p = *pp;
    if (only && !only_set.contains(map_key(get(p, "id")))) {
      continue;
    }
    const Value* a = by_id.get(get(p, "from"));
    const Value* b = by_id.get(get(p, "to"));
    if (a == nullptr || b == nullptr) {
      continue;
    }
    const double dm = or0(num(get(p, "dia"))) / per;
    const double ax = num(get(*a, "x"));
    const double ay = num(get(*a, "y"));
    const double bx = num(get(*b, "x"));
    const double by = num(get(*b, "y"));
    const double len = base::js_hypot(bx - ax, by - ay);
    const auto ga = ground(ax, ay, a);
    std::vector<double> incoming;
    for (const Value& q : arr(get(net, "pipes"))) {
      if (strict_equal(get(q, "to"), get(p, "from"))) {
        const Value* r = res.get(get(q, "id"));
        const Value dn = r != nullptr ? get(*r, "invDn") : Value();
        if (fin(dn)) {
          incoming.push_back(dn.as_number());
        }
      }
    }
    std::optional<double> up;
    if (ga) {
      up = *ga - c - dm;
    }
    if (!incoming.empty()) {
      double m = kInf;
      for (const double v : incoming) {
        m = jmin(m, v);
      }
      const double v = m - drop;
      up = up ? jmin(*up, v) : v;
    }
    if (!up) {
      problems.emplace_back(
          w.t("pipes.noground", {{"name", fv(get(p, "name"), "name")}, {"at", fv(get(*a, "name"), "at")}}));
      continue;
    }
    double dn = *up - s * len;
    const double nn = jmax(2, jmin(200, std::ceil(len / 2)));
    for (std::int64_t k = 1; static_cast<double>(k) <= nn; ++k) {
      const double t = static_cast<double>(k) / nn;
      const double x = ax + (bx - ax) * t;
      const double y = ay + (by - ay) * t;
      const auto g = static_cast<double>(k) == nn ? ground(bx, by, b) : ground(x, y, nullptr);
      if (!g) {
        continue;
      }
      const double lim = *up + (*g - c - dm - *up) / t;
      dn = std::min(dn, lim);  // (lim < dn ? lim : dn, as the JS's if)
    }
    const double inv_up = round2(*up);
    double inv_dn = round2(jmin(dn, *up - s * len));
    if (inv_dn > inv_up - s * len + 1e-9) {
      inv_dn = round2(inv_up - s * len);
    }
    p.set("invUp", inv_up);
    p.set("invDn", inv_dn);
  }
  Array out;
  for (const Value& p : arr(get(net, "pipes"))) {
    const Value* r = res.get(get(p, "id"));
    out.push_back(r != nullptr ? *r : Value());
  }
  return make_object({{"pipes", std::move(out)}, {"problems", std::move(problems)}});
}

// ---------------------------------------------------------------------------------------------- hydro.js

Value hydro_defaults(std::string_view units) {
  if (units == "m") {
    return make_object({{"idf", make_object({{"a", 1700}, {"b", 12}, {"c", 0.8}})},
                        {"n", 0.013},
                        {"kj", 0.5},
                        {"tailwater", nullptr},
                        {"minTc", 10},
                        {"vAssumed", 1}});
  }
  return make_object({{"idf", make_object({{"a", 60}, {"b", 10}, {"c", 0.75}})},
                      {"n", 0.013},
                      {"kj", 0.5},
                      {"tailwater", nullptr},
                      {"minTc", 10},
                      {"vAssumed", 3}});
}

Value clean_hydro(const Value& o, std::string_view units) {
  Value d = hydro_defaults(units);
  if (!o.is_object() && !o.is_array()) {
    return d;
  }
  auto pos = [](const Value& v, const Value& dv, double max) {
    return fin(v) && v.as_number() > 0 && v.as_number() < max ? v : dv;
  };
  const Value idf_in = get(o, "idf");
  const Value di = d.get("idf");
  const Value b = get(idf_in, "b");
  const Value kj = get(o, "kj");
  const Value tw = get(o, "tailwater");
  return make_object(
      {{"idf", make_object({{"a", pos(get(idf_in, "a"), di.get("a"), 1e6)},
                            {"b", fin(b) && b.as_number() >= 0 && b.as_number() < 1000 ? b : di.get("b")},
                            {"c", pos(get(idf_in, "c"), di.get("c"), 5)}})},
       {"n", pos(get(o, "n"), d.get("n"), 1)},
       {"kj", fin(kj) && kj.as_number() >= 0 && kj.as_number() <= 10 ? kj : d.get("kj")},
       {"tailwater", fin(tw) && std::abs(tw.as_number()) < 1e6 ? tw : Value(nullptr)},
       {"minTc", pos(get(o, "minTc"), d.get("minTc"), 1000)},
       {"vAssumed", d.get("vAssumed")}});
}

Value clean_catchment(const Value& o) {
  if ((!o.is_object() && !o.is_array()) || !get(o, "pts").is_array()) {
    return nullptr;
  }
  Array pts;
  for (const Value& p : arr(get(o, "pts"))) {
    if (pts.size() >= 10000) {
      break;
    }
    const Value x = get(p, "x");
    const Value y = get(p, "y");
    if (p.truthy() && fin(x) && fin(y) && std::abs(x.as_number()) < 1e9 && std::abs(y.as_number()) < 1e9) {
      pts.push_back(base::pt2(x.as_number(), y.as_number()));
    }
  }
  if (pts.size() < 3) {
    return nullptr;
  }
  const std::string id = base::js_string(get(o, "id"));
  bool id_ok = id.size() >= 2 && id.size() <= 10 && id[0] == 'k';
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
  {  // .slice(0, 120) in UTF-16 units
    const auto u16 = base::to_utf16(name);
    if (u16.size() > 120) {
      std::string cut;
      base::append_utf16(cut, std::span(u16).first(120));
      name = cut;
    }
  }
  const Value C = get(o, "C");
  const Value tc = get(o, "tc");
  const Value to = get(o, "to");
  return make_object({{"id", id_ok ? Value(id) : Value(nullptr)},
                      {"name", name.empty() ? std::string("Catchment") : name},
                      {"pts", std::move(pts)},
                      {"C", fin(C) && C.as_number() >= 0 && C.as_number() <= 1 ? C : Value(0.5)},
                      {"tc", fin(tc) && tc.as_number() > 0 && tc.as_number() < 1000 ? tc : Value(10)},
                      {"to", to.is_string() && base::utf16_length(to.as_string()) < 40 ? to : Value(nullptr)}});
}

double catchment_area(const Value& pts_v, std::string_view units) {
  const Array& pts = arr(pts_v);
  double a = 0;
  const std::size_t n = pts.size();
  for (std::size_t i = 0, j = n == 0 ? 0 : n - 1; i < n; j = i++) {
    a += (num(get(pts[j], "x")) + num(get(pts[i], "x"))) * (num(get(pts[j], "y")) - num(get(pts[i], "y")));
  }
  a = std::abs(a) / 2;
  return units == "m" ? a / 10000 : a / 43560;
}

double idf(double tc, const Value& h) {
  const Value i = get(h, "idf");
  return num(get(i, "a")) / base::js_pow(tc + num(get(i, "b")), num(get(i, "c")));
}

namespace {
double kk(std::string_view units) {
  return units == "m" ? 1 : 1.486;
}
double gg(std::string_view units) {
  return units == "m" ? 9.81 : 32.174;
}
double dia_of(const Value& p, std::string_view units) {
  return or0(num(get(p, "dia"))) / per_unit(units);
}
double q_at(double y, double D, double S, double n, std::string_view units) {
  const PartFull pf = part_full(y, D);
  return pf.P > 0 ? (kk(units) / n) * pf.A * base::js_pow(pf.A / pf.P, 2.0 / 3) * std::sqrt(S) : 0;
}
}  // namespace

Manning manning_full(double D, double S, double n, std::string_view units) {
  if (!(D > 0) || !(S > 0) || !(n > 0)) {
    return {0, 0, (std::numbers::pi * D * D) / 4};
  }
  const double A = (std::numbers::pi * D * D) / 4;
  const double R = D / 4;
  const double Q = (kk(units) / n) * A * base::js_pow(R, 2.0 / 3) * std::sqrt(S);
  return {Q, Q / A, A};
}

PartFull part_full(double y, double D) {
  const double yy = jmax(0, jmin(D, y));
  const double th = 2 * base::js_acos(jmax(-1, jmin(1, 1 - (2 * yy) / D)));
  return {(D * D / 8) * (th - base::js_sin(th)), (D * th) / 2, D * base::js_sin(th / 2)};
}

std::optional<double> normal_depth(double Q, double D, double S, double n, std::string_view units) {
  if (!(Q > 0)) {
    return 0;
  }
  if (!(S > 0)) {
    return std::nullopt;
  }
  double hi = D;
  double best = 0;
  for (int k = 0; k <= 200; ++k) {
    const double y = (D * k) / 200;
    const double q = q_at(y, D, S, n, units);
    if (q > best) {
      best = q;
      hi = y;
    }
  }
  if (Q > best) {
    return std::nullopt;
  }
  double lo = 0;
  for (int it = 0; it < 80; ++it) {
    const double m = (lo + hi) / 2;
    if (q_at(m, D, S, n, units) < Q) {
      lo = m;
    } else {
      hi = m;
    }
  }
  return (lo + hi) / 2;
}

namespace {
/// hydro.js order(): pipes in an order where each comes after the ones into its upper structure; nullopt on a loop.
std::optional<std::vector<Value>> hydro_order(const Value& net) {
  const Array& ps = arr(get(net, "pipes"));
  JsMap<std::vector<std::size_t>> ins;
  for (std::size_t i = 0; i < ps.size(); ++i) {
    if (!ins.has(get(ps[i], "to"))) {
      ins.set(get(ps[i], "to"), {});
    }
    ins.get(get(ps[i], "to"))->push_back(i);
  }
  std::set<std::string> done;
  std::vector<Value> out;
  while (out.size() < ps.size()) {
    bool moved = false;
    for (const Value& p : ps) {
      const std::string id = map_key(get(p, "id"));
      if (done.contains(id)) {
        continue;
      }
      const auto* list = ins.get(get(p, "from"));
      const bool ready = list == nullptr || std::ranges::all_of(*list, [&](std::size_t q) {
                           return done.contains(map_key(get(ps[q], "id")));
                         });
      if (ready) {
        done.insert(id);
        out.push_back(p);
        moved = true;
      }
    }
    if (!moved) {
      return std::nullopt;
    }
  }
  return out;
}
}  // namespace

std::shared_ptr<Flows> rational_flows(const Value& net, std::string_view units, const i18n::Catalog& w) {
  auto f = std::make_shared<Flows>();
  f->hydro = clean_hydro(get(net, "hydro"), units);
  const Value& h = f->hydro;
  Array problems;
  JsMap<Value> by_id;
  for (const Value& s : arr(get(net, "structures"))) {
    f->structs.set(get(s, "id"), StructFlow{});
    by_id.set(get(s, "id"), s);
  }
  for (const Value& c : arr(get(net, "catchments"))) {
    StructFlow* s = f->structs.get(get(c, "to"));
    if (s == nullptr) {
      problems.push_back(
          problem(get(c, "id"), "catchment", w.t("hy.no.outlet", {{"name", fv(get(c, "name"), "name")}})));
      continue;
    }
    s->CA += num(get(c, "C")) * catchment_area(get(c, "pts"), units);
    s->tc = jmax(s->tc, num(get(c, "tc")));
  }
  const auto ord = hydro_order(net.truthy() ? net : make_object({{"pipes", Array{}}}));
  if (!ord) {
    problems.push_back(problem(nullptr, "network", w.t("hy.loop")));
    f->problems = std::move(problems);
    return f;
  }
  const double min_tc = num(get(h, "minTc"));
  const double hn = num(get(h, "n"));
  const double v_assumed = num(get(h, "vAssumed"));
  for (const Value& p : *ord) {
    const StructFlow* up = f->structs.get(get(p, "from"));
    StructFlow* dn = f->structs.get(get(p, "to"));
    if (up == nullptr || dn == nullptr) {
      continue;
    }
    const double tc = jmax(up->tc, min_tc);
    const double i = idf(tc, h);
    const double Q = units == "m" ? (up->CA * i) / 360 : up->CA * i;
    const Value* a = by_id.get(get(p, "from"));
    const Value* b = by_id.get(get(p, "to"));
    const double len = base::js_hypot(num(get(*b, "x")) - num(get(*a, "x")), num(get(*b, "y")) - num(get(*a, "y")));
    const Value iu = get(p, "invUp");
    const Value id = get(p, "invDn");
    const double S = fin(iu) && fin(id) && len > 0 ? (iu.as_number() - id.as_number()) / len : 0;
    const double v = S > 0 ? manning_full(dia_of(p, units), S, hn, units).V : 0;
    const double travel = len / jmax(v > 0 ? v : v_assumed, 1e-6) / 60;
    f->pipes.set(get(p, "id"), PipeFlow{up->CA, tc, i, Q, travel, len, S});
    // (up and dn may be the same entry: read up's values first, as the JS does)
    const double up_ca = up->CA;
    dn->CA += up_ca;
    dn->tc = jmax(dn->tc, tc + travel);
  }
  f->problems = std::move(problems);
  return f;
}

Value flows_value(const Flows& f) {
  return make_object(
      {{"structs", map_obj(f.structs, [](const StructFlow& s) { return make_object({{"CA", s.CA}, {"tc", s.tc}}); })},
       {"pipes", map_obj(f.pipes,
                         [](const PipeFlow& p) {
                           return make_object({{"CA", p.CA},
                                               {"tc", p.tc},
                                               {"i", p.i},
                                               {"Q", p.Q},
                                               {"travel", p.travel},
                                               {"len", p.len},
                                               {"S", p.S}});
                         })},
       {"problems", f.problems},
       {"hydro", f.hydro}});
}

Value size_pipes(const Value& net, const Flows& flows, std::string_view units, const Value& sizes_in) {
  const double per = per_unit(units);
  const Value h = flows.hydro.truthy() ? flows.hydro : clean_hydro(get(net, "hydro"), units);
  std::vector<double> sizes;
  if (sizes_in.is_undefined()) {
    sizes = units == "m"
                ? std::vector<double>{300, 375, 450, 525, 600, 675, 750, 900, 1050, 1200, 1350, 1500, 1650, 1800}
                : std::vector<double>{12, 15, 18, 21, 24, 27, 30, 36, 42, 48, 54, 60, 66, 72};
  } else {
    for (const Value& v : arr(sizes_in)) {
      sizes.push_back(num(v));
    }
  }
  const double hn = num(get(h, "n"));
  Array out;
  for (const Value& p : arr(get(net, "pipes"))) {
    const PipeFlow* f = flows.pipes.get(get(p, "id"));
    if (f == nullptr) {
      continue;
    }
    if (!(f->S > 0)) {
      out.push_back(make_object(
          {{"id", get(p, "id")}, {"dia", get(p, "dia")}, {"Q", f->Q}, {"cap", 0}, {"ok", false}, {"flat", true}}));
      continue;
    }
    std::optional<std::pair<double, double>> pick;
    bool none = false;
    for (const double d : sizes) {
      const double cap = manning_full(d / per, f->S, hn, units).Q;
      if (cap >= f->Q) {
        pick = std::pair{d, cap};
        break;
      }
    }
    if (!pick) {
      const double d = sizes.empty() ? std::nan("") : sizes.back();
      pick = std::pair{d, manning_full(d / per, f->S, hn, units).Q};
      none = true;
    }
    Value dia = pick->first;
    if (sizes.empty()) {
      dia = Value();  // sizes[-1] is undefined
    }
    out.push_back(make_object({{"id", get(p, "id")}, {"dia", dia}, {"Q", f->Q}, {"cap", pick->second}, {"ok", !none}}));
  }
  return out;
}

Value hydraulic_grade(const Value& net, const Flows& flows, std::string_view units, const Surface* surface,
                      const i18n::Catalog& w) {
  const Value h = flows.hydro.truthy() ? flows.hydro : clean_hydro(get(net, "hydro"), units);
  const double g = gg(units);
  Array problems;
  struct PG {
    double hgl_up, hgl_dn, egl_up, egl_dn;
    std::optional<double> yn;
    bool full;
    double V, Sf;
  };
  struct SG {
    double hgl, egl;
    std::optional<double> rim;
  };
  JsMap<PG> pipes;
  JsMap<SG> structs;
  auto opt = [](const std::optional<double>& v) { return v ? Value(*v) : Value(nullptr); };
  auto view = [&] {
    return make_object({{"pipes", map_obj(pipes,
                                          [&](const PG& p) {
                                            return make_object({{"hglUp", p.hgl_up},
                                                                {"hglDn", p.hgl_dn},
                                                                {"eglUp", p.egl_up},
                                                                {"eglDn", p.egl_dn},
                                                                {"yn", opt(p.yn)},
                                                                {"full", p.full},
                                                                {"V", p.V},
                                                                {"Sf", p.Sf}});
                                          })},
                        {"structs", map_obj(structs,
                                            [&](const SG& s) {
                                              return make_object({{"hgl", s.hgl}, {"egl", s.egl}, {"rim", opt(s.rim)}});
                                            })},
                        {"problems", problems}});
  };
  const auto ord = hydro_order(net);
  if (!ord) {
    problems.push_back(problem(nullptr, "network", w.t("hy.loop")));
    return view();
  }
  JsMap<Value> s_by_id;
  for (const Value& s : arr(get(net, "structures"))) {
    s_by_id.set(get(s, "id"), s);
  }
  auto rim_of = [&](const Value* s) -> std::optional<double> {
    if (s == nullptr) {
      return std::nullopt;  // the JS reads s.rim of undefined and throws; never from a sound network
    }
    if (fin(get(*s, "rim"))) {
      return get(*s, "rim").as_number();
    }
    return surface != nullptr ? surface->elevation_at(num(get(*s, "x")), num(get(*s, "y")), 1e-6) : std::nullopt;
  };
  const double hn = num(get(h, "n"));
  const double kj = num(get(h, "kj"));
  const Value tw = get(h, "tailwater");
  for (std::size_t q = ord->size(); q-- > 0;) {
    const Value& p = (*ord)[q];
    const PipeFlow* f = flows.pipes.get(get(p, "id"));
    if (f == nullptr || !fin(get(p, "invUp")) || !fin(get(p, "invDn"))) {
      continue;
    }
    const double inv_up = get(p, "invUp").as_number();
    const double inv_dn = get(p, "invDn").as_number();
    const double D = dia_of(p, units);
    const Manning full = manning_full(D, f->S, hn, units);
    const SG* below = structs.get(get(p, "to"));
    const bool outlet = !std::ranges::any_of(
        arr(get(net, "pipes")), [&](const Value& x) { return strict_equal(get(x, "from"), get(p, "to")); });
    const std::optional<double> yn = f->S > 0 ? normal_depth(f->Q, D, f->S, hn, units) : std::nullopt;
    const bool surch = !yn;
    const double tail = (tw.is_null() || tw.is_undefined()) ? -kInf : num(tw);
    double down_hgl = tail;  // the tailwater at an outlet, else the structure below's HGL
    if (!outlet) {
      down_hgl = below != nullptr ? below->hgl : -kInf;
    }
    const double start_dn = jmax(inv_dn + (surch ? D : yn.value_or(0)), down_hgl);
    const double A = full.A;
    const double R = D / 4;
    const double Sf = A > 0 ? base::js_pow((f->Q * hn) / (kk(units) * A * base::js_pow(R, 2.0 / 3)), 2) : 0;
    const bool full_flow = surch || start_dn > inv_dn + D + 1e-9;
    const double hgl_dn = start_dn;
    const double hgl_up = full_flow ? jmax(hgl_dn + Sf * f->len, inv_up + D) : inv_up + *yn;
    const double a_flow = full_flow ? A : part_full(*yn, D).A;
    const double V = a_flow > 0 ? f->Q / a_flow : 0;
    const double hv = (V * V) / (2 * g);
    pipes.set(get(p, "id"), PG{hgl_up, hgl_dn, hgl_up + hv, hgl_dn + hv, yn, full_flow, V, Sf});
    if (full_flow) {
      problems.push_back(problem(get(p, "id"), "pipe", w.t("hy.full", {{"name", fv(get(p, "name"), "name")}})));
    }
    const double sh = hgl_up + kj * hv;
    const SG* prev = structs.get(get(p, "from"));
    if (prev == nullptr || sh > prev->hgl) {
      structs.set(get(p, "from"), SG{sh, sh + hv, rim_of(s_by_id.get(get(p, "from")))});
    }
    if (!structs.has(get(p, "to")) && outlet) {
      structs.set(get(p, "to"), SG{hgl_dn, hgl_dn + hv, rim_of(s_by_id.get(get(p, "to")))});
    }
  }
  for (const auto& [id, s] : structs.entries()) {
    if (s.rim && std::isfinite(*s.rim) && s.hgl > *s.rim + 1e-9) {
      const Value* st = s_by_id.get(id);
      const Value nm = st != nullptr ? get(*st, "name") : Value();
      problems.push_back(problem(id, "structure",
                                 w.t("hy.flood", {{"name", nm.truthy() ? base::js_string(nm) : fv(id, "name")},
                                                  {"by", base::js_to_fixed(s.hgl - *s.rim, 2)}})));
    }
  }
  return view();
}

}  // namespace cc::civil
