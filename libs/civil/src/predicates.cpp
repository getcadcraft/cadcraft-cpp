// SPDX-License-Identifier: Apache-2.0
// orient2d: robust-predicates 3.0.3 esm/orient2d.js and esm/util.js (Shewchuk's adaptive orient2d; both placed in the
// public domain by their authors), followed step by step so the value returned matches. incircle: the same fast
// filter, then the determinant evaluated exactly with expansion arithmetic (Shewchuk's two-sum / two-product), which
// gives the same (exact) sign as robust-predicates' adaptive stages.
#include "cc/civil/predicates.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace cc::civil {
namespace {

constexpr double kEpsilon = 1.1102230246251565e-16;
constexpr double kSplitter = 134217729;
constexpr double kResultErrBound = (3 + 8 * kEpsilon) * kEpsilon;
constexpr double kCcwErrBoundA = (3 + 16 * kEpsilon) * kEpsilon;
constexpr double kCcwErrBoundB = (2 + 12 * kEpsilon) * kEpsilon;
constexpr double kCcwErrBoundC = (9 + 64 * kEpsilon) * kEpsilon * kEpsilon;
constexpr double kIccErrBoundA = (10 + 96 * kEpsilon) * kEpsilon;

/// fast_expansion_sum_zeroelim (util.js sum): h = e + f; returns h's length. h must hold elen + flen items.
std::size_t sum(std::size_t elen, std::span<const double> e, std::size_t flen, std::span<const double> f,
                std::span<double> h) {
  double q = 0;
  double qnew = 0;
  double hh = 0;
  double bvirt = 0;
  std::size_t eindex = 0;
  std::size_t findex = 0;
  auto at = [](std::span<const double> v, std::size_t i) { return i < v.size() ? v[i] : 0.0; };
  double enow = at(e, 0);
  double fnow = at(f, 0);
  if ((fnow > enow) == (fnow > -enow)) {
    q = enow;
    enow = at(e, ++eindex);
  } else {
    q = fnow;
    fnow = at(f, ++findex);
  }
  std::size_t hindex = 0;
  if (eindex < elen && findex < flen) {
    if ((fnow > enow) == (fnow > -enow)) {
      qnew = enow + q;
      hh = q - (qnew - enow);
      enow = at(e, ++eindex);
    } else {
      qnew = fnow + q;
      hh = q - (qnew - fnow);
      fnow = at(f, ++findex);
    }
    q = qnew;
    if (hh != 0) {
      h[hindex++] = hh;
    }
    while (eindex < elen && findex < flen) {
      if ((fnow > enow) == (fnow > -enow)) {
        qnew = q + enow;
        bvirt = qnew - q;
        hh = q - (qnew - bvirt) + (enow - bvirt);
        enow = at(e, ++eindex);
      } else {
        qnew = q + fnow;
        bvirt = qnew - q;
        hh = q - (qnew - bvirt) + (fnow - bvirt);
        fnow = at(f, ++findex);
      }
      q = qnew;
      if (hh != 0) {
        h[hindex++] = hh;
      }
    }
  }
  while (eindex < elen) {
    qnew = q + enow;
    bvirt = qnew - q;
    hh = q - (qnew - bvirt) + (enow - bvirt);
    enow = at(e, ++eindex);
    q = qnew;
    if (hh != 0) {
      h[hindex++] = hh;
    }
  }
  while (findex < flen) {
    qnew = q + fnow;
    bvirt = qnew - q;
    hh = q - (qnew - bvirt) + (fnow - bvirt);
    fnow = at(f, ++findex);
    q = qnew;
    if (hh != 0) {
      h[hindex++] = hh;
    }
  }
  if (q != 0 || hindex == 0) {
    h[hindex++] = q;
  }
  return hindex;
}

double estimate(std::size_t elen, std::span<const double> e) {
  double q = e[0];
  for (std::size_t i = 1; i < elen; ++i) {
    q += e[i];
  }
  return q;
}

struct Split {
  double hi, lo;
};
Split split(double a) {
  const double c = kSplitter * a;
  const double hi = c - (c - a);
  return {hi, a - hi};
}
/// Two_Product: a * b = x + y exactly (y returned).
double product_tail(double a, double b, double x) {
  const Split sa = split(a);
  const Split sb = split(b);
  return sa.lo * sb.lo - (x - sa.hi * sb.hi - sa.lo * sb.hi - sa.hi * sb.lo);
}
/// Two_Two_Diff of (s1, s0) − (t1, t0) into a 4-term expansion, as orient2dadapt writes it.
std::array<double, 4> two_two_diff(double s1, double s0, double t1, double t0) {
  std::array<double, 4> out{};
  double i = s0 - t0;
  double bvirt = s0 - i;
  out[0] = s0 - (i + bvirt) + (bvirt - t0);
  const double j = s1 + i;
  bvirt = j - s1;
  const double z = s1 - (j - bvirt) + (i - bvirt);
  i = z - t1;
  bvirt = z - i;
  out[1] = z - (i + bvirt) + (bvirt - t1);
  const double u3 = j + i;
  bvirt = u3 - j;
  out[2] = j - (u3 - bvirt) + (i - bvirt);
  out[3] = u3;
  return out;
}
std::array<double, 4> cross_diff(double a, double b, double c, double d) {  // a*b - c*d as 4 terms
  const double s1 = a * b;
  const double s0 = product_tail(a, b, s1);
  const double t1 = c * d;
  const double t0 = product_tail(c, d, t1);
  return two_two_diff(s1, s0, t1, t0);
}

double orient2dadapt(double ax, double ay, double bx, double by, double cx, double cy, double detsum) {
  const double acx = ax - cx;
  const double bcx = bx - cx;
  const double acy = ay - cy;
  const double bcy = by - cy;
  const std::array<double, 4> b = cross_diff(acx, bcy, acy, bcx);
  double det = estimate(4, b);
  double errbound = kCcwErrBoundB * detsum;
  if (det >= errbound || -det >= errbound) {
    return det;
  }
  double bvirt = ax - acx;
  const double acxtail = ax - (acx + bvirt) + (bvirt - cx);
  bvirt = bx - bcx;
  const double bcxtail = bx - (bcx + bvirt) + (bvirt - cx);
  bvirt = ay - acy;
  const double acytail = ay - (acy + bvirt) + (bvirt - cy);
  bvirt = by - bcy;
  const double bcytail = by - (bcy + bvirt) + (bvirt - cy);
  if (acxtail == 0 && acytail == 0 && bcxtail == 0 && bcytail == 0) {
    return det;
  }
  errbound = kCcwErrBoundC * detsum + kResultErrBound * std::abs(det);
  det += (acx * bcytail + bcy * acxtail) - (acy * bcxtail + bcx * acytail);
  if (det >= errbound || -det >= errbound) {
    return det;
  }
  std::array<double, 8> c1{};
  std::array<double, 12> c2{};
  std::array<double, 16> d{};
  std::array<double, 4> u = cross_diff(acxtail, bcy, acytail, bcx);
  const std::size_t c1len = sum(4, b, 4, u, c1);
  u = cross_diff(acx, bcytail, acy, bcxtail);
  const std::size_t c2len = sum(c1len, c1, 4, u, c2);
  u = cross_diff(acxtail, bcytail, acytail, bcxtail);
  const std::size_t dlen = sum(c2len, c2, 4, u, d);
  return d[dlen - 1];
}

// ---- exact expansions for incircle (only the sign is used)
using Exp = std::vector<double>;

Exp exp_sum(const Exp& e, const Exp& f) {
  Exp h(e.size() + f.size() + 1);
  const std::size_t n = sum(e.size(), e, f.size(), f, h);
  h.resize(n);
  return h;
}
/// scale_expansion_zeroelim: e * b.
Exp exp_scale(const Exp& e, double b) {
  Exp h(2 * e.size() + 1);
  const Split sb = split(b);
  double q = e[0] * b;
  Split se = split(e[0]);
  double hh = se.lo * sb.lo - (q - se.hi * sb.hi - se.lo * sb.hi - se.hi * sb.lo);
  std::size_t hindex = 0;
  if (hh != 0) {
    h[hindex++] = hh;
  }
  for (std::size_t i = 1; i < e.size(); ++i) {
    const double enow = e[i];
    const double product1 = enow * b;
    se = split(enow);
    const double product0 = se.lo * sb.lo - (product1 - se.hi * sb.hi - se.lo * sb.hi - se.hi * sb.lo);
    const double s = q + product0;
    const double bvirt = s - q;
    hh = q - (s - bvirt) + (product0 - bvirt);
    if (hh != 0) {
      h[hindex++] = hh;
    }
    q = product1 + s;
    hh = s - (q - product1);
    if (hh != 0) {
      h[hindex++] = hh;
    }
  }
  if (q != 0 || hindex == 0) {
    h[hindex++] = q;
  }
  h.resize(hindex);
  return h;
}
Exp exp_mul(const Exp& e, const Exp& f) {
  Exp acc{0.0};
  for (const double x : f) {
    acc = exp_sum(acc, exp_scale(e, x));
  }
  return acc;
}
Exp exp_neg(Exp e) {
  for (double& x : e) {
    x = -x;
  }
  return e;
}
/// a − b exactly (Two_Diff).
Exp exp_diff(double a, double b) {
  const double x = a - b;
  const double bvirt = a - x;
  const double avirt = x + bvirt;
  const double y = (a - avirt) + (bvirt - b);
  return y != 0 ? Exp{y, x} : Exp{x};
}

double incircle_exact(double ax, double ay, double bx, double by, double cx, double cy, double dx, double dy) {
  const Exp adx = exp_diff(ax, dx);
  const Exp ady = exp_diff(ay, dy);
  const Exp bdx = exp_diff(bx, dx);
  const Exp bdy = exp_diff(by, dy);
  const Exp cdx = exp_diff(cx, dx);
  const Exp cdy = exp_diff(cy, dy);
  const Exp alift = exp_sum(exp_mul(adx, adx), exp_mul(ady, ady));
  const Exp blift = exp_sum(exp_mul(bdx, bdx), exp_mul(bdy, bdy));
  const Exp clift = exp_sum(exp_mul(cdx, cdx), exp_mul(cdy, cdy));
  const Exp bc = exp_sum(exp_mul(bdx, cdy), exp_neg(exp_mul(cdx, bdy)));
  const Exp ca = exp_sum(exp_mul(cdx, ady), exp_neg(exp_mul(adx, cdy)));
  const Exp ab = exp_sum(exp_mul(adx, bdy), exp_neg(exp_mul(bdx, ady)));
  const Exp det = exp_sum(exp_sum(exp_mul(alift, bc), exp_mul(blift, ca)), exp_mul(clift, ab));
  return det.back();  // the largest component carries the sign
}

}  // namespace

double orient2d(double ax, double ay, double bx, double by, double cx, double cy) noexcept {
  const double detleft = (ay - cy) * (bx - cx);
  const double detright = (ax - cx) * (by - cy);
  const double det = detleft - detright;
  const double detsum = std::abs(detleft + detright);
  if (std::abs(det) >= kCcwErrBoundA * detsum) {
    return det;
  }
  return -orient2dadapt(ax, ay, bx, by, cx, cy, detsum);
}

double incircle(double ax, double ay, double bx, double by, double cx, double cy, double dx, double dy) noexcept {
  const double adx = ax - dx;
  const double bdx = bx - dx;
  const double cdx = cx - dx;
  const double ady = ay - dy;
  const double bdy = by - dy;
  const double cdy = cy - dy;
  const double bdxcdy = bdx * cdy;
  const double cdxbdy = cdx * bdy;
  const double alift = adx * adx + ady * ady;
  const double cdxady = cdx * ady;
  const double adxcdy = adx * cdy;
  const double blift = bdx * bdx + bdy * bdy;
  const double adxbdy = adx * bdy;
  const double bdxady = bdx * ady;
  const double clift = cdx * cdx + cdy * cdy;
  const double det = alift * (bdxcdy - cdxbdy) + blift * (cdxady - adxcdy) + clift * (adxbdy - bdxady);
  if (!std::isfinite(det) || !std::isfinite(ax + ay + bx + by + cx + cy + dx + dy)) {
    return std::nan("");
  }
  const double permanent = (std::abs(bdxcdy) + std::abs(cdxbdy)) * alift +
                           (std::abs(cdxady) + std::abs(adxcdy)) * blift +
                           (std::abs(adxbdy) + std::abs(bdxady)) * clift;
  const double errbound = kIccErrBoundA * permanent;
  if (det > errbound || -det > errbound) {
    return det;
  }
  try {
    return incircle_exact(ax, ay, bx, by, cx, cy, dx, dy);
  } catch (...) {  // std::bad_alloc only (small vectors)
    return det;
  }
}

}  // namespace cc::civil
