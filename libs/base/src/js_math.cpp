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

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

namespace cc::base {

namespace {

std::int32_t high_word(double d) noexcept {
  return static_cast<std::int32_t>(std::bit_cast<std::uint64_t>(d) >> 32U);
}
std::uint32_t low_word(double d) noexcept {
  return static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(d) & 0xFFFFFFFFULL);
}
double from_words(std::uint32_t hi, std::uint32_t lo) noexcept {
  return std::bit_cast<double>((static_cast<std::uint64_t>(hi) << 32U) | lo);
}

// ---- sin / cos (fdlibm k_sin.c, k_cos.c, e_rem_pio2.c, k_rem_pio2.c, as V8's ieee754.cc has them)

double kernel_sin(double x, double y, int iy) noexcept {
  constexpr double kHalf = 5.00000000000000000000e-01;
  constexpr double kS1 = -1.66666666666666324348e-01; /* 0xBFC55555, 0x55555549 */
  constexpr double kS2 = 8.33333333332248946124e-03;  /* 0x3F811111, 0x1110F8A6 */
  constexpr double kS3 = -1.98412698298579493134e-04; /* 0xBF2A01A0, 0x19C161D5 */
  constexpr double kS4 = 2.75573137070700676789e-06;  /* 0x3EC71DE3, 0x57B1FE7D */
  constexpr double kS5 = -2.50507602534068634195e-08; /* 0xBE5AE5E6, 0x8A2B9CEB */
  constexpr double kS6 = 1.58969099521155010221e-10;  /* 0x3DE5D93A, 0x5ACFD57C */
  const std::int32_t ix = high_word(x) & 0x7FFFFFFF;
  if (ix < 0x3E400000 && std::trunc(x) == 0) {  // |x| < 2**-27: (int)x == 0
    return x;
  }
  const double z = x * x;
  const double v = z * x;
  const double r = kS2 + z * (kS3 + z * (kS4 + z * (kS5 + z * kS6)));
  if (iy == 0) {
    return x + v * (kS1 + z * r);
  }
  return x - ((z * (kHalf * y - v * r) - y) - v * kS1);
}

double kernel_cos(double x, double y) noexcept {
  constexpr double kOne = 1.0;
  constexpr double kC1 = 4.16666666666666019037e-02;  /* 0x3FA55555, 0x5555554C */
  constexpr double kC2 = -1.38888888888741095749e-03; /* 0xBF56C16C, 0x16C15177 */
  constexpr double kC3 = 2.48015872894767294178e-05;  /* 0x3EFA01A0, 0x19CB1590 */
  constexpr double kC4 = -2.75573143513906633035e-07; /* 0xBE927E4F, 0x809C52AD */
  constexpr double kC5 = 2.08757232129817482790e-09;  /* 0x3E21EE9E, 0xBDB4B1C4 */
  constexpr double kC6 = -1.13596475577881948265e-11; /* 0xBDA8FAE9, 0xBE8838D4 */
  const std::int32_t ix = high_word(x) & 0x7FFFFFFF;
  if (ix < 0x3E400000 && std::trunc(x) == 0) {
    return kOne;
  }
  const double z = x * x;
  const double r = z * (kC1 + z * (kC2 + z * (kC3 + z * (kC4 + z * (kC5 + z * kC6)))));
  if (ix < 0x3FD33333) {  // |x| < 0.3
    return kOne - (0.5 * z - (z * r - x * y));
  }
  const double qx = ix > 0x3FE90000 ? 0.28125 : from_words(static_cast<std::uint32_t>(ix - 0x00200000), 0);  // x/4
  const double hz = 0.5 * z - qx;
  const double a = kOne - qx;
  return a - (hz - (z * r - x * y));
}

constexpr std::array<std::int32_t, 66> kTwoOverPi = {
    0xA2F983, 0x6E4E44, 0x1529FC, 0x2757D1, 0xF534DD, 0xC0DB62, 0x95993C, 0x439041, 0xFE5163, 0xABDEBB, 0xC561B7,
    0x246E3A, 0x424DD2, 0xE00649, 0x2EEA09, 0xD1921C, 0xFE1DEB, 0x1CB129, 0xA73EE8, 0x8235F5, 0x2EBB44, 0x84E99C,
    0x7026B4, 0x5F7E41, 0x3991D6, 0x398353, 0x39F49C, 0x845F8B, 0xBDF928, 0x3B1FF8, 0x97FFDE, 0x05980F, 0xEF2F11,
    0x8B5A0A, 0x6D1F6D, 0x367ECF, 0x27CB09, 0xB74F46, 0x3F669E, 0x5FEA2D, 0x7527BA, 0xC7EBE5, 0xF17B3D, 0x0739F7,
    0x8A5292, 0xEA6BFB, 0x5FB11F, 0x8D5D08, 0x560330, 0x46FC7B, 0x6BABF0, 0xCFBC20, 0x9AF436, 0x1DA9E3, 0x91615E,
    0xE61B08, 0x659985, 0x5F14A0, 0x68408D, 0xFFD880, 0x4D7327, 0x310606, 0x1556CA, 0x73A8C9, 0x60E27B, 0xC08C6B,
};

/// k_rem_pio2 for prec = 2 (jk = 4): x[0..nx) = the 24-bit chunks of |x| · 2^-e0; → n mod 8, y = the remainder.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) — fdlibm's algorithm, kept as written
int kernel_rem_pio2(const std::array<double, 3>& x, std::array<double, 2>& y, int e0, int nx) noexcept {
  static constexpr std::array<double, 8> kPIo2 = {
      1.57079625129699707031e+00, /* 0x3FF921FB, 0x40000000 */
      7.54978941586159635335e-08, /* 0x3E74442D, 0x00000000 */
      5.39030252995776476554e-15, /* 0x3CF84698, 0x80000000 */
      3.28200341580791294123e-22, /* 0x3B78CC51, 0x60000000 */
      1.27065575308067607349e-29, /* 0x39F01B83, 0x80000000 */
      1.22933308981111328932e-36, /* 0x387A2520, 0x40000000 */
      2.73370053816464559624e-44, /* 0x36E38222, 0x80000000 */
      2.16741683877804819444e-51, /* 0x3569F31D, 0x00000000 */
  };
  constexpr double kTwo24 = 1.67772160000000000000e+07;
  constexpr double kTwon24 = 5.96046447753906250000e-08;
  constexpr int jk = 4;  // init_jk[2]
  constexpr int jp = jk;
  std::array<std::int32_t, 20> iq{};
  std::array<double, 20> f{};
  std::array<double, 20> fq{};
  std::array<double, 20> q{};
  // fdlibm's indices stay inside its 20-element arrays; the hardened library traps if one ever did not
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  const auto at = [](auto& a, int i) -> auto& { return a[static_cast<std::size_t>(i)]; };
  const int jx = nx - 1;
  const int jv = std::max((e0 - 3) / 24, 0);
  int q0 = e0 - 24 * (jv + 1);
  int j = jv - jx;
  const int m = jx + jk;
  for (int i = 0; i <= m; ++i, ++j) {
    at(f, i) = j < 0 ? 0.0 : static_cast<double>(at(kTwoOverPi, j));
  }
  for (int i = 0; i <= jk; ++i) {
    double fw = 0.0;
    for (j = 0; j <= jx; ++j) {
      fw += at(x, j) * at(f, jx + i - j);
    }
    at(q, i) = fw;
  }
  int jz = jk;
  int n = 0;
  int ih = 0;
  double z = 0;
  for (;;) {  // recompute:
    int i = 0;
    j = jz;
    z = at(q, jz);
    for (; j > 0; ++i, --j) {
      const auto fw = static_cast<double>(static_cast<std::int32_t>(kTwon24 * z));
      at(iq, i) = static_cast<std::int32_t>(z - kTwo24 * fw);
      z = at(q, j - 1) + fw;
    }
    z = std::scalbn(z, q0);
    z -= 8.0 * std::floor(z * 0.125);
    n = static_cast<std::int32_t>(z);
    z -= static_cast<double>(n);
    ih = 0;
    if (q0 > 0) {
      const std::int32_t k = at(iq, jz - 1) >> (24 - q0);
      n += k;
      at(iq, jz - 1) -= k << (24 - q0);
      ih = at(iq, jz - 1) >> (23 - q0);
    } else if (q0 == 0) {
      ih = at(iq, jz - 1) >> 23;
    } else if (z >= 0.5) {
      ih = 2;
    }
    if (ih > 0) {
      n += 1;
      int carry = 0;
      for (i = 0; i < jz; ++i) {
        const std::int32_t w = at(iq, i);
        if (carry == 0) {
          if (w != 0) {
            carry = 1;
            at(iq, i) = 0x1000000 - w;
          }
        } else {
          at(iq, i) = 0xffffff - w;
        }
      }
      if (q0 == 1) {
        at(iq, jz - 1) &= 0x7fffff;
      } else if (q0 == 2) {
        at(iq, jz - 1) &= 0x3fffff;
      }
      if (ih == 2) {
        z = 1.0 - z;
        if (carry != 0) {
          z -= std::scalbn(1.0, q0);
        }
      }
    }
    if (z == 0.0) {
      std::int32_t any = 0;
      for (i = jz - 1; i >= jk; --i) {
        any |= at(iq, i);
      }
      if (any == 0) {  // need more terms of 2/pi
        int k = 1;
        while (k < jk && at(iq, jk - k) == 0) {
          ++k;
        }
        if (jz + k >= 19) {
          break;  // out of room (cannot happen for doubles; fdlibm's arrays are 20 long)
        }
        for (i = jz + 1; i <= jz + k; ++i) {
          at(f, jx + i) = static_cast<double>(at(kTwoOverPi, jv + i));
          double fw = 0.0;
          for (j = 0; j <= jx; ++j) {
            fw += at(x, j) * at(f, jx + i - j);
          }
          at(q, i) = fw;
        }
        jz += k;
        continue;
      }
    }
    break;
  }
  if (z == 0.0) {
    jz -= 1;
    q0 -= 24;
    while (jz > 0 && at(iq, jz) == 0) {
      --jz;
      q0 -= 24;
    }
  } else {
    z = std::scalbn(z, -q0);
    if (z >= kTwo24) {
      const auto fw = static_cast<double>(static_cast<std::int32_t>(kTwon24 * z));
      at(iq, jz) = static_cast<std::int32_t>(z - kTwo24 * fw);
      jz += 1;
      q0 += 24;
      at(iq, jz) = static_cast<std::int32_t>(fw);
    } else {
      at(iq, jz) = static_cast<std::int32_t>(z);
    }
  }
  double fw = std::scalbn(1.0, q0);
  for (int i = jz; i >= 0; --i) {
    at(q, i) = fw * static_cast<double>(at(iq, i));
    fw *= kTwon24;
  }
  for (int i = jz; i >= 0; --i) {
    fw = 0.0;
    for (int k = 0; k <= jp && k <= jz - i; ++k) {
      fw += at(kPIo2, k) * at(q, i + k);
    }
    at(fq, jz - i) = fw;
  }
  fw = 0.0;
  for (int i = jz; i >= 0; --i) {
    fw += at(fq, i);
  }
  y[0] = ih == 0 ? fw : -fw;
  fw = at(fq, 0) - fw;
  for (int i = 1; i <= jz; ++i) {
    fw += at(fq, i);
  }
  y[1] = ih == 0 ? fw : -fw;
  return n & 7;
}

/// __ieee754_rem_pio2(x, y): x = n·π/2 + y[0] + y[1]; → n.
// NOLINTNEXTLINE(readability-function-cognitive-complexity,bugprone-exception-escape) — fdlibm's, as written; no throw
int rem_pio2(double x, std::array<double, 2>& y) noexcept {
  static constexpr std::array<std::int32_t, 32> kNpio2Hw = {
      0x3FF921FB, 0x400921FB, 0x4012D97C, 0x401921FB, 0x401F6A7A, 0x4022D97C, 0x4025FDBB, 0x402921FB,
      0x402C463A, 0x402F6A7A, 0x4031475C, 0x4032D97C, 0x40346B9C, 0x4035FDBB, 0x40378FDB, 0x403921FB,
      0x403AB41B, 0x403C463A, 0x403DD85A, 0x403F6A7A, 0x40407E4C, 0x4041475C, 0x4042106C, 0x4042D97C,
      0x4043A28C, 0x40446B9C, 0x404534AC, 0x4045FDBB, 0x4046C6CB, 0x40478FDB, 0x404858EB, 0x404921FB,
  };
  constexpr double kHalf = 5.00000000000000000000e-01;
  constexpr double kTwo24 = 1.67772160000000000000e+07;
  constexpr double kInvpio2 = 6.36619772367581382433e-01; /* 0x3FE45F30, 0x6DC9C883 */
  constexpr double kPio2_1 = 1.57079632673412561417e+00;  /* 0x3FF921FB, 0x54400000 */
  constexpr double kPio2_1t = 6.07710050650619224932e-11; /* 0x3DD0B461, 0x1A626331 */
  constexpr double kPio2_2 = 6.07710050630396597660e-11;  /* 0x3DD0B461, 0x1A600000 */
  constexpr double kPio2_2t = 2.02226624879595063154e-21; /* 0x3BA3198A, 0x2E037073 */
  constexpr double kPio2_3 = 2.02226624871116645580e-21;  /* 0x3BA3198A, 0x2E000000 */
  constexpr double kPio2_3t = 8.47842766036889956997e-32; /* 0x397B839A, 0x252049C1 */
  const std::int32_t hx = high_word(x);
  const std::int32_t ix = hx & 0x7FFFFFFF;
  if (ix <= 0x3FE921FB) {
    y[0] = x;
    y[1] = 0;
    return 0;
  }
  if (ix < 0x4002D97C) {  // |x| < 3pi/4, special case with n = ±1
    if (hx > 0) {
      double z = x - kPio2_1;
      if (ix != 0x3FF921FB) {
        y[0] = z - kPio2_1t;
        y[1] = (z - y[0]) - kPio2_1t;
      } else {
        z -= kPio2_2;
        y[0] = z - kPio2_2t;
        y[1] = (z - y[0]) - kPio2_2t;
      }
      return 1;
    }
    double z = x + kPio2_1;
    if (ix != 0x3FF921FB) {
      y[0] = z + kPio2_1t;
      y[1] = (z - y[0]) + kPio2_1t;
    } else {
      z += kPio2_2;
      y[0] = z + kPio2_2t;
      y[1] = (z - y[0]) + kPio2_2t;
    }
    return -1;
  }
  if (ix <= 0x413921FB) {  // |x| ~<= 2^19 (pi/2), medium size
    const double t = std::abs(x);
    const auto n = static_cast<std::int32_t>(t * kInvpio2 + kHalf);
    const auto fn = static_cast<double>(n);
    double r = t - fn * kPio2_1;
    double w = fn * kPio2_1t;  // 1st round good to 85 bits
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) — 1 ≤ n < 32 here
    if (n < 32 && ix != kNpio2Hw[static_cast<std::size_t>(n - 1)]) {
      y[0] = r - w;
    } else {
      const std::int32_t j = ix >> 20;
      y[0] = r - w;
      std::int32_t i = j - ((high_word(y[0]) >> 20) & 0x7FF);
      if (i > 16) {  // 2nd iteration, good to 118 bits
        double tt = r;
        w = fn * kPio2_2;
        r = tt - w;
        w = fn * kPio2_2t - ((tt - r) - w);
        y[0] = r - w;
        i = j - ((high_word(y[0]) >> 20) & 0x7FF);
        if (i > 49) {  // 3rd iteration, 151 bits
          tt = r;
          w = fn * kPio2_3;
          r = tt - w;
          w = fn * kPio2_3t - ((tt - r) - w);
          y[0] = r - w;
        }
      }
    }
    y[1] = (r - y[0]) - w;
    if (hx < 0) {
      y[0] = -y[0];
      y[1] = -y[1];
      return -n;
    }
    return n;
  }
  if (ix >= 0x7FF00000) {  // inf or NaN
    y[0] = y[1] = std::numeric_limits<double>::quiet_NaN();
    return 0;
  }
  // z = scalbn(|x|, ilogb(x) - 23)
  const std::int32_t e0 = (ix >> 20) - 1046;
  double z = from_words(static_cast<std::uint32_t>(ix - (e0 << 20)), low_word(x));
  std::array<double, 3> tx{};
  for (std::size_t i = 0; i < 2; ++i) {
    tx.at(i) = static_cast<double>(static_cast<std::int32_t>(z));
    z = (z - tx.at(i)) * kTwo24;
  }
  tx[2] = z;
  int nx = 3;
  while (nx > 1 &&
         tx[static_cast<std::size_t>(nx - 1)] == 0.0) {  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    --nx;
  }
  const int n = kernel_rem_pio2(tx, y, e0, nx);
  if (hx < 0) {
    y[0] = -y[0];
    y[1] = -y[1];
    return -n;
  }
  return n;
}

}  // namespace

double js_sin(double x) noexcept {
  const std::int32_t ix = high_word(x) & 0x7FFFFFFF;
  if (ix <= 0x3FE921FB) {
    return kernel_sin(x, 0.0, 0);
  }
  if (ix >= 0x7FF00000) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  std::array<double, 2> y{};
  switch (rem_pio2(x, y) & 3) {
    case 0: return kernel_sin(y[0], y[1], 1);
    case 1: return kernel_cos(y[0], y[1]);
    case 2: return -kernel_sin(y[0], y[1], 1);
    default: return -kernel_cos(y[0], y[1]);
  }
}

double js_cos(double x) noexcept {
  const std::int32_t ix = high_word(x) & 0x7FFFFFFF;
  if (ix <= 0x3FE921FB) {
    return kernel_cos(x, 0.0);
  }
  if (ix >= 0x7FF00000) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  std::array<double, 2> y{};
  switch (rem_pio2(x, y) & 3) {
    case 0: return kernel_cos(y[0], y[1]);
    case 1: return -kernel_sin(y[0], y[1], 1);
    case 2: return -kernel_cos(y[0], y[1]);
    default: return kernel_sin(y[0], y[1], 1);
  }
}

namespace {}  // namespace

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

double js_hypot(std::span<const double> v) noexcept {
  if (v.empty()) {
    return 0;
  }
  bool nan = false;
  double max = 0;
  for (const double x : v) {
    if (std::isnan(x)) {
      nan = true;
    } else if (std::abs(x) > max) {
      max = std::abs(x);
    }
  }
  if (std::isinf(max)) {
    return max;
  }
  if (nan) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (max == 0) {
    return 0;
  }
  double sum = 0;
  double compensation = 0;
  for (const double x : v) {
    const double n = std::abs(x) / max;
    const double summand = n * n - compensation;
    const double preliminary = sum + summand;
    compensation = (preliminary - sum) - summand;
    sum = preliminary;
  }
  return std::sqrt(sum) * max;
}

}  // namespace cc::base
