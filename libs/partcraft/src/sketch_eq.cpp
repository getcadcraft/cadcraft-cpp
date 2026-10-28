// SPDX-License-Identifier: Apache-2.0
// sketch.js equations(): every relation as residual functions over the solver's variable vector.
#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "sketch_js.hpp"

namespace cc::partcraft::sketch {

const std::vector<std::size_t>* Vars::find(std::string_view id) const {
  for (const auto& [k, v] : std::views::reverse(map)) {  // a later set of the same id wins (Map#set)
    if (k == id) {
      return &v;
    }
  }
  return nullptr;
}

Vars build_vars(const base::Value& sk, const std::vector<std::string>& pinned) {
  using detail::num;
  Vars v;
  const auto is_pinned = [&](const base::Value& id) {
    return id.is_string() && std::ranges::find(pinned, id.as_string()) != pinned.end();
  };
  const auto key = [](const base::Value& id) { return id.is_string() ? id.as_string() : base::js_string(id); };
  for (const base::Value& e : detail::entities_of(sk)) {
    if (e.get("type") == base::Value("point") && !is_pinned(e.get("id"))) {
      v.map.emplace_back(key(e.get("id")), std::vector<std::size_t>{v.x.size(), v.x.size() + 1});
      v.x.push_back(num(e.get("x")));
      v.x.push_back(num(e.get("y")));
    }
  }
  for (const base::Value& e : detail::entities_of(sk)) {
    const base::Value t = e.get("type");
    if (t == base::Value("circle") && !is_pinned(e.get("id"))) {
      v.map.emplace_back(key(e.get("id")), std::vector<std::size_t>{v.x.size()});
      v.x.push_back(num(e.get("r")));
    }
    if (t == base::Value("spline")) {
      for (const std::string_view k : {"a0", "a1"}) {
        const base::Value a = e.get(k);
        if (a.is_number() && std::isfinite(a.as_number())) {
          v.map.emplace_back(key(e.get("id")) + ":" + std::string(k), std::vector<std::size_t>{v.x.size()});
          v.x.push_back(a.as_number());
        }
      }
    }
  }
  return v;
}

namespace detail {
namespace {

constexpr double PI = std::numbers::pi;

double nz(double d) {  // `d || 1e-300`
  return d == 0 || std::isnan(d) ? 1e-300 : d;
}
double hyp(P2 v) {
  return base::js_hypot(v[0], v[1]);
}

// NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members): lives as long as the equations it serves
struct Ctx {
  const Value& sk;
  std::vector<double>& x;
  const Vars& vars;
  const i18n::Catalog& w;
  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  std::unordered_map<std::string, const Value*> ents;

  [[nodiscard]] const Value* ent(const Value& id) const {
    if (!id.is_string()) {
      return nullptr;
    }
    const auto it = ents.find(id.as_string());
    return it == ents.end() ? nullptr : it->second;
  }
  [[nodiscard]] const std::vector<std::size_t>* var(const Value& id) const {
    return id.is_string() ? vars.find(id.as_string()) : nullptr;
  }
  [[nodiscard]] P2 P(const Value& id) const {
    if (id == Value(kOrigin)) {
      return {0, 0};
    }
    if (const auto* v = var(id); v != nullptr) {
      if (v->size() > 1) {
        return {x.at((*v)[0]), x.at((*v)[1])};
      }
      if ((*v)[0] != 0) {  // a circle's radius index: `[x[v[0]], x[v[1]]]` of a number is undefined twice
        return {NaN, NaN};
      }
    }
    const Value* e = ent(id);
    if (e == nullptr) {
      type_error(Value{}, "x");
    }
    return {num(e->get("x")), num(e->get("y"))};
  }
  [[nodiscard]] std::vector<std::size_t> deps_p(const Value& id) const {
    const auto* v = var(id);
    return v != nullptr ? *v : std::vector<std::size_t>{};
  }
  [[nodiscard]] double R(const Value& e) const {
    if (e.get("type") == Value("circle")) {
      const auto* v = var(e.get("id"));
      return v != nullptr ? x.at(v->front()) : num(e.get("r"));
    }
    const P2 c = P(e.get("c"));
    const P2 s = P(e.get("s"));
    return base::js_hypot(s[0] - c[0], s[1] - c[1]);
  }
  [[nodiscard]] std::vector<std::size_t> deps_e(const Value& id) const {
    const Value* e = ent(id);
    if (e == nullptr || id == Value(kOrigin)) {
      return deps_p(id);
    }
    if (e->get("type") == Value("point")) {
      return deps_p(id);
    }
    std::vector<std::size_t> d;
    for (const Value& p : points_of(*e)) {
      const auto dp = deps_p(p);
      d.insert(d.end(), dp.begin(), dp.end());
    }
    const std::string eid = base::js_string(e->get("id"));
    if (e->get("type") == Value("circle")) {
      if (const auto* v = vars.find(eid)) {
        d.push_back(v->front());
      }
    }
    if (e->get("type") == Value("spline")) {
      for (const std::string_view k : {":a0", ":a1"}) {
        if (const auto* v = vars.find(eid + std::string(k))) {
          d.push_back(v->front());
        }
      }
    }
    return d;
  }
  [[nodiscard]] Value aget(const Value& e, std::string_view key) const {
    if (const auto* v = vars.find(base::js_string(e.get("id")) + ":" + std::string(key))) {
      return x.at(v->front());
    }
    return e.get(key);
  }
  [[nodiscard]] P2 tan(const Value& e, char end) const {
    return spline_end_tangent(
        sk, e, end, [this](const Value& id) { return std::optional<P2>(P(id)); },
        [this](const Value& ee, std::string_view k) { return aget(ee, k); }, w);
  }
  [[nodiscard]] std::array<P2, 2> L(const Value& id) const {
    const Value* l = ent(id);
    if (l == nullptr) {
      type_error(Value{}, "p1");
    }
    return {P(l->get("p1")), P(l->get("p2"))};
  }
  [[nodiscard]] P2 dir(const Value& id) const {
    const auto [a, b] = L(id);
    return {b[0] - a[0], b[1] - a[1]};
  }
  [[nodiscard]] double sd(P2 p, const Value& id) const { return signed_dist(p, L(id)); }
  [[nodiscard]] const Value& E(const Value& id) const {
    const Value* e = ent(id);
    if (e == nullptr) {
      static const Value kNull(nullptr);
      return kNull;
    }
    return *e;
  }
};

double cross_n(P2 u, P2 v) {
  return (u[0] * v[1] - u[1] * v[0]) / nz(hyp(u) * hyp(v));
}

}  // namespace

std::vector<Eq> equations(const Value& sk, std::vector<double>& x, const Vars& vars, const i18n::Catalog& w) {
  auto ctx = std::make_shared<Ctx>(Ctx{.sk = sk, .x = x, .vars = vars, .w = w, .ents = {}});
  for (const Value& e : entities_of(sk)) {
    const Value id = prop(e, "id");
    if (id.is_string()) {
      ctx->ents[id.as_string()] = &e;
    }
  }
  const Ctx& C = *ctx;
  std::vector<Eq> out;
  const auto push = [&](const std::string& cid, const std::vector<std::size_t>& deps, std::function<double()> f) {
    out.push_back({cid, std::move(f), deps});
  };
  for (const Value& e : entities_of(sk)) {
    if (!(e.get("type") == Value("arc"))) {
      continue;
    }
    const Value* ep = &e;
    push("arc:" + base::js_string(e.get("id")), C.deps_e(e.get("id")), [ctx, ep] {
      const P2 c = ctx->P(ep->get("c"));
      const P2 s = ctx->P(ep->get("s"));
      const P2 q = ctx->P(ep->get("e"));
      return base::js_hypot(q[0] - c[0], q[1] - c[1]) - base::js_hypot(s[0] - c[0], s[1] - c[1]);
    });
  }
  for (const Value& c : constraints_of(sk)) {
    const auto m = match(sk, prop(c, "type"), c.get("refs"));
    if (!m) {
      continue;
    }
    const std::vector<Value> r = c.get("refs").as_array();
    const std::string& sig = m->first;
    std::vector<std::size_t> deps;
    for (const Value& id : r) {
      for (const std::size_t d : C.deps_e(id)) {
        if (std::ranges::find(deps, d) == deps.end()) {
          deps.push_back(d);
        }
      }
    }
    const std::string cid = base::js_string(c.get("id"));
    const std::string t = base::js_string(c.get("type"));
    const Value* cp = &c;
    const auto sign = [cp] {
      const double s = num(cp->get("sign"));
      return s == 0 || std::isnan(s) ? 1.0 : s;
    };
    const auto value = [cp] { return num(cp->get("value")); };
    const auto at = [&r](std::size_t i) { return i < r.size() ? r[i] : Value{}; };
    const Value r0 = at(0);
    const Value r1 = at(1);
    const Value r2 = at(2);
    if (t == "coincident") {
      if (sig == "PP") {
        push(cid, deps, [ctx, r0, r1] { return ctx->P(r0)[0] - ctx->P(r1)[0]; });
        push(cid, deps, [ctx, r0, r1] { return ctx->P(r0)[1] - ctx->P(r1)[1]; });
      } else if (sig == "PL") {
        push(cid, deps, [ctx, r0, r1] { return ctx->sd(ctx->P(r0), r1); });
      } else {
        push(cid, deps, [ctx, r0, r1] {
          const P2 p = ctx->P(r0);
          const Value& e1 = ctx->E(r1);
          const P2 ce = ctx->P(prop(e1, "c"));
          return base::js_hypot(p[0] - ce[0], p[1] - ce[1]) - ctx->R(e1);
        });
      }
    } else if (t == "horizontal" || t == "vertical") {
      const std::size_t k = t == "horizontal" ? 1 : 0;
      if (sig == "L") {
        push(cid, deps, [ctx, r0, k] {
          const auto [a, b] = ctx->L(r0);
          return b[k] - a[k];
        });
      } else {
        push(cid, deps, [ctx, r0, r1, k] { return ctx->P(r1)[k] - ctx->P(r0)[k]; });
      }
    } else if (t == "parallel") {
      push(cid, deps, [ctx, r0, r1] { return cross_n(ctx->dir(r0), ctx->dir(r1)); });
    } else if (t == "perpendicular") {
      push(cid, deps, [ctx, r0, r1] {
        const P2 u = ctx->dir(r0);
        const P2 v = ctx->dir(r1);
        return (u[0] * v[0] + u[1] * v[1]) / nz(hyp(u) * hyp(v));
      });
    } else if (t == "tangent") {
      if (sig.contains('S')) {
        const Value& a = C.E(r0);
        const Value& b = C.E(r1);
        const auto meet = meet_of(sk, a, b);
        if (!meet) {
          continue;
        }
        const auto end_of = [](const Value& e, const Value& pid) {
          const Value pts = e.get("pts");
          return pts.is_array() && !pts.as_array().empty() && pid == pts.as_array()[0] ? 's' : 'e';
        };
        const Value* ap = &a;
        const Value* bp = &b;
        const char eb = end_of(b, (*meet)[1]);
        if (sig == "LS") {
          push(cid, deps, [ctx, r0, bp, eb] { return cross_n(ctx->dir(r0), ctx->tan(*bp, eb)); });
        } else if (sig == "SS") {
          const char ea = end_of(a, (*meet)[0]);
          push(cid, deps, [ctx, ap, bp, ea, eb] { return cross_n(ctx->tan(*ap, ea), ctx->tan(*bp, eb)); });
        } else {
          const Value pm = (*meet)[1];
          push(cid, deps, [ctx, ap, bp, eb, pm] {
            const P2 tv = ctx->tan(*bp, eb);
            const P2 ce = ctx->P(ap->get("c"));
            const P2 p = ctx->P(pm);
            return (tv[0] * (p[0] - ce[0]) + tv[1] * (p[1] - ce[1])) / nz(ctx->R(*ap));
          });
        }
        continue;
      }
      const auto j = shared_end(sk, C.E(r0), C.E(r1));
      if (j && sig == "LR") {
        const Value& jv = *j;
        push(cid, deps, [ctx, r0, r1, jv] {
          const P2 u = ctx->dir(r0);
          const Value& e1 = ctx->E(r1);
          const P2 ce = ctx->P(prop(e1, "c"));
          const P2 p = ctx->P(jv);
          return (u[0] * (p[0] - ce[0]) + u[1] * (p[1] - ce[1])) / nz(hyp(u) * ctx->R(e1));
        });
        continue;
      }
      if (j) {
        const Value& jv = *j;
        push(cid, deps, [ctx, r0, r1, jv] {
          const P2 p = ctx->P(jv);
          const Value& e0 = ctx->E(r0);
          const Value& e1 = ctx->E(r1);
          const P2 a = ctx->P(prop(e0, "c"));
          const P2 b = ctx->P(prop(e1, "c"));
          return ((p[0] - a[0]) * (p[1] - b[1]) - (p[1] - a[1]) * (p[0] - b[0])) / nz(ctx->R(e0) * ctx->R(e1));
        });
        continue;
      }
      if (sig == "LR") {
        push(cid, deps, [ctx, r0, r1, sign] {
          const Value& e1 = ctx->E(r1);
          return sign() * ctx->sd(ctx->P(prop(e1, "c")), r0) - ctx->R(e1);
        });
      } else {
        push(cid, deps, [ctx, r0, r1, sign, cp] {
          const Value& a = ctx->E(r0);
          const Value& b = ctx->E(r1);
          const P2 ca = ctx->P(prop(a, "c"));
          const P2 cb = ctx->P(prop(b, "c"));
          const double d = base::js_hypot(cb[0] - ca[0], cb[1] - ca[1]);
          return cp->get("kind") == Value("int") ? d - sign() * (ctx->R(a) - ctx->R(b)) : d - (ctx->R(a) + ctx->R(b));
        });
      }
    } else if (t == "equal") {
      if (sig == "LL") {
        push(cid, deps, [ctx, r0, r1] { return hyp(ctx->dir(r0)) - hyp(ctx->dir(r1)); });
      } else {
        push(cid, deps, [ctx, r0, r1] { return ctx->R(ctx->E(r0)) - ctx->R(ctx->E(r1)); });
      }
    } else if (t == "concentric") {
      const Value ca = sig == "PR" ? r0 : prop(C.E(r0), "c");
      const Value cb = prop(C.E(r1), "c");
      push(cid, deps, [ctx, ca, cb] { return ctx->P(ca)[0] - ctx->P(cb)[0]; });
      push(cid, deps, [ctx, ca, cb] { return ctx->P(ca)[1] - ctx->P(cb)[1]; });
    } else if (t == "midpoint") {
      push(cid, deps, [ctx, r0, r1] {
        const auto [a, b] = ctx->L(r1);
        return ctx->P(r0)[0] - (a[0] + b[0]) / 2;
      });
      push(cid, deps, [ctx, r0, r1] {
        const auto [a, b] = ctx->L(r1);
        return ctx->P(r0)[1] - (a[1] + b[1]) / 2;
      });
    } else if (t == "fix") {
      const Value* ep = C.ent(r0);
      const Value e = ep != nullptr ? *ep : ent_by_id(sk, r0);
      const Value vv = cp->get("value");
      const auto vk = [vv](std::size_t i) {
        return vv.truthy() && vv.is_array() && i < vv.as_array().size() ? num(vv.as_array()[i]) : NaN;
      };
      const std::vector<Value> pts = prop(e, "type") == Value("point") ? std::vector<Value>{e.get("id")} : points_of(e);
      for (std::size_t i = 0; i < pts.size(); i++) {
        const Value& pid = pts[i];
        const double vx = vk(2 * i);
        const double vy = vk(2 * i + 1);
        push(cid, deps, [ctx, pid, vx] { return ctx->P(pid)[0] - vx; });
        push(cid, deps, [ctx, pid, vy] { return ctx->P(pid)[1] - vy; });
      }
      if (e.get("type") == Value("circle")) {
        const double v2 = vk(2);
        push(cid, deps, [ctx, e, v2] { return ctx->R(e) - v2; });
      }
    } else if (t == "distance") {
      if (sig == "PP") {
        push(cid, deps, [ctx, r0, r1, value] {
          const P2 a = ctx->P(r0);
          const P2 b = ctx->P(r1);
          return base::js_hypot(b[0] - a[0], b[1] - a[1]) - value();
        });
      } else if (sig == "L") {
        push(cid, deps, [ctx, r0, value] { return hyp(ctx->dir(r0)) - value(); });
      } else if (sig == "PL") {
        push(cid, deps, [ctx, r0, r1, sign, value] { return sign() * ctx->sd(ctx->P(r0), r1) - value(); });
      } else {
        push(cid, deps, [ctx, r0, r1, sign, value] { return sign() * ctx->sd(ctx->L(r1)[0], r0) - value(); });
      }
    } else if (t == "hdistance" || t == "vdistance") {
      const std::size_t k = t == "hdistance" ? 0 : 1;
      const bool line = sig == "L";
      push(cid, deps, [ctx, r0, r1, k, line, sign, value] {
        std::array<P2, 2> ab{};
        if (line) {
          ab = ctx->L(r0);
        } else {
          ab = {ctx->P(r0), ctx->P(r1)};
        }
        return sign() * (ab[1][k] - ab[0][k]) - value();
      });
    } else if (t == "symmetric") {
      push(cid, deps, [ctx, r0, r1, r2] {
        const P2 a = ctx->P(r0);
        const P2 b = ctx->P(r1);
        return ctx->sd({(a[0] + b[0]) / 2, (a[1] + b[1]) / 2}, r2);
      });
      push(cid, deps, [ctx, r0, r1, r2] {
        const P2 a = ctx->P(r0);
        const P2 b = ctx->P(r1);
        const P2 u = ctx->dir(r2);
        return ((b[0] - a[0]) * u[0] + (b[1] - a[1]) * u[1]) / nz(hyp(u));
      });
    } else if (t == "offset" || t == "offsetlink") {
      const Value* master = nullptr;
      if (t == "offset") {
        master = cp;
      } else {
        for (const Value& k : constraints_of(sk)) {
          if (k.get("id") == cp->get("of") && k.get("type") == Value("offset")) {
            master = &k;
            break;
          }
        }
      }
      if (master == nullptr) {
        continue;
      }
      const auto mv = [master] { return num(master->get("value")); };
      if (sig == "LL") {
        push(cid, deps, [ctx, r0, r1] { return cross_n(ctx->dir(r0), ctx->dir(r1)); });
        push(cid, deps, [ctx, r0, r1, sign, mv] { return sign() * ctx->sd(ctx->L(r1)[0], r0) - mv(); });
      } else {
        const Value& a = C.E(r0);
        const Value& b = C.E(r1);
        const Value* ap = &a;
        const Value* bp = &b;
        push(cid, deps, [ctx, ap, bp] { return ctx->P(prop(*ap, "c"))[0] - ctx->P(prop(*bp, "c"))[0]; });
        push(cid, deps, [ctx, ap, bp] { return ctx->P(prop(*ap, "c"))[1] - ctx->P(prop(*bp, "c"))[1]; });
        push(cid, deps, [ctx, ap, bp, sign, mv] { return sign() * (ctx->R(*bp) - ctx->R(*ap)) - mv(); });
      }
    } else if (t == "radius") {
      push(cid, deps, [ctx, r0, value] { return ctx->R(ctx->E(r0)) - value(); });
    } else if (t == "diameter") {
      push(cid, deps, [ctx, r0, value] { return 2 * ctx->R(ctx->E(r0)) - value(); });
    } else if (t == "angle") {
      push(cid, deps, [ctx, r0, r1, sign, value] {
        const P2 u = ctx->dir(r0);
        const P2 v = ctx->dir(r1);
        double d = base::js_atan2(u[0] * v[1] - u[1] * v[0], u[0] * v[0] + u[1] * v[1]) - sign() * value() * PI / 180;
        while (d > PI) {
          d -= 2 * PI;
        }
        while (d <= -PI) {
          d += 2 * PI;
        }
        return d;
      });
    }
  }
  return out;
}

}  // namespace detail
}  // namespace cc::partcraft::sketch
