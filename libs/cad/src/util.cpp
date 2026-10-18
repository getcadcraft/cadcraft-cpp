// SPDX-License-Identifier: Apache-2.0
#include "util.hpp"

#include <cstdint>
#include <limits>
#include <span>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"

namespace cc::cad::detail {

const Value& undef() {
  static const Value kUndef;
  return kUndef;
}

const Value& at(const Value& o, std::string_view k) {
  return o.is_object() ? o[k] : undef();
}

const Value& item(const Value& a, std::size_t i) {
  return a.is_array() && i < a.as_array().size() ? a.as_array()[i] : undef();
}

double norm_angle(double a) {
  a = std::fmod(a, kTau);
  return a < 0 ? a + kTau : a;
}

Mat m_mul(const Mat& m, const Mat& n) {
  return {m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1],        m[0] * n[2] + m[2] * n[3],
          m[1] * n[2] + m[3] * n[3], m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]};
}

Mat m_rotate(double r) {
  const double c = cc::base::js_cos(r);
  const double s = cc::base::js_sin(r);
  return {c, s, -s, c, 0, 0};
}

bool m_inverse(const Mat& m, Mat& out) {
  const double d = m_det(m);
  if (d == 0 || std::isnan(d)) {  // `if (!d) return null`
    return false;
  }
  const double a = m[0];
  const double b = m[1];
  const double c = m[2];
  const double e = m[3];
  const double f = m[4];
  const double gg = m[5];
  out = {e / d, -b / d, -c / d, a / d, (c * gg - e * f) / d, (b * f - a * gg) / d};
  return true;
}

Mat insert_matrix(const Value& e, const Value& blk) {
  // const b = blk.base || { x: 0, y: 0 }
  const Value b = g(blk, "base");
  const double bx = b.truthy() ? num(g(b, "x")) : 0;
  const double by = b.truthy() ? num(g(b, "y")) : 0;
  const Value rot = g(e, "rot");
  const Value sx = g(e, "sx");
  const Value sy = g(e, "sy");
  return m_mul(m_translate(num(g(e, "x")), num(g(e, "y"))),
               m_mul(m_rotate(rot.truthy() ? num(rot) : 0),
                     m_mul(m_scale(nullish(sx) ? 1 : num(sx), nullish(sy) ? 1 : num(sy)), m_translate(-bx, -by))));
}

Value tx_text(const Value& e, const Mat& m) {
  // mSimilar(m) || { s: sqrt|det|, rot: atan2(m1, m0), flip: det < 0 }
  const double sx = base::js_hypot(m[0], m[1]);
  const double sy = base::js_hypot(m[2], m[3]);
  const double dot = m[0] * m[2] + m[1] * m[3];
  double s = sx;
  double srot = base::js_atan2(m[1], m[0]);
  const bool flip = m_det(m) < 0;
  if (std::abs(sx - sy) > 1e-9 * std::max(sx, sy) || std::abs(dot) > 1e-9 * sx * sy) {
    s = std::sqrt(std::abs(m_det(m)));
  }
  const double x = num(g(e, "x"));
  const double y = num(g(e, "y"));
  const Value er = g(e, "rot");
  const double r0 = er.truthy() ? num(er) : 0;
  double rot = r0 + srot;
  if (flip) {
    rot = base::js_atan2(m[1], m[0]) - r0;
    if (cc::base::js_cos(rot) < -1e-9) {
      rot += std::numbers::pi;
    }
  }
  Value out = e;
  out.set("x", m[0] * x + m[2] * y + m[4]);
  out.set("y", m[1] * x + m[3] * y + m[5]);
  out.set("h", num(g(e, "h")) * s);
  out.set("rot", norm_angle(rot));
  if (g(e, "ww").truthy()) {
    out.set("ww", num(g(e, "ww")) * s);
  }
  return out;
}

// ---------------------------------------------------------------- text

namespace {

/// One UTF-8 code point at i (WTF-8 surrogates included); advances i. Invalid bytes come back one at a time.
std::uint32_t next_cp(std::string_view s, std::size_t& i) {
  const auto b = [&](std::size_t k) { return static_cast<std::uint32_t>(static_cast<unsigned char>(s[k])); };
  const std::uint32_t c = b(i);
  std::size_t n = 0;
  std::uint32_t cp = c;
  if (c >= 0xF0 && i + 3 < s.size()) {
    n = 3;
    cp = c & 0x07U;
  } else if (c >= 0xE0 && i + 2 < s.size()) {
    n = 2;
    cp = c & 0x0FU;
  } else if (c >= 0xC0 && i + 1 < s.size()) {
    n = 1;
    cp = c & 0x1FU;
  }
  for (std::size_t k = 1; k <= n; ++k) {
    cp = (cp << 6U) | (b(i + k) & 0x3FU);
  }
  i += n + 1;
  return cp;
}

/// JS \s (and String#trim's set).
bool js_space(std::uint32_t c) {
  return c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 ||
         c == 0xFEFF;
}

bool hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// The %% codes and \U+XXXX: `drop` = the letters after %% that vanish (dxf.js "uUoOkK", dwg-import.js "uUoO").
/// Each JS .replace is its own pass, in the order the JS chains them.
std::string percent_codes(std::string_view in, std::string_view drop) {
  auto pass = [](std::string_view s, std::string_view letters, std::string_view with) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
      if (i + 2 < s.size() && s[i] == '%' && s[i + 1] == '%' && letters.contains(s[i + 2])) {
        out += with;
        i += 3;
      } else {
        out += s[i];
        ++i;
      }
    }
    return out;
  };
  std::string s = pass(in, "cC", "⌀");
  s = pass(s, "dD", "°");
  s = pass(s, "pP", "±");
  s = pass(s, drop, "");
  s = pass(s, "%", "%");  // %%% → %
  // \U+XXXX → String.fromCharCode(parseInt(h, 16)): one UTF-16 unit (a lone surrogate stays as WTF-8)
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    if (i + 7 <= s.size() && s[i] == '\\' && s[i + 1] == 'U' && s[i + 2] == '+' && hex_digit(s[i + 3]) &&
        hex_digit(s[i + 4]) && hex_digit(s[i + 5]) && hex_digit(s[i + 6])) {
      std::uint32_t v = 0;
      for (std::size_t k = 3; k < 7; ++k) {
        const char c = s[i + k];
        v = v * 16 + static_cast<std::uint32_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
      }
      base::append_code_point(out, v);
      i += 7;
    } else {
      out += s[i];
      ++i;
    }
  }
  return out;
}

}  // namespace

std::string plain(std::string_view s) {
  return percent_codes(s, "uUoO");
}

std::string mtext_plain(std::string_view in) {
  std::string s = percent_codes(in, "uUoOkK");  // decodeText
  // .replace(/\\P/g, '\n')
  {
    std::string o;
    for (std::size_t i = 0; i < s.size();) {
      if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'P') {
        o += '\n';
        i += 2;
      } else {
        o += s[i++];
      }
    }
    s = std::move(o);
  }
  // .replace(/\\[fFcCHhQqTtWwAap][^;]*;/g, '')
  {
    std::string o;
    for (std::size_t i = 0; i < s.size();) {
      if (s[i] == '\\' && i + 1 < s.size() && std::string_view("fFcCHhQqTtWwAap").contains(s[i + 1])) {
        const std::size_t semi = s.find(';', i + 2);
        if (semi != std::string::npos) {
          i = semi + 1;
          continue;
        }
      }
      o += s[i++];
    }
    s = std::move(o);
  }
  // .replace(/\^I|\t/g, ' ')
  {
    std::string o;
    for (std::size_t i = 0; i < s.size();) {
      if (s[i] == '^' && i + 1 < s.size() && s[i + 1] == 'I') {
        o += ' ';
        i += 2;
      } else if (s[i] == '\t') {
        o += ' ';
        ++i;
      } else {
        o += s[i++];
      }
    }
    s = std::move(o);
  }
  // .replace(/\\S([^;^/#]*)[\^/#]([^;]*);/g, '$1/$2')
  {
    std::string o;
    for (std::size_t i = 0; i < s.size();) {
      if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'S') {
        std::size_t j = i + 2;
        while (j < s.size() && s[j] != ';' && s[j] != '^' && s[j] != '/' && s[j] != '#') {
          ++j;
        }
        if (j < s.size() && (s[j] == '^' || s[j] == '/' || s[j] == '#')) {
          const std::size_t semi = s.find(';', j + 1);
          if (semi != std::string::npos) {
            o.append(s, i + 2, j - (i + 2));
            o += '/';
            o.append(s, j + 1, semi - (j + 1));
            i = semi + 1;
            continue;
          }
        }
      }
      o += s[i++];
    }
    s = std::move(o);
  }
  // .replace(/\\[LlOoKkX]/g, '').replace(/\\~/g, ' ')
  {
    std::string o;
    for (std::size_t i = 0; i < s.size();) {
      if (s[i] == '\\' && i + 1 < s.size() && std::string_view("LlOoKkX").contains(s[i + 1])) {
        i += 2;
      } else {
        o += s[i++];
      }
    }
    std::string o2;
    for (std::size_t i = 0; i < o.size();) {
      if (o[i] == '\\' && i + 1 < o.size() && o[i + 1] == '~') {
        o2 += ' ';
        i += 2;
      } else {
        o2 += o[i++];
      }
    }
    s = std::move(o2);
  }
  // .replace(/\\\\/g, '\0').replace(/[{}]/g, '').replace(/\0/g, '\\')
  {
    std::string o;
    for (std::size_t i = 0; i < s.size();) {
      if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\\') {
        o += '\0';
        i += 2;
      } else {
        o += s[i++];
      }
    }
    std::string o2;
    for (const char c : o) {
      if (c == '{' || c == '}') {
        continue;
      }
      o2 += c == '\0' ? '\\' : c;
    }
    s = std::move(o2);
  }
  return std::string(base::js_trim(s));
}

std::string norm_tag(const Value& t) {
  const std::string up = base::js_to_upper(base::js_trim(nullish(t) ? std::string() : str(t)));
  std::string o;
  bool in_space = false;
  for (std::size_t i = 0; i < up.size();) {
    const std::size_t start = i;
    const std::uint32_t cp = next_cp(up, i);
    if (js_space(cp)) {
      if (!in_space) {
        o += '_';
      }
      in_space = true;
      continue;
    }
    in_space = false;
    o.append(up, start, i - start);
  }
  return js_slice(o, 255);
}

std::string one_line(std::string_view s) {
  std::string o;
  o.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '\r' || s[i] == '\n') {
      while (i < s.size() && (s[i] == '\r' || s[i] == '\n')) {
        ++i;
      }
      o += ' ';
    } else {
      o += s[i++];
    }
  }
  return o;
}

bool is_standard(std::string_view s) {
  constexpr std::string_view kStd = "standard";
  if (s.size() != kStd.size()) {
    return false;
  }
  for (std::size_t i = 0; i < s.size(); ++i) {
    if ((s[i] | 0x20) != kStd[i]) {
      return false;
    }
  }
  return true;
}

bool upper_eq(std::string_view s, std::string_view upper) {
  return base::js_to_upper(s) == upper;
}

std::string js_slice(std::string_view s, std::size_t n) {
  if (s.size() <= n) {
    return std::string(s);  // n UTF-8 bytes always hold at most n UTF-16 units
  }
  const auto u = base::to_utf16(s);
  if (u.size() <= n) {
    return std::string(s);
  }
  std::string out;
  base::append_utf16(out, std::span<const std::uint16_t>(u).first(n));
  return out;
}

std::string hex_color(double n) {
  const auto v = static_cast<std::uint32_t>(base::js_to_int32(n)) & 0xFFFFFFU;
  std::string h = base::hex_lower(v);
  return "#" + std::string(h.size() < 6 ? 6 - h.size() : 0, '0') + h;
}

}  // namespace cc::cad::detail
