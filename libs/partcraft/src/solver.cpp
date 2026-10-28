// SPDX-License-Identifier: Apache-2.0
// solver.js (PartCraft 0.5): damped least-norm Newton over the sketch's equations, and the rank analysis.
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "jsx.hpp"
#include "sketch_js.hpp"

namespace cc::partcraft::sketch {

using namespace detail;  // NOLINT(google-build-using-namespace): the module's own helpers

namespace {

/// A sparse Jacobian row: (variable, ∂f/∂x) in the order first met (a Map).
using Row = std::vector<std::pair<std::size_t, double>>;

const double* row_get(const Row& r, std::size_t k) {
  for (const auto& [i, v] : r) {
    if (i == k) {
      return &v;
    }
  }
  return nullptr;
}

std::vector<double> residuals_of(const std::vector<Eq>& eqs) {
  std::vector<double> r;
  r.reserve(eqs.size());
  for (const Eq& q : eqs) {
    r.push_back(q.f());
  }
  return r;
}
double norm2(const std::vector<double>& r) {
  double s = 0;
  for (const double v : r) {
    s += v * v;
  }
  return s;
}

std::vector<Row> jacobian(const std::vector<Eq>& eqs, std::vector<double>& x) {
  std::vector<Row> out;
  out.reserve(eqs.size());
  for (const Eq& q : eqs) {
    Row row;
    std::vector<std::size_t> seen;
    for (const std::size_t k : q.deps) {
      if (std::ranges::find(seen, k) != seen.end()) {
        continue;
      }
      seen.push_back(k);
      const double x0 = x.at(k);
      const double h = 1e-6 * jsx::max(1, std::abs(x0));
      x[k] = x0 + h;
      const double fp = q.f();
      x[k] = x0 - h;
      const double fm = q.f();
      x[k] = x0;
      const double d = (fp - fm) / (2 * h);
      if (d != 0) {
        row.emplace_back(k, d);
      }
    }
    out.push_back(std::move(row));
  }
  return out;
}

double sparse_dot(const Row* a, const Row* b) {
  if (a->size() > b->size()) {
    std::swap(a, b);
  }
  double s = 0;
  for (const auto& [k, v] : *a) {
    if (const double* w = row_get(*b, k)) {
      s += v * *w;
    }
  }
  return s;
}

std::optional<std::vector<double>> chol_solve(const std::vector<std::vector<double>>& A, const std::vector<double>& b) {
  const std::size_t n = b.size();
  std::vector<std::vector<double>> L = A;
  for (std::size_t j = 0; j < n; j++) {
    double s = L[j][j];
    for (std::size_t k = 0; k < j; k++) {
      s -= L[j][k] * L[j][k];
    }
    if (!(s > 0)) {
      return std::nullopt;
    }
    const double d = std::sqrt(s);
    L[j][j] = d;
    for (std::size_t i = j + 1; i < n; i++) {
      double t = L[i][j];
      for (std::size_t k = 0; k < j; k++) {
        t -= L[i][k] * L[j][k];
      }
      L[i][j] = t / d;
    }
  }
  std::vector<double> y(n);
  for (std::size_t i = 0; i < n; i++) {
    double s = b[i];
    for (std::size_t k = 0; k < i; k++) {
      s -= L[i][k] * y[k];
    }
    y[i] = s / L[i][i];
  }
  std::vector<double> z(n);
  for (std::size_t ii = n; ii-- > 0;) {
    double s = y[ii];
    for (std::size_t k = ii + 1; k < n; k++) {
      s -= L[k][ii] * z[k];
    }
    z[ii] = s / L[ii][ii];
  }
  return z;
}

struct Newton {
  bool ok = false;
  int it = 0;
};

bool all_within(const std::vector<double>& r, double tol) {
  return std::ranges::all_of(r, [tol](double v) { return std::abs(v) <= tol; });
}

Newton newton(const std::vector<Eq>& eqs, std::vector<double>& x, double tol, int max_iter) {
  std::vector<double> r = residuals_of(eqs);
  double f = norm2(r);
  double mu = 1e-12;
  int it = 0;
  for (; it < max_iter; it++) {
    if (all_within(r, tol)) {
      return {.ok = true, .it = it};
    }
    const std::vector<Row> J = jacobian(eqs, x);
    const std::size_t m = J.size();
    std::vector<std::vector<double>> A(m, std::vector<double>(m, 0));
    double scale = 0;
    for (std::size_t i = 0; i < m; i++) {
      for (std::size_t j = 0; j <= i; j++) {
        const double v = sparse_dot(&J[i], &J[j]);
        A[i][j] = v;
        if (j < i) {
          A[j][i] = v;
        }
      }
      scale = jsx::max(scale, A[i][i]);
    }
    bool improved = false;
    for (int tries = 0; tries < 12 && !improved; tries++) {
      std::vector<std::vector<double>> B = A;
      for (std::size_t i = 0; i < m; i++) {
        B[i][i] = A[i][i] + mu * jsx::max(scale, 1e-12) + 1e-300;
      }
      std::vector<double> neg(r.size());
      for (std::size_t i = 0; i < r.size(); i++) {
        neg[i] = -r[i];
      }
      const auto y = chol_solve(B, neg);
      if (!y) {
        mu *= 100;
        continue;
      }
      std::vector<double> dx(x.size(), 0);
      for (std::size_t i = 0; i < J.size(); i++) {
        for (const auto& [k, v] : J[i]) {
          dx[k] += v * (*y)[i];
        }
      }
      const std::vector<double> x0 = x;
      double alpha = 1;
      for (int ls = 0; ls < 20; ls++) {
        for (std::size_t k = 0; k < x.size(); k++) {
          x[k] = x0[k] + alpha * dx[k];
        }
        std::vector<double> r2 = residuals_of(eqs);
        const double f2 = norm2(r2);
        if (f2 < f) {
          r = std::move(r2);
          f = f2;
          improved = true;
          break;
        }
        alpha /= 2;
      }
      if (!improved) {
        x = x0;
        mu *= 100;
        if (mu > 1e6) {
          break;
        }
      } else {
        mu = jsx::max(1e-12, mu / 10);
      }
    }
    if (!improved) {
      return {.ok = all_within(r, tol), .it = it};
    }
  }
  return {.ok = all_within(r, tol), .it = it};
}

void write_back(Value& sk, const Vars& vars, const std::vector<double>& x) {
  for (Value& e : entities(sk)->as_array()) {
    const Value id = e.get("id");
    const auto* v = id.is_string() ? vars.find(id.as_string()) : nullptr;
    if (v == nullptr) {
      continue;
    }
    const Value t = e.get("type");
    if (t == Value("point")) {
      e.set("x", x.at((*v)[0]));
      e.set("y", v->size() > 1 ? x.at((*v)[1]) : NaN);
    } else if (t == Value("circle")) {
      e.set("r", x.at(v->front()));
    }
  }
  for (Value& e : entities(sk)->as_array()) {
    if (!(e.get("type") == Value("spline"))) {
      continue;
    }
    for (const std::string_view k : {"a0", "a1"}) {
      if (const auto* v = vars.find(base::js_string(e.get("id")) + ":" + std::string(k))) {
        const double a = x.at(v->front());
        e.set(k, base::js_atan2(base::js_sin(a), base::js_cos(a)));
      }
    }
  }
}

struct Analysis {
  double dof, rank, vars;
  std::vector<std::string> redundant, conflicts;
  std::vector<Value> determined;
};

Analysis analyze_impl(const Value& sk, std::optional<double> tol_scale, const i18n::Catalog& w) {
  Vars vars = build_vars(sk);
  std::vector<double>& x = vars.x;
  const std::vector<Eq> eqs = equations(sk, x, vars, w);
  const double tol = 1e-8 * (tol_scale ? *tol_scale : jsx::max(1, extent(sk)));
  const std::vector<Row> J = jacobian(eqs, x);
  const std::size_t n = x.size();
  std::vector<std::vector<double>> Q;
  std::vector<std::string> redundant;
  std::vector<std::string> conflicts;
  const auto add = [](std::vector<std::string>& set, const std::string& s) {
    if (std::ranges::find(set, s) == set.end()) {
      set.push_back(s);
    }
  };
  for (std::size_t i = 0; i < eqs.size(); i++) {
    const Eq& q = eqs[i];
    const double r = q.f();
    if (std::abs(r) > tol * 100) {
      add(conflicts, q.cid);
    }
    std::vector<double> v(n, 0);
    double vn = 0;
    for (const auto& [k, d] : J[i]) {
      v[k] = d;
      vn += d * d;
    }
    vn = std::sqrt(vn);
    if (vn < 1e-12) {
      add(redundant, q.cid);
      continue;
    }
    for (std::size_t k = 0; k < n; k++) {
      v[k] /= vn;
    }
    for (int pass = 0; pass < 2; pass++) {
      for (const auto& b : Q) {
        double d = 0;
        for (std::size_t k = 0; k < n; k++) {
          d += b[k] * v[k];
        }
        if (d != 0) {
          for (std::size_t k = 0; k < n; k++) {
            v[k] -= d * b[k];
          }
        }
      }
    }
    double wn = 0;
    for (std::size_t k = 0; k < n; k++) {
      wn += v[k] * v[k];
    }
    wn = std::sqrt(wn);
    if (wn < 1e-7) {
      add(redundant, q.cid);
      continue;
    }
    for (std::size_t k = 0; k < n; k++) {
      v[k] /= wn;
    }
    Q.push_back(std::move(v));
  }
  std::vector<double> fixed(n, 0);
  for (const auto& b : Q) {
    for (std::size_t k = 0; k < n; k++) {
      fixed[k] += b[k] * b[k];
    }
  }
  const auto det = [&](std::size_t k) { return k < n && fixed[k] > 1 - 1e-6; };
  std::vector<Value> determined{Value(kOrigin)};
  const auto pt_det = [&](const Value& id) {
    if (id == Value(kOrigin)) {
      return true;
    }
    const auto* v = id.is_string() ? vars.find(id.as_string()) : nullptr;
    return v != nullptr && std::ranges::all_of(*v, det);
  };
  for (const Value& e : entities_of(sk)) {
    bool ok = std::ranges::all_of(points_of(e), pt_det);
    const std::string id = base::js_string(e.get("id"));
    if (e.get("type") == Value("circle")) {
      const auto* v = vars.find(id);
      ok = ok && v != nullptr && det(v->front());
    }
    if (e.get("type") == Value("spline")) {
      for (const std::string_view k : {":a0", ":a1"}) {
        if (const auto* v = vars.find(id + std::string(k))) {
          ok = ok && det(v->front());
        }
      }
    }
    if (ok && std::ranges::find(determined, e.get("id")) == determined.end()) {
      determined.push_back(e.get("id"));
    }
  }
  const auto strip = [](const std::vector<std::string>& s) {
    std::vector<std::string> out;
    for (const std::string& c : s) {
      if (!c.starts_with("arc:")) {
        out.push_back(c);
      }
    }
    return out;
  };
  return {.dof = static_cast<double>(n) - static_cast<double>(Q.size()),
          .rank = static_cast<double>(Q.size()),
          .vars = static_cast<double>(n),
          .redundant = strip(redundant),
          .conflicts = strip(conflicts),
          .determined = std::move(determined)};
}

Value strings(const std::vector<std::string>& v) {
  base::Array a;
  for (const std::string& s : v) {
    a.emplace_back(s);
  }
  return a;
}

}  // namespace

base::Result<std::vector<std::pair<std::string, double>>> residuals(const Value& sk, const i18n::Catalog& w) {
  return guard([&] {
    Vars vars = build_vars(sk);
    const std::vector<Eq> eqs = equations(sk, vars.x, vars, w);
    std::vector<std::pair<std::string, double>> out;
    out.reserve(eqs.size());
    for (const Eq& q : eqs) {
      out.emplace_back(q.cid, q.f());
    }
    return out;
  });
}

base::Result<Value> analyze(const Value& sk, std::optional<double> tol_scale, const i18n::Catalog& w) {
  return guard([&] {
    const Analysis a = analyze_impl(sk, tol_scale, w);
    return base::make_object({{"dof", a.dof},
                              {"rank", a.rank},
                              {"vars", a.vars},
                              {"redundant", strings(a.redundant)},
                              {"conflicts", strings(a.conflicts)},
                              {"determined", base::Array(a.determined.begin(), a.determined.end())}});
  });
}

base::Result<Value> solve_sketch(Value& sk, const std::vector<std::pair<std::string, std::array<double, 2>>>& pin,
                                 const std::vector<std::pair<std::string, double>>& pin_radius,
                                 const i18n::Catalog& w) {
  return guard([&] {
    // what to put back when the relations cannot be met
    struct Snap {
      Value a, b;
    };
    std::vector<Snap> snap;
    for (const Value& e : entities_of(sk)) {
      const Value t = e.get("type");
      if (t == Value("point")) {
        snap.push_back({e.get("x"), e.get("y")});
      } else if (t == Value("circle")) {
        snap.push_back({e.get("r"), {}});
      } else if (t == Value("spline")) {
        snap.push_back({e.get("a0"), e.get("a1")});
      } else {
        snap.push_back({});
      }
    }
    const auto restore = [&] {
      auto& list = entities(sk)->as_array();
      for (std::size_t i = 0; i < list.size() && i < snap.size(); i++) {
        Value& e = list[i];
        const Value t = e.get("type");
        if (t == Value("point")) {
          e.set("x", snap[i].a);
          e.set("y", snap[i].b);
        } else if (t == Value("circle")) {
          e.set("r", snap[i].a);
        } else if (t == Value("spline")) {
          if (!snap[i].a.is_undefined()) {
            e.set("a0", snap[i].a);
          }
          if (!snap[i].b.is_undefined()) {
            e.set("a1", snap[i].b);
          }
        }
      }
    };
    const double tol_scale = jsx::max(1, extent(sk));
    const double tol = 1e-9 * tol_scale;
    Newton res;
    bool pinned_ok = false;
    if (!pin.empty() || !pin_radius.empty()) {
      std::vector<std::string> pinned;
      pinned.reserve(pin.size() + pin_radius.size());
      for (const auto& [k, v] : pin) {
        pinned.push_back(k);
      }
      for (const auto& [k, v] : pin_radius) {
        pinned.push_back(k);
      }
      for (Value& e : entities(sk)->as_array()) {
        const Value id = e.get("id");
        if (!id.is_string()) {
          continue;
        }
        for (const auto& [k, v] : pin) {
          if (k == id.as_string()) {
            e.set("x", v[0]);
            e.set("y", v[1]);
          }
        }
        for (const auto& [k, v] : pin_radius) {
          if (k == id.as_string()) {
            e.set("r", jsx::max(1e-9, v));
          }
        }
      }
      Vars vars = build_vars(sk, pinned);
      const std::vector<Eq> eqs = equations(sk, vars.x, vars, w);
      res = newton(eqs, vars.x, tol, 60);
      if (res.ok) {
        write_back(sk, vars, vars.x);
        pinned_ok = true;
      } else {
        restore();
      }
    }
    if (!pinned_ok) {
      Vars vars = build_vars(sk);
      const std::vector<Eq> eqs = equations(sk, vars.x, vars, w);
      res = newton(eqs, vars.x, tol, 100);
      if (res.ok) {
        write_back(sk, vars, vars.x);
      } else {
        restore();
      }
    }
    const Analysis a = analyze_impl(sk, tol_scale, w);
    const bool geometry = !entities_of(sk).empty();
    std::string status = "under";
    if (!geometry) {
      status = "empty";
    } else if (!a.conflicts.empty() || !a.redundant.empty()) {
      status = "over";
    } else if (a.dof == 0) {
      status = "full";
    }
    return base::make_object({{"ok", res.ok},
                              {"dragged", pinned_ok},
                              {"status", status},
                              {"dof", a.dof},
                              {"conflicts", strings(a.conflicts)},
                              {"redundant", strings(a.redundant)},
                              {"determined", base::Array(a.determined.begin(), a.determined.end())},
                              {"iterations", res.it}});
  });
}

}  // namespace cc::partcraft::sketch
