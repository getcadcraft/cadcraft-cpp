// SPDX-License-Identifier: Apache-2.0
// sketch-spline.js (PartCraft 0.5): cubic B-splines.
#include "cc/partcraft/spline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "jsx.hpp"

namespace cc::partcraft::spline {
namespace {

using base::Value;
constexpr int DEG = 3;
constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

/// What the JS throws (a TypeError on a missing pole, or one of its own Errors): caught at each public function.
struct Thrown : std::runtime_error {
  using std::runtime_error::runtime_error;
};

/// U[i]: undefined (NaN in arithmetic) past either end.
double kn(const std::vector<double>& U, std::int64_t i) {
  return i >= 0 && static_cast<std::size_t>(i) < U.size() ? U[static_cast<std::size_t>(i)] : NaN;
}
/// P[i]: a missing pole is the TypeError `q[0]` of undefined gives.
const P2& pole(const std::vector<P2>& P, std::int64_t i) {
  if (i < 0 || static_cast<std::size_t>(i) >= P.size()) {
    throw Thrown("Cannot read properties of undefined (reading '0')");
  }
  return P[static_cast<std::size_t>(i)];
}

template <class F>
auto guard(F&& f) -> base::Result<decltype(f())> {
  try {
    return std::forward<F>(f)();
  } catch (const Thrown& e) {
    return base::fail(base::Errc::invalid, e.what());
  }
}

std::int64_t find_span(const std::vector<double>& U, std::int64_t p, std::int64_t m, double u) {
  if (u >= kn(U, m)) {
    std::int64_t k = m - 1;
    while (k > p && kn(U, k) >= kn(U, k + 1)) {
      k--;
    }
    return k;
  }
  if (u <= kn(U, p)) {
    std::int64_t k = p;
    while (k < m - 1 && kn(U, k + 1) <= u) {
      k++;
    }
    return k;
  }
  std::int64_t lo = p;
  std::int64_t hi = m - 1;
  while (lo < hi) {
    const std::int64_t mid = (lo + hi + 1) >> 1;
    if (kn(U, mid) <= u) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return lo;
}

std::vector<double> basis_funs(std::int64_t k, double u, std::int64_t p, const std::vector<double>& U) {
  const auto n = static_cast<std::size_t>(std::max<std::int64_t>(p, 0));
  std::vector<double> N(n + 1, 0);
  std::vector<double> left(n + 1, 0);
  std::vector<double> right(n + 1, 0);
  N[0] = 1;
  for (std::size_t j = 1; j <= n; j++) {
    left[j] = u - kn(U, k + 1 - static_cast<std::int64_t>(j));
    right[j] = kn(U, k + static_cast<std::int64_t>(j)) - u;
    double saved = 0;
    for (std::size_t r = 0; r < j; r++) {
      const double den = right[r + 1] + left[j - r];
      const double t = den == 0 ? 0 : N[r] / den;
      N[r] = saved + right[r + 1] * t;
      saved = left[j - r] * t;
    }
    N[j] = saved;
  }
  return N;
}

std::vector<std::vector<double>> ders_basis_funs(std::int64_t k, double u, std::int64_t p64, std::int64_t n64,
                                                 const std::vector<double>& U) {
  const auto p = static_cast<std::size_t>(std::max<std::int64_t>(p64, 0));
  const auto n = static_cast<std::size_t>(std::max<std::int64_t>(n64, 0));
  std::vector<std::vector<double>> ndu(p + 1, std::vector<double>(p + 1, 0));
  std::vector<double> left(p + 1, 0);
  std::vector<double> right(p + 1, 0);
  ndu[0][0] = 1;
  for (std::size_t j = 1; j <= p; j++) {
    left[j] = u - kn(U, k + 1 - static_cast<std::int64_t>(j));
    right[j] = kn(U, k + static_cast<std::int64_t>(j)) - u;
    double saved = 0;
    for (std::size_t r = 0; r < j; r++) {
      ndu[j][r] = right[r + 1] + left[j - r];
      const double t = ndu[j][r] == 0 ? 0 : ndu[r][j - 1] / ndu[j][r];
      ndu[r][j] = saved + right[r + 1] * t;
      saved = left[j - r] * t;
    }
    ndu[j][j] = saved;
  }
  std::vector<std::vector<double>> ders(n + 1, std::vector<double>(p + 1, 0));
  for (std::size_t j = 0; j <= p; j++) {
    ders[0][j] = ndu[j][p];
  }
  const auto P = static_cast<std::int64_t>(p);
  for (std::int64_t r = 0; r <= P; r++) {
    std::size_t s1 = 0;
    std::size_t s2 = 1;
    std::array<std::vector<double>, 2> a{std::vector<double>(p + 1, 0), std::vector<double>(p + 1, 0)};
    a[0][0] = 1;
    for (std::int64_t kk = 1; std::cmp_less_equal(kk, n); kk++) {
      double d = 0;
      const std::int64_t rk = r - kk;
      const std::int64_t pk = P - kk;
      const auto at = [&](std::int64_t i, std::int64_t j) -> double {  // ndu[i][j], undefined past the table
        if (i < 0 || j < 0 || i > P || j > P) {
          return NaN;
        }
        return ndu[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
      };
      const auto A = [&](std::size_t s, std::int64_t j) -> double& { return a[s][static_cast<std::size_t>(j)]; };
      if (r >= kk) {
        A(s2, 0) = at(pk + 1, rk) == 0 ? 0 : A(s1, 0) / at(pk + 1, rk);
        d = A(s2, 0) * at(rk, pk);
      }
      const std::int64_t j1 = rk >= -1 ? 1 : -rk;
      const std::int64_t j2 = r - 1 <= pk ? kk - 1 : P - r;
      for (std::int64_t j = j1; j <= j2; j++) {
        A(s2, j) = at(pk + 1, rk + j) == 0 ? 0 : (A(s1, j) - A(s1, j - 1)) / at(pk + 1, rk + j);
        d += A(s2, j) * at(rk + j, pk);
      }
      if (r <= pk) {
        A(s2, kk) = at(pk + 1, r) == 0 ? 0 : -A(s1, kk - 1) / at(pk + 1, r);
        d += A(s2, kk) * at(r, pk);
      }
      ders[static_cast<std::size_t>(kk)][static_cast<std::size_t>(r)] = d;
      std::swap(s1, s2);
    }
  }
  auto f = static_cast<double>(p);
  for (std::size_t kk = 1; kk <= n; kk++) {
    for (std::size_t j = 0; j <= p; j++) {
      ders[kk][j] *= f;
    }
    f *= static_cast<double>(P - static_cast<std::int64_t>(kk));
  }
  return ders;
}

P2 point(const Curve& c, double u) {
  const std::int64_t p = c.deg;
  const auto m = static_cast<std::int64_t>(c.poles.size());
  const std::int64_t k = find_span(c.knots, p, m, u);
  const std::vector<double> N = basis_funs(k, u, p, c.knots);
  double x = 0;
  double y = 0;
  for (std::int64_t j = 0; j <= p; j++) {
    const P2& q = pole(c.poles, k - p + j);
    x += N[static_cast<std::size_t>(j)] * q[0];
    y += N[static_cast<std::size_t>(j)] * q[1];
  }
  return {x, y};
}

std::vector<P2> derivs(const Curve& c, double u, int n) {
  const std::int64_t p = c.deg;
  const auto m = static_cast<std::int64_t>(c.poles.size());
  const std::int64_t k = find_span(c.knots, p, m, u);
  const auto D = ders_basis_funs(k, u, p, std::min<std::int64_t>(n, p), c.knots);
  std::vector<P2> out;
  for (int i = 0; i <= n; i++) {
    double x = 0;
    double y = 0;
    if (i <= p) {
      for (std::int64_t j = 0; j <= p; j++) {
        const P2& q = pole(c.poles, k - p + j);
        x += D[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] * q[0];
        y += D[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] * q[1];
      }
    }
    out.push_back({x, y});
  }
  return out;
}

std::vector<double> chord_params(const std::vector<P2>& Q, bool closed, const i18n::Catalog& w) {
  std::vector<double> t{0};
  const std::size_t n = Q.size();
  for (std::size_t i = 1; i < n + (closed ? 1 : 0); i++) {
    const P2& a = Q[i - 1];
    const P2& b = Q[i % n];
    const double d = base::js_hypot(b[0] - a[0], b[1] - a[1]);
    if (!(d > 0)) {
      throw Thrown(w.t("model.sketch_spline.fit_points_coincide"));
    }
    t.push_back(t[i - 1] + d);
  }
  return t;
}

std::vector<std::vector<double>> solve_dense(const std::vector<std::vector<double>>& A,
                                             const std::vector<std::vector<double>>& B, const i18n::Catalog& w) {
  const std::size_t n = A.size();
  const std::size_t wd = B[0].size();
  std::vector<std::vector<double>> M(n);
  for (std::size_t i = 0; i < n; i++) {
    M[i] = A[i];
    M[i].insert(M[i].end(), B[i].begin(), B[i].end());
  }
  for (std::size_t c = 0; c < n; c++) {
    std::size_t piv = c;
    for (std::size_t r = c + 1; r < n; r++) {
      if (std::abs(M[r][c]) > std::abs(M[piv][c])) {
        piv = r;
      }
    }
    if (!(std::abs(M[piv][c]) > 1e-300)) {
      throw Thrown(w.t("model.sketch_spline.cannot_pass"));
    }
    std::swap(M[c], M[piv]);
    for (std::size_t r = 0; r < n; r++) {
      if (r == c || M[r][c] == 0) {
        continue;
      }
      const double f = M[r][c] / M[c][c];
      for (std::size_t k = c; k < n + wd; k++) {
        M[r][k] -= f * M[c][k];
      }
    }
  }
  std::vector<std::vector<double>> out(n);
  for (std::size_t i = 0; i < n; i++) {
    for (std::size_t k = n; k < n + wd; k++) {
      out[i].push_back(M[i][k] / M[i][i]);
    }
  }
  return out;
}

Curve fit(const std::vector<P2>& Q, const FitOptions& o, const i18n::Catalog& w) {
  if (o.closed) {
    const std::size_t n = Q.size();
    if (n < 3) {
      throw Thrown(w.t("model.sketch_spline.closed_three"));
    }
    const std::vector<double> tau = chord_params(Q, true, w);
    const Curve flat = periodic_curve(Q, tau);
    std::vector<std::vector<double>> A(n, std::vector<double>(n, 0));
    for (std::size_t i = 0; i < n; i++) {
      const auto k = static_cast<std::int64_t>(i) + DEG;
      const std::vector<double> N = basis_funs(k, tau[i], DEG, flat.knots);
      for (std::int64_t j = 0; j <= DEG; j++) {
        A[i][static_cast<std::size_t>(k - DEG + j) % n] += N[static_cast<std::size_t>(j)];
      }
    }
    std::vector<std::vector<double>> B;
    B.reserve(Q.size());
    for (const P2& q : Q) {
      B.push_back({q[0], q[1]});
    }
    const auto X = solve_dense(A, B, w);
    std::vector<P2> P;
    P.reserve(X.size());
    for (const auto& r : X) {
      P.push_back({r[0], r[1]});
    }
    return periodic_curve(P, tau);
  }
  const std::size_t n1 = Q.size();
  if (n1 < 2) {
    throw Thrown(w.t("model.sketch_spline.open_two"));
  }
  const std::vector<double> t = chord_params(Q, false, w);
  std::vector<double> U{t[0], t[0], t[0], t[0]};
  U.insert(U.end(), t.begin() + 1, t.end() - 1);
  for (int i = 0; i < 4; i++) {
    U.push_back(t[n1 - 1]);
  }
  const auto m = static_cast<std::int64_t>(n1 + 2);
  const std::int64_t p = DEG;
  std::vector<std::vector<double>> A;
  std::vector<std::vector<double>> B;
  const auto row = [&](double u, std::int64_t d, std::vector<double> rhs) {
    const std::int64_t k = find_span(U, p, m, u);
    const auto D = ders_basis_funs(k, u, p, d, U);
    std::vector<double> r(static_cast<std::size_t>(m), 0);
    for (std::int64_t j = 0; j <= p; j++) {
      const std::int64_t at = k - p + j;
      if (at < 0 || at >= m) {
        throw Thrown("Cannot read properties of undefined (reading '0')");
      }
      r[static_cast<std::size_t>(at)] = D[static_cast<std::size_t>(d)][static_cast<std::size_t>(j)];
    }
    A.push_back(std::move(r));
    B.push_back(std::move(rhs));
  };
  const auto end = [&](double u, const std::optional<double>& ang) {
    if (ang && std::isfinite(*ang)) {
      row(u, 1, {base::js_cos(*ang), base::js_sin(*ang)});
    } else {
      row(u, 2, {0, 0});
    }
  };
  row(t[0], 0, {Q[0][0], Q[0][1]});
  end(t[0], o.a0);
  for (std::size_t i = 1; i + 1 < n1; i++) {
    row(t[i], 0, {Q[i][0], Q[i][1]});
  }
  end(t[n1 - 1], o.a1);
  row(t[n1 - 1], 0, {Q[n1 - 1][0], Q[n1 - 1][1]});
  const auto X = solve_dense(A, B, w);
  std::vector<P2> P;
  P.reserve(X.size());
  for (const auto& r : X) {
    P.push_back({r[0], r[1]});
  }
  P[0] = Q[0];
  P[static_cast<std::size_t>(m - 1)] = Q[n1 - 1];
  return {.deg = DEG, .knots = std::move(U), .poles = std::move(P), .periodic = false, .n = 0, .P = {}, .tau = {}};
}

Curve insert(const Curve& c, double u) {
  const std::int64_t p = c.deg;
  const auto& U = c.knots;
  const auto& P = c.poles;
  const auto m = static_cast<std::int64_t>(P.size());
  const std::int64_t k = find_span(U, p, m, u);
  std::vector<P2> Q;
  for (std::int64_t i = 0; i <= k - p; i++) {
    Q.push_back(pole(P, i));
  }
  for (std::int64_t i = k - p + 1; i <= k; i++) {
    const double a = (u - kn(U, i)) / (kn(U, i + p) - kn(U, i));
    const P2& q0 = pole(P, i - 1);
    const P2& q1 = pole(P, i);
    Q.push_back({(1 - a) * q0[0] + a * q1[0], (1 - a) * q0[1] + a * q1[1]});
  }
  for (std::int64_t i = k; i < m; i++) {
    Q.push_back(pole(P, i));
  }
  std::vector<double> K;
  for (std::int64_t i = 0; i <= k && static_cast<std::size_t>(i) < U.size(); i++) {
    K.push_back(U[static_cast<std::size_t>(i)]);
  }
  K.push_back(u);
  for (std::int64_t i = k + 1; std::cmp_less(i, U.size()); i++) {
    if (i >= 0) {
      K.push_back(U[static_cast<std::size_t>(i)]);
    }
  }
  return {.deg = c.deg, .knots = std::move(K), .poles = std::move(Q), .periodic = false, .n = 0, .P = {}, .tau = {}};
}

double curvature(const Curve& c, double u) {
  const auto d = derivs(c, u, 2);
  const P2& d1 = d[1];
  const P2& d2 = d[2];
  const double s = base::js_hypot(d1[0], d1[1]);
  return s > 0 ? (d1[0] * d2[1] - d1[1] * d2[0]) / (s * s * s) : 0;
}

double closest(const Curve& c, P2 p, double per_span) {
  const auto [lo, hi] = domain(c);
  double best = lo;
  double bd = std::numeric_limits<double>::infinity();
  for (const auto& [u0, u1] : spans(c)) {
    for (std::int64_t i = 0; static_cast<double>(i) <= per_span; i++) {
      const double u = u0 + (u1 - u0) * static_cast<double>(i) / per_span;
      const P2 q = point(c, u);
      const double d = (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]);
      if (d < bd) {
        bd = d;
        best = u;
      }
    }
  }
  double u = best;
  for (int it = 0; it < 30; it++) {
    const auto d = derivs(c, u, 2);
    const P2& q = d[0];
    const P2& d1 = d[1];
    const P2& d2 = d[2];
    const P2 r{q[0] - p[0], q[1] - p[1]};
    const double f = r[0] * d1[0] + r[1] * d1[1];
    const double df = d1[0] * d1[0] + d1[1] * d1[1] + r[0] * d2[0] + r[1] * d2[1];
    if (!(df > 0)) {
      break;
    }
    double un = u - f / df;
    if (c.periodic) {
      const double T = hi - lo;
      while (un < lo) {
        un += T;
      }
      while (un > hi) {
        un -= T;
      }
    } else {
      const double m = std::isnan(un) || std::isnan(hi) ? NaN : std::min(hi, un);  // Math.max(lo, Math.min(hi, un))
      un = jsx::max(lo, m);
    }
    if (std::abs(un - u) < 1e-15 * jsx::max(1, std::abs(u))) {
      u = un;
      break;
    }
    u = un;
  }
  return u;
}

/// Array#slice(a, b)'s [start, end) on a length n: negative counts from the end, both clamped.
std::pair<std::ptrdiff_t, std::ptrdiff_t> slice_range(std::int64_t n, std::int64_t a, std::int64_t b) {
  const auto fix = [n](std::int64_t x) { return x < 0 ? std::max<std::int64_t>(n + x, 0) : std::min(x, n); };
  const std::int64_t s = fix(a);
  const std::int64_t e = std::max(fix(b), s);
  return {static_cast<std::ptrdiff_t>(s), static_cast<std::ptrdiff_t>(e)};
}

std::vector<double> nums(const Value& v) {
  std::vector<double> out;
  if (v.is_array()) {
    for (const Value& x : v.as_array()) {
      out.push_back(base::js_to_number(x));
    }
  }
  return out;
}

}  // namespace

std::vector<P2> points_of(const Value& v) {
  std::vector<P2> out;
  if (v.is_array()) {
    for (const Value& q : v.as_array()) {
      const auto& a = q.is_array() ? q.as_array() : base::Array{};
      out.push_back({a.empty() ? NaN : base::js_to_number(a[0]), a.size() < 2 ? NaN : base::js_to_number(a[1])});
    }
  }
  return out;
}
Value points_value(const std::vector<P2>& pts) {
  base::Array a;
  a.reserve(pts.size());
  for (const P2& p : pts) {
    a.emplace_back(base::Array{p[0], p[1]});
  }
  return a;
}

namespace {

/// A count from untrusted JSON: NaN (a missing or odd field) is 0, the rest clamped.
int count_of(const Value& v, double hi) {
  const double d = base::js_to_number(v);
  return std::isnan(d) ? 0 : static_cast<int>(std::clamp(d, 0.0, hi));
}

}  // namespace

Curve from_value(const Value& v) {
  Curve c;
  c.deg = count_of(v.get("deg"), 64);
  c.knots = nums(v.get("knots"));
  c.poles = points_of(v.get("poles"));
  c.periodic = v.get("periodic").truthy();
  if (c.periodic) {
    c.n = count_of(v.get("n"), 1e6);
    c.P = points_of(v.get("P"));
    c.tau = nums(v.get("tau"));
  }
  return c;
}
Value to_value(const Curve& c) {
  base::Array knots(c.knots.begin(), c.knots.end());
  Value o = base::make_object({{"deg", c.deg}, {"knots", std::move(knots)}, {"poles", points_value(c.poles)}});
  if (c.periodic) {
    o.append("periodic", true);
    o.append("n", c.n);
    o.append("P", points_value(c.P));
    o.append("tau", base::Array(c.tau.begin(), c.tau.end()));
  }
  return o;
}

std::array<double, 2> domain(const Curve& c) {
  return {kn(c.knots, c.deg), kn(c.knots, static_cast<std::int64_t>(c.poles.size()))};
}
base::Result<P2> point_at(const Curve& c, double u) {
  return guard([&] { return point(c, u); });
}
base::Result<std::vector<P2>> derivs_at(const Curve& c, double u, int n) {
  return guard([&] { return derivs(c, u, n); });
}
std::vector<std::array<double, 2>> spans(const Curve& c) {
  std::vector<std::array<double, 2>> out;
  for (std::int64_t k = c.deg; std::cmp_less(k, c.poles.size()); k++) {
    if (kn(c.knots, k + 1) > kn(c.knots, k)) {
      out.push_back({kn(c.knots, k), kn(c.knots, k + 1)});
    }
  }
  return out;
}

Curve periodic_curve(const std::vector<P2>& P, const std::vector<double>& tau, int periods) {
  const auto n = static_cast<std::int64_t>(P.size());
  const double T = kn(tau, n) - kn(tau, 0);
  const auto tk = [&](std::int64_t i) {
    const auto q = static_cast<std::int64_t>(std::floor(static_cast<double>(i) / static_cast<double>(n)));
    return kn(tau, i - q * n) + static_cast<double>(q) * T;
  };
  const std::int64_t m = periods * n + DEG;
  Curve c;
  c.deg = DEG;
  for (std::int64_t j = 0; j <= m + DEG; j++) {
    c.knots.push_back(tk(j - DEG));
  }
  for (std::int64_t j = 0; j < m; j++) {
    c.poles.push_back(P[static_cast<std::size_t>(((j % n) + n) % n)]);
  }
  c.periodic = true;
  c.n = static_cast<int>(n);
  c.P = P;
  c.tau = tau;
  return c;
}

std::vector<double> clamped_uniform(int m, int p) {
  std::vector<double> U;
  for (int i = 0; i <= p; i++) {
    U.push_back(0);
  }
  for (int i = 1; i <= m - p - 1; i++) {
    U.push_back(i);
  }
  for (int i = 0; i <= p; i++) {
    U.push_back(m - p);
  }
  return U;
}

Curve control_curve(const std::vector<P2>& P, bool closed, const std::optional<std::vector<double>>& knots) {
  if (closed) {
    std::vector<double> tau;
    for (std::size_t i = 0; i <= P.size(); i++) {
      tau.push_back(static_cast<double>(i));
    }
    return periodic_curve(P, tau);
  }
  const int p = std::min(DEG, static_cast<int>(P.size()) - 1);
  Curve c;
  c.deg = p;
  c.knots = knots ? *knots : clamped_uniform(static_cast<int>(P.size()), p);
  c.poles = P;
  return c;
}

std::optional<std::string> knot_problem(const Value& Uv, double m, const i18n::Catalog& w) {
  const double p = std::min(static_cast<double>(DEG), m - 1);
  if (!Uv.is_array() || static_cast<double>(Uv.as_array().size()) != m + p + 1 ||
      !std::ranges::all_of(Uv.as_array(), [](const Value& v) {
        return v.is_number() && std::isfinite(v.as_number()) && std::abs(v.as_number()) < 1e9;
      })) {
    return w.t("model.sketch_spline.knots.mismatch");
  }
  std::vector<double> U;
  for (const Value& v : Uv.as_array()) {
    U.push_back(v.as_number());
  }
  for (std::size_t i = 1; i < U.size(); i++) {
    if (U[i] < U[i - 1]) {
      return w.t("model.sketch_spline.knots.backwards");
    }
  }
  const auto pi = static_cast<std::int64_t>(p);
  for (std::int64_t i = 1; i <= pi; i++) {
    if (kn(U, i) != U[0] || kn(U, static_cast<std::int64_t>(U.size()) - 1 - i) != U.back()) {
      return w.t("model.sketch_spline.knots.unclamped");
    }
  }
  if (!(U.back() > U[0])) {
    return w.t("model.sketch_spline.knots.no_length");
  }
  int run = 1;
  for (std::int64_t i = pi + 1; i < static_cast<std::int64_t>(U.size()) - pi - 1; i++) {
    run = kn(U, i) == kn(U, i - 1) ? run + 1 : 1;
    if (run > p) {
      return w.t("model.sketch_spline.knots.repeats");
    }
  }
  return std::nullopt;
}

base::Result<Curve> interp_fit(const std::vector<P2>& Q, const FitOptions& o, const i18n::Catalog& w) {
  return guard([&] { return fit(Q, o, w); });
}

base::Result<std::vector<P2>> tessellate(const Curve& c, double per_span) {
  return guard([&] {
    std::vector<P2> out;
    for (const auto& [u0, u1] : spans(c)) {
      for (std::int64_t i = 0; static_cast<double>(i) < per_span; i++) {
        out.push_back(point(c, u0 + (u1 - u0) * static_cast<double>(i) / per_span));
      }
    }
    out.push_back(point(c, domain(c)[1]));
    return out;
  });
}

base::Result<std::vector<P2>> tessellate_tol(const Curve& c, double tol) {
  return guard([&] {
    std::vector<P2> out;
    // NOLINTNEXTLINE(misc-no-recursion) — depth capped at 15
    const std::function<void(double, P2, double, P2, int)> rec = [&](double u0, P2 p0, double u1, P2 p1, int depth) {
      double worst = 0;
      for (const double f : {0.25, 0.5, 0.75}) {
        const P2 q = point(c, u0 + (u1 - u0) * f);
        const double dx = p1[0] - p0[0];
        const double dy = p1[1] - p0[1];
        const double l = base::js_hypot(dx, dy);
        const double d = l > 0 ? std::abs(dx * (q[1] - p0[1]) - dy * (q[0] - p0[0])) / l
                               : base::js_hypot(q[0] - p0[0], q[1] - p0[1]);
        worst = jsx::max(worst, d);
      }
      if (worst <= tol || depth > 14) {
        out.push_back(p1);
        return;
      }
      const double um = (u0 + u1) / 2;
      const P2 pm = point(c, um);
      rec(u0, p0, um, pm, depth + 1);
      rec(um, pm, u1, p1, depth + 1);
    };
    const auto sp = spans(c);
    if (sp.empty()) {
      throw Thrown("Cannot read properties of undefined (reading '0')");
    }
    out.push_back(point(c, sp[0][0]));
    for (const auto& [u0, u1] : sp) {
      for (int i = 0; i < 4; i++) {
        const double a = u0 + (u1 - u0) * i / 4;
        const double b = u0 + (u1 - u0) * (i + 1) / 4;
        rec(a, point(c, a), b, point(c, b), 0);
      }
    }
    return out;
  });
}

base::Result<double> area_term(const Curve& c) {
  return guard([&] {
    const double g = std::sqrt(3.0 / 5);
    const std::array<std::array<double, 2>, 3> gl3{{{-g, 5.0 / 9}, {0, 8.0 / 9}, {g, 5.0 / 9}}};
    double s = 0;
    for (const auto& [u0, u1] : spans(c)) {
      const double h = (u1 - u0) / 2;
      const double m = (u0 + u1) / 2;
      for (const auto& [gg, w] : gl3) {
        const auto d = derivs(c, m + h * gg, 1);
        s += w * h * (d[0][0] * d[1][1] - d[0][1] * d[1][0]) / 2;
      }
    }
    return s;
  });
}

base::Result<double> arc_length(const Curve& c) {
  return guard([&] {
    const std::array<std::array<double, 2>, 3> g5{
        {{0, 128.0 / 225},
         {std::sqrt(5 - 2 * std::sqrt(10.0 / 7)) / 3, (322 + 13 * std::sqrt(70.0)) / 900},
         {std::sqrt(5 + 2 * std::sqrt(10.0 / 7)) / 3, (322 - 13 * std::sqrt(70.0)) / 900}}};
    double s = 0;
    for (const auto& [u0, u1] : spans(c)) {
      for (int i = 0; i < 8; i++) {
        const double a = u0 + (u1 - u0) * i / 8;
        const double b = u0 + (u1 - u0) * (i + 1) / 8;
        const double h = (b - a) / 2;
        const double m = (a + b) / 2;
        for (const auto& [g, w] : g5) {
          for (const double sg : g != 0 ? std::vector<double>{-1, 1} : std::vector<double>{1}) {
            const P2 d = derivs(c, m + sg * h * g, 1)[1];
            s += w * h * base::js_hypot(d[0], d[1]);
          }
        }
      }
    }
    return s;
  });
}

base::Result<double> closest_param(const Curve& c, P2 p, double per_span) {
  return guard([&] { return closest(c, p, per_span); });
}
base::Result<double> curvature_at(const Curve& c, double u) {
  return guard([&] { return curvature(c, u); });
}
base::Result<Curve> insert_knot(const Curve& c, double u) {
  return guard([&] { return insert(c, u); });
}

base::Result<Curve> segment(const Curve& c, double ua, double ub) {
  return guard([&] {
    const auto [lo, hi] = domain(c);
    Curve src = c;
    if (c.periodic && ub > hi + 1e-12 * jsx::max(1, std::abs(hi))) {
      src = periodic_curve(c.P, c.tau, 2);
    }
    const double eps = 1e-12 * jsx::max(jsx::max(jsx::max(1, std::abs(lo)), std::abs(hi)), hi - lo);
    const auto raise = [&](Curve cur, double u) {
      for (const double v : cur.knots) {
        if (std::abs(v - u) <= eps) {
          u = v;
          break;
        }
      }
      cur.periodic = false;
      while (std::ranges::count(cur.knots, u) < cur.deg) {
        cur = insert(cur, u);
      }
      return std::pair{cur, u};
    };
    auto [c1, A] = raise(src, ua);
    auto [c2, Bu] = raise(c1, ub);
    const auto& K = c2.knots;
    const auto& P = c2.poles;
    const std::int64_t p = c.deg;
    std::int64_t last_a = -1;
    std::int64_t first_b = -1;
    for (std::size_t j = 0; j < K.size(); j++) {
      if (K[j] == A) {
        last_a = static_cast<std::int64_t>(j);
      }
      if (K[j] == Bu && first_b < 0) {
        first_b = static_cast<std::int64_t>(j);
      }
    }
    const std::int64_t i0 = last_a - p + 1;
    const std::int64_t i1 = first_b + p - 1;
    std::vector<double> knots{A};
    const auto [ka, kb] = slice_range(static_cast<std::int64_t>(K.size()), i0, i1 + 1);
    knots.insert(knots.end(), K.begin() + ka, K.begin() + kb);
    knots.push_back(Bu);
    const auto nP = static_cast<std::int64_t>(knots.size()) - p - 1;
    const auto [pa, pb] = slice_range(static_cast<std::int64_t>(P.size()), i0 - 1, i0 - 1 + nP);
    std::vector<P2> poles(P.begin() + pa, P.begin() + pb);
    return Curve{.deg = c.deg,
                 .knots = std::move(knots),
                 .poles = std::move(poles),
                 .periodic = false,
                 .n = 0,
                 .P = {},
                 .tau = {}};
  });
}

Curve reverse(const Curve& c) {
  const auto& U = c.knots;
  const double a = U.empty() ? NaN : U.front();
  const double b = U.empty() ? NaN : U.back();
  Curve out;
  out.deg = c.deg;
  for (std::size_t j = 0; j < U.size(); j++) {
    out.knots.push_back(a + b - U[U.size() - 1 - j]);
  }
  out.poles.assign(c.poles.rbegin(), c.poles.rend());
  return out;
}

Curve normalize_knots(const Curve& c) {
  const double a = c.knots.empty() ? NaN : c.knots.front();
  const double L = (c.knots.empty() ? NaN : c.knots.back()) - a;
  const double s = L > 0 ? (static_cast<double>(c.poles.size()) - c.deg) / L : 1;
  Curve out;
  out.deg = c.deg;
  for (const double u : c.knots) {
    out.knots.push_back((u - a) * s);
  }
  out.poles = c.poles;
  return out;
}

base::Result<Offset> offset_fit(const Curve& c, double d, double tol, const i18n::Catalog& w) {
  return guard([&] {
    const auto sp = spans(c);
    const auto off = [&](double u) {
      const auto q = derivs(c, u, 1);
      const double l = base::js_hypot(q[1][0], q[1][1]);
      return P2{q[0][0] - d * q[1][1] / l, q[0][1] + d * q[1][0] / l};
    };
    for (const auto& [u0, u1] : sp) {
      for (int i = 0; i <= 32; i++) {
        const double k = curvature(c, u0 + (u1 - u0) * i / 32);
        if (k * d > 0 && std::abs(d) >= 1 / std::abs(k) - 1e-12) {
          const double r = base::js_number(base::js_to_fixed(1 / std::abs(k), 3));
          throw Thrown(w.t("model.sketch_spline.offset_too_big", {{"r", base::js_string(Value(r))}}));
        }
      }
    }
    const bool closed = c.periodic;
    std::vector<P2> truth;
    for (const auto& [u0, u1] : sp) {
      for (int i = 0; i < 40; i++) {
        truth.push_back(off(u0 + (u1 - u0) * (i + 0.5) / 40));
      }
    }
    const auto angle = [&](double u) {
      const P2 t = derivs(c, u, 1)[1];
      return base::js_atan2(t[1], t[0]);
    };
    const auto [lo, hi] = domain(c);
    for (int per = 2; per <= 64; per *= 2) {
      std::vector<P2> Q;
      for (const auto& [u0, u1] : sp) {
        for (int i = 0; i < per; i++) {
          Q.push_back(off(u0 + (u1 - u0) * i / per));
        }
      }
      if (!closed) {
        Q.push_back(off(hi));
      }
      FitOptions opts;
      opts.closed = closed;
      if (!closed) {
        opts.a0 = angle(lo);
        opts.a1 = angle(hi);
      }
      Curve f;
      try {
        f = fit(Q, opts, w);
      } catch (const Thrown&) {
        continue;
      }
      double err = 0;
      for (const P2& p : truth) {
        const P2 q = point(f, closest(f, p, 6));
        err = jsx::max(err, base::js_hypot(q[0] - p[0], q[1] - p[1]));
      }
      if (err <= tol) {
        return Offset{
            .Q = std::move(Q), .a0 = opts.a0, .a1 = opts.a1, .closed = closed, .err = err, .curve = std::move(f)};
      }
    }
    throw Thrown(w.t("model.sketch_spline.offset_tolerance", {{"tol", base::js_string(Value(tol))}}));
  });
}

}  // namespace cc::partcraft::spline
