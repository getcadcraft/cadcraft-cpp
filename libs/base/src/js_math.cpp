// SPDX-License-Identifier: Apache-2.0 AND BSD-3-Clause
//
// atan and atan2 as JavaScript computes them (V8's src/base/ieee754.cc, which Math.atan / Math.atan2 run), so a port
// gets the very same last bit. The algorithm, the constants and the comments are V8's, adapted from fdlibm;
// the notices of both are kept as their licenses ask (THIRD_PARTY.md):
//
// The following is adapted from fdlibm (http://www.netlib.org/fdlibm).
//
// ====================================================
// Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//
// Developed at SunSoft, a Sun Microsystems, Inc. business.
// Permission to use, copy, modify, and distribute this
// software is freely granted, provided that this notice
// is preserved.
// ====================================================
//
// The original source code covered by the above license above has been
// modified significantly by Google Inc.
// Copyright 2016 the V8 project authors. All rights reserved.
// (V8: BSD-3-Clause, https://chromium.googlesource.com/v8/v8/+/main/LICENSE)
//
// Changes for this fork: word access through std::bit_cast; the `volatile tiny` terms (which only raise the inexact
// flag) are plain constants; C++ naming.
#include "cc/base/js_math.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace cc::base {

namespace {

std::int32_t high_word(double d) noexcept {
  return static_cast<std::int32_t>(std::bit_cast<std::uint64_t>(d) >> 32U);
}
std::uint32_t low_word(double d) noexcept {
  return static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(d) & 0xFFFFFFFFULL);
}

}  // namespace

double js_atan(double x) noexcept {
  static constexpr std::array<double, 4> kAtanHi = {
      4.63647609000806093515e-01, /* atan(0.5)hi 0x3FDDAC67, 0x0561BB4F */
      7.85398163397448278999e-01, /* atan(1.0)hi 0x3FE921FB, 0x54442D18 */
      9.82793723247329054082e-01, /* atan(1.5)hi 0x3FEF730B, 0xD281F69B */
      1.57079632679489655800e+00, /* atan(inf)hi 0x3FF921FB, 0x54442D18 */
  };
  static constexpr std::array<double, 4> kAtanLo = {
      2.26987774529616870924e-17, /* atan(0.5)lo 0x3C7A2B7F, 0x222F65E2 */
      3.06161699786838301793e-17, /* atan(1.0)lo 0x3C81A626, 0x33145C07 */
      1.39033110312309984516e-17, /* atan(1.5)lo 0x3C700788, 0x7AF0CBBD */
      6.12323399573676603587e-17, /* atan(inf)lo 0x3C91A626, 0x33145C07 */
  };
  static constexpr std::array<double, 11> kAT = {
      3.33333333333329318027e-01,  /* 0x3FD55555, 0x5555550D */
      -1.99999999998764832476e-01, /* 0xBFC99999, 0x9998EBC4 */
      1.42857142725034663711e-01,  /* 0x3FC24924, 0x920083FF */
      -1.11111104054623557880e-01, /* 0xBFBC71C6, 0xFE231671 */
      9.09088713343650656196e-02,  /* 0x3FB745CD, 0xC54C206E */
      -7.69187620504482999495e-02, /* 0xBFB3B0F2, 0xAF749A6D */
      6.66107313738753120669e-02,  /* 0x3FB10D66, 0xA0D03D51 */
      -5.83357013379057348645e-02, /* 0xBFADDE2D, 0x52DEFD9A */
      4.97687799461593236017e-02,  /* 0x3FA97B4B, 0x24760DEB */
      -3.65315727442169155270e-02, /* 0xBFA2B444, 0x2C6A6C2F */
      1.62858201153657823623e-02,  /* 0x3F90AD3A, 0xE322DA11 */
  };
  constexpr double one = 1.0;

  const std::int32_t hx = high_word(x);
  const std::int32_t ix = hx & 0x7FFFFFFF;
  int id = 0;
  if (ix >= 0x44100000) { /* if |x| >= 2^66 */
    const std::uint32_t low = low_word(x);
    if (ix > 0x7FF00000 || (ix == 0x7FF00000 && (low != 0))) {
      return x + x; /* NaN */
    }
    return hx > 0 ? kAtanHi[3] + kAtanLo[3] : -kAtanHi[3] - kAtanLo[3];
  }
  if (ix < 0x3FDC0000) {   /* |x| < 0.4375 */
    if (ix < 0x3E400000) { /* |x| < 2^-27 */
      return x;
    }
    id = -1;
  } else {
    x = std::fabs(x);
    if (ix < 0x3FF30000) {   /* |x| < 1.1875 */
      if (ix < 0x3FE60000) { /* 7/16 <=|x|<11/16 */
        id = 0;
        x = (2.0 * x - one) / (2.0 + x);
      } else { /* 11/16<=|x|< 19/16 */
        id = 1;
        x = (x - one) / (x + one);
      }
    } else {
      if (ix < 0x40038000) { /* |x| < 2.4375 */
        id = 2;
        x = (x - 1.5) / (one + 1.5 * x);
      } else { /* 2.4375 <= |x| < 2^66 */
        id = 3;
        x = -1.0 / x;
      }
    }
  }
  /* end of argument reduction */
  double z = x * x;
  const double w = z * z;
  /* break sum from i=0 to 10 aT[i]z**(i+1) into odd and even poly */
  const double s1 = z * (kAT[0] + w * (kAT[2] + w * (kAT[4] + w * (kAT[6] + w * (kAT[8] + w * kAT[10])))));
  const double s2 = w * (kAT[1] + w * (kAT[3] + w * (kAT[5] + w * (kAT[7] + w * kAT[9]))));
  if (id < 0) {
    return x - x * (s1 + s2);
  }
  const auto i = static_cast<std::size_t>(id);
  z = kAtanHi[i] - ((x * (s1 + s2) - kAtanLo[i]) - x);
  return (hx < 0) ? -z : z;
}

double js_atan2(double y, double x) noexcept {
  constexpr double zero = 0.0;
  constexpr double pi_o_4 = 7.8539816339744827900E-01; /* 0x3FE921FB, 0x54442D18 */
  constexpr double pi_o_2 = 1.5707963267948965580E+00; /* 0x3FF921FB, 0x54442D18 */
  constexpr double pi = std::numbers::pi;              /* 0x400921FB, 0x54442D18 */
  constexpr double pi_lo = 1.2246467991473531772E-16;  /* 0x3CA1A626, 0x33145C07 */

  const std::int32_t hx = high_word(x);
  const std::uint32_t lx = low_word(x);
  const std::int32_t ix = hx & 0x7FFFFFFF;
  const std::int32_t hy = high_word(y);
  const std::uint32_t ly = low_word(y);
  const std::int32_t iy = hy & 0x7FFFFFFF;
  // (lx | -lx) >> 31: 1 when the low word is not zero
  const auto nz = [](std::uint32_t l) { return static_cast<std::int32_t>((l | (0U - l)) >> 31U); };
  if ((ix | nz(lx)) > 0x7FF00000 || (iy | nz(ly)) > 0x7FF00000) {
    return x + y; /* x or y is NaN */
  }
  if (((static_cast<std::uint32_t>(hx) - 0x3FF00000U) | lx) == 0) {
    return js_atan(y); /* x=1.0 */
  }
  std::int32_t m = ((hy >> 31) & 1) | ((hx >> 30) & 2); /* 2*sign(x)+sign(y) */

  /* when y = 0 */
  if ((static_cast<std::uint32_t>(iy) | ly) == 0) {
    switch (m) {
      case 0:
      case 1: return y;    /* atan(+-0,+anything)=+-0 */
      case 2: return pi;   /* atan(+0,-anything) = pi */
      default: return -pi; /* atan(-0,-anything) =-pi */
    }
  }
  /* when x = 0 */
  if ((static_cast<std::uint32_t>(ix) | lx) == 0) {
    return (hy < 0) ? -pi_o_2 : pi_o_2;
  }
  /* when x is INF */
  if (ix == 0x7FF00000) {
    if (iy == 0x7FF00000) {
      switch (m) {
        case 0: return pi_o_4;         /* atan(+INF,+INF) */
        case 1: return -pi_o_4;        /* atan(-INF,+INF) */
        case 2: return 3.0 * pi_o_4;   /*atan(+INF,-INF)*/
        default: return -3.0 * pi_o_4; /*atan(-INF,-INF)*/
      }
    }
    switch (m) {
      case 0: return zero;  /* atan(+...,+INF) */
      case 1: return -zero; /* atan(-...,+INF) */
      case 2: return pi;    /* atan(+...,-INF) */
      default: return -pi;  /* atan(-...,-INF) */
    }
  }
  /* when y is INF */
  if (iy == 0x7FF00000) {
    return (hy < 0) ? -pi_o_2 : pi_o_2;
  }

  /* compute y/x */
  const std::int32_t k = (iy - ix) >> 20;
  double z = 0;
  if (k > 60) { /* |y/x| >  2**60 */
    z = pi_o_2 + 0.5 * pi_lo;
    m &= 1;
  } else if (hx < 0 && k < -60) {
    z = 0.0; /* 0 > |y|/x > -2**-60 */
  } else {
    z = js_atan(std::fabs(y / x)); /* safe to do y/x */
  }
  switch (m) {
    case 0: return z;                 /* atan(+,+) */
    case 1: return -z;                /* atan(-,+) */
    case 2: return pi - (z - pi_lo);  /* atan(+,-) */
    default: return (z - pi_lo) - pi; /* case 3 */
  }
}

}  // namespace cc::base
