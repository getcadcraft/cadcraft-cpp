// SPDX-License-Identifier: Apache-2.0
// parcels.js and traverse.js (CivilCraft 0.5), ported function by function; their regular expressions matched by hand.
#include "cc/civil/deeds.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include "cc/appkit/jsops.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/civil/units.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::make_object;
using base::Value;
constexpr double kPi = std::numbers::pi;
constexpr double kTau = 2 * kPi;

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
std::string sv(const Value& v) {
  return (v.is_null() || v.is_undefined()) ? std::string() : base::js_string(v);
}
double wrap(double a) {
  a = std::fmod(a, kTau);
  return a < 0 ? a + kTau : a;
}
std::string slice16(const std::string& s, std::size_t n) {
  const auto u = base::to_utf16(s);
  if (u.size() <= n) {
    return s;
  }
  std::string out;
  base::append_utf16(out, std::span(u).first(n));
  return out;
}
std::string ctrl_to_space(const std::string& s) {
  std::string o = s;
  for (char& c : o) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) {
      c = ' ';
    }
  }
  return o;
}

// ---- tiny matching helpers (ASCII word characters, JS white space, case-blind ASCII)
bool word(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}
bool digit(char c) {
  return c >= '0' && c <= '9';
}
bool boundary(std::string_view s, std::size_t i) {
  const bool a = i > 0 && word(s[i - 1]);
  const bool b = i < s.size() && word(s[i]);
  return a != b;
}
char up(char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
}
/// s[i…] starts with w, case-blind (w upper case ASCII)
bool ihas(std::string_view s, std::size_t i, std::string_view w) {
  if (i + w.size() > s.size()) {
    return false;
  }
  for (std::size_t k = 0; k < w.size(); ++k) {
    if (up(s[i + k]) != w[k]) {
      return false;
    }
  }
  return true;
}
std::size_t skip_ws(std::string_view s, std::size_t i) {
  while (i < s.size()) {
    const std::size_t n = base::js_space_at(s, i);
    if (n == 0) {
      break;
    }
    i += n;
  }
  return i;
}
/// A line terminator at byte i ('.' does not match it): \n, \r, U+2028, U+2029
std::size_t line_end_at(std::string_view s, std::size_t i) {
  if (s[i] == '\n' || s[i] == '\r') {
    return 1;
  }
  if (s.substr(i, 3) == "\xE2\x80\xA8" || s.substr(i, 3) == "\xE2\x80\xA9") {
    return 3;
  }
  return 0;
}
bool no_line_end(std::string_view s) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (line_end_at(s, i) != 0) {
      return false;
    }
  }
  return true;
}
/// \d+(?:\.\d+)? at i: its end, or npos
std::size_t number_end(std::string_view s, std::size_t i) {
  std::size_t j = i;
  while (j < s.size() && digit(s[j])) {
    ++j;
  }
  if (j == i) {
    return std::string_view::npos;
  }
  if (j + 1 < s.size() && s[j] == '.' && digit(s[j + 1])) {
    j += 2;
    while (j < s.size() && digit(s[j])) {
      ++j;
    }
  }
  return j;
}
/// split(/\r?\n/)
std::vector<std::string> split_lines(const std::string& t) {
  std::vector<std::string> out;
  std::string cur;
  for (const char ch : t) {
    if (ch == '\n') {
      if (!cur.empty() && cur.back() == '\r') {
        cur.pop_back();
      }
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  out.push_back(cur);
  return out;
}
/// .replace(/\b(?:feet|foot|ft|meters?|metres?[|m])\b\.?/gi, ' ')
std::string drop_units(std::string_view s, bool with_m) {
  std::string out;
  for (std::size_t i = 0; i < s.size();) {
    if (boundary(s, i)) {
      std::size_t e = std::string_view::npos;
      for (const std::string_view w : {"FEET", "FOOT", "FT"}) {
        if (ihas(s, i, w) && boundary(s, i + w.size())) {
          e = i + w.size();
          break;
        }
      }
      if (e == std::string_view::npos) {
        for (const std::string_view w : {"METER", "METRE"}) {
          if (ihas(s, i, w)) {
            const std::size_t j = i + w.size();
            if (j < s.size() && up(s[j]) == 'S' && boundary(s, j + 1)) {
              e = j + 1;
            } else if (boundary(s, j)) {
              e = j;
            }
            if (e != std::string_view::npos) {
              break;
            }
          }
        }
      }
      if (e == std::string_view::npos && with_m && ihas(s, i, "M") && boundary(s, i + 1)) {
        e = i + 1;
      }
      if (e != std::string_view::npos) {
        if (e < s.size() && s[e] == '.') {
          ++e;
        }
        out.push_back(' ');
        i = e;
        continue;
      }
    }
    out.push_back(s[i]);
    ++i;
  }
  return out;
}
std::string semis_to_space(std::string s) {
  for (char& c : s) {
    if (c == ';' || c == ',') {
      c = ' ';
    }
  }
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------------------------- parcels.js

Value clean_parcel(const Value& o) {
  if ((!o.is_object() && !o.is_array()) || !get(o, "pts").is_array()) {
    return nullptr;
  }
  Array pts;
  for (const Value& p : arr(get(o, "pts"))) {
    if (pts.size() >= 5000) {
      break;
    }
    const Value x = get(p, "x");
    const Value y = get(p, "y");
    const bool good = p.truthy() && fin(x) && fin(y) && std::abs(x.as_number()) < 1e9 && std::abs(y.as_number()) < 1e9;
    if (!good) {
      continue;
    }
    const Value b = get(p, "b");
    if (fin(b) && b.as_number() != 0 && std::abs(b.as_number()) <= 1) {
      pts.push_back(make_object({{"x", x}, {"y", y}, {"b", b}}));
    } else {
      pts.push_back(base::pt2(x.as_number(), y.as_number()));
    }
  }
  const bool any_b = std::ranges::any_of(pts, [](const Value& p) { return get(p, "b").truthy(); });
  if (pts.size() < 2 || (pts.size() == 2 && !any_b)) {
    return nullptr;
  }
  const std::string pob = slice16(std::string(base::js_trim(ctrl_to_space(sv(get(o, "pob"))))), 200);
  const std::string id = base::js_string(get(o, "id"));
  bool id_ok = id.size() >= 2 && id.size() <= 10 && id[0] == 'r';
  for (std::size_t i = 1; id_ok && i < id.size(); ++i) {
    id_ok = digit(id[i]);
  }
  const std::string name = slice16(std::string(base::js_trim(ctrl_to_space(sv(get(o, "name"))))), 120);
  Value out = make_object({{"id", id_ok ? Value(id) : Value(nullptr)},
                           {"name", name.empty() ? std::string("Parcel") : name},
                           {"pts", std::move(pts)}});
  if (!pob.empty()) {
    out.set("pob", pob);
  }
  return out;
}

std::optional<double> parse_angle(const Value& x) {
  const std::string s = sv(x);
  std::vector<double> n;
  for (std::size_t i = 0; i < s.size();) {
    if (digit(s[i])) {
      const std::size_t e = number_end(s, i);
      n.push_back(base::js_number(std::string_view(s).substr(i, e - i)));
      i = e;
    } else {
      ++i;
    }
  }
  if (n.empty() || n.size() > 3) {
    return std::nullopt;
  }
  const double d = n[0];
  const double m = n.size() > 1 ? n[1] : 0;
  const double sec = n.size() > 2 ? n[2] : 0;
  auto integer = [](double v) { return std::isfinite(v) && std::floor(v) == v; };
  if (m >= 60 || sec >= 60 || (n.size() > 1 && !integer(d)) || (n.size() > 2 && !integer(m))) {
    return std::nullopt;
  }
  return d + m / 60 + sec / 3600;
}

std::optional<double> parse_bearing(const Value& s_in) {
  std::string str = base::js_to_upper(base::js_trim(sv(s_in)));
  auto repl = [&](std::string_view from, std::string_view to) {
    for (std::size_t p = str.find(from); p != std::string::npos; p = str.find(from, p + to.size())) {
      str.replace(p, from.size(), to);
    }
  };
  repl("\xE2\x80\x99", "'");     // ’
  repl("\xE2\x80\xB2", "'");     // ′
  repl("\xE2\x80\x9D", "\"");    // ”
  repl("\xE2\x80\xB3", "\"");    // ″
  repl("\xC2\xBA", "\xC2\xB0");  // º → °
  if (str.empty() || base::utf16_length(str) > 60) {
    return std::nullopt;
  }
  // /^AZ(?:IMUTH)?\s*(.+)$/
  if (str.starts_with("AZ")) {
    for (const std::size_t after : {std::size_t{7}, std::size_t{2}}) {
      if (after == 7 && !str.starts_with("AZIMUTH")) {
        continue;
      }
      const std::size_t g = skip_ws(str, after);
      if (g < str.size() && no_line_end(std::string_view(str).substr(after))) {
        const auto a = parse_angle(Value(str.substr(g)));
        if (!a || *a > 360) {
          return std::nullopt;
        }
        return wrap(((90 - *a) * kPi) / 180);
      }
    }
  }
  // /^([NS])\s*(.*?)\s*([EW])$/
  if (str.size() < 2 || (str[0] != 'N' && str[0] != 'S') || (str.back() != 'E' && str.back() != 'W') ||
      !no_line_end(str)) {
    return std::nullopt;
  }
  const std::string inner(base::js_trim(std::string_view(str).substr(1, str.size() - 2)));
  const auto a = parse_angle(Value(inner));
  if (!a || *a > 90) {
    return std::nullopt;
  }
  double azd = 0;
  if (str[0] == 'N') {
    azd = str.back() == 'E' ? *a : 360 - *a;
  } else {
    azd = str.back() == 'E' ? 180 - *a : 180 + *a;
  }
  return wrap(((90 - azd) * kPi) / 180);
}

namespace {

/// /\bR(?:ADIUS)?\s*=?\s*(\d+(?:\.\d+)?)/i (or L / LENGTH): the first match's number
std::optional<double> named_number(std::string_view rest, char letter, std::string_view tail) {
  for (std::size_t i = 0; i < rest.size(); ++i) {
    if (!boundary(rest, i) || up(rest[i]) != letter) {
      continue;
    }
    for (const bool long_form : {true, false}) {
      std::size_t j = i + 1;
      if (long_form) {
        if (!ihas(rest, j, tail)) {
          continue;
        }
        j += tail.size();
      }
      j = skip_ws(rest, j);
      if (j < rest.size() && rest[j] == '=') {
        ++j;
      }
      j = skip_ws(rest, j);
      const std::size_t e = number_end(rest, j);
      if (e != std::string_view::npos) {
        return base::js_number(rest.substr(j, e - j));
      }
    }
  }
  return std::nullopt;
}

bool word_at(std::string_view s, std::size_t i, std::string_view w) {  // \bWORD\b
  return boundary(s, i) && ihas(s, i, w) && boundary(s, i + w.size());
}
bool has_word(std::string_view s, std::string_view w) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (word_at(s, i, w)) {
      return true;
    }
  }
  return false;
}

/// [0-9°'".\-\s] at i: its length, or 0
std::size_t delta_class(std::string_view s, std::size_t i) {
  const char c = s[i];
  if (digit(c) || c == '\'' || c == '"' || c == '.' || c == '-') {
    return 1;
  }
  if (s.substr(i, 2) == "\xC2\xB0") {
    return 2;
  }
  return base::js_space_at(s, i);
}

/// (?=\s+(?:LEFT|RIGHT|CB|R\b|L\b)|\s*$) at i
bool delta_end(std::string_view s, std::size_t i) {
  const std::size_t j = skip_ws(s, i);
  if (j == s.size()) {
    return true;
  }
  if (j == i) {
    return false;
  }
  return ihas(s, j, "LEFT") || ihas(s, j, "RIGHT") || ihas(s, j, "CB") ||
         ((up(s[j]) == 'R' || up(s[j]) == 'L') && boundary(s, j + 1));
}

/// /\b(?:D|DELTA|Δ)\s*=?\s*([0-9°'".\-\s]+?)(?=…)/i → the group
std::optional<std::string> delta_text(std::string_view rest) {
  for (std::size_t i = 0; i < rest.size(); ++i) {
    if (!boundary(rest, i)) {
      continue;
    }
    std::vector<std::size_t> starts;
    if (up(rest[i]) == 'D') {
      starts.push_back(i + 1);
    }
    if (ihas(rest, i, "DELTA")) {
      starts.push_back(i + 5);
    }
    if (rest.substr(i, 2) == "\xCE\x94" || rest.substr(i, 2) == "\xCE\xB4") {  // Δ, or δ (case-blind)
      starts.push_back(i + 2);
    }
    for (const std::size_t s0 : starts) {
      std::size_t j = skip_ws(rest, s0);
      if (j < rest.size() && rest[j] == '=') {
        ++j;
      }
      j = skip_ws(rest, j);
      for (std::size_t e = j; e < rest.size();) {
        const std::size_t n = delta_class(rest, e);
        if (n == 0) {
          break;
        }
        e += n;
        if (delta_end(rest, e)) {
          return std::string(rest.substr(j, e - j));
        }
      }
    }
  }
  return std::nullopt;
}

/// /\bCB\s*(.+?)\s*$/i → the group
std::optional<std::string> chord_text(std::string_view rest) {
  for (std::size_t i = 0; i < rest.size(); ++i) {
    if (!boundary(rest, i) || !ihas(rest, i, "CB")) {
      continue;
    }
    const std::size_t g = skip_ws(rest, i + 2);
    const std::string_view tail = rest.substr(g);
    if (tail.empty() || !no_line_end(tail)) {
      continue;
    }
    const std::string_view t = base::js_trim_end(tail);
    if (!t.empty()) {
      return std::string(t);
    }
  }
  return std::nullopt;
}

/// \s+(\d+(?:\.\d+)?)\s*'?\s*$ from i: the number's text
std::optional<std::string> dist_tail(std::string_view s, std::size_t i) {
  const std::size_t j = skip_ws(s, i);
  if (j == i) {
    return std::nullopt;
  }
  const std::size_t e = number_end(s, j);
  if (e == std::string_view::npos) {
    return std::nullopt;
  }
  std::size_t k = skip_ws(s, e);
  if (k < s.size() && s[k] == '\'') {
    ++k;
  }
  k = skip_ws(s, k);
  if (k != s.size()) {
    return std::nullopt;
  }
  return std::string(s.substr(j, e - j));
}

}  // namespace

Value parse_calls(const Value& text, const i18n::Catalog& w) {
  Array calls;
  Array errors;
  const std::vector<std::string> lines = split_lines(sv(text));
  for (std::size_t li = 0; li < lines.size() && li < 2000; ++li) {
    std::string_view raw = lines[li];
    // ^\s*(?:thence|then)\b[\s,]*
    {
      const std::size_t p = skip_ws(raw, 0);
      std::size_t e = std::string_view::npos;
      if (ihas(raw, p, "THENCE") && boundary(raw, p + 6)) {
        e = p + 6;
      } else if (ihas(raw, p, "THEN") && boundary(raw, p + 4)) {
        e = p + 4;
      }
      if (e != std::string_view::npos) {
        while (e < raw.size()) {
          const std::size_t n = base::js_space_at(raw, e);
          if (n != 0) {
            e += n;
          } else if (raw[e] == ',') {
            ++e;
          } else {
            break;
          }
        }
        raw = raw.substr(e);
      }
    }
    const std::string line(base::js_trim(semis_to_space(drop_units(raw, true))));
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    const auto n = static_cast<double>(li + 1);
    auto err = [&](const char* key) { errors.push_back(make_object({{"line", n}, {"msg", w.t(key)}})); };
    std::size_t head = 0;
    if (up(line[0]) == 'C' && boundary(line, 1)) {
      head = 1;
    } else if (ihas(line, 0, "CURVE") && boundary(line, 5)) {
      head = 5;
    }
    if (head != 0) {
      const std::string rest = " " + line.substr(head);
      const auto R = named_number(rest, 'R', "ADIUS");
      const auto L = named_number(rest, 'L', "ENGTH");
      const auto dm = delta_text(rest);
      double turn = 0;
      if (has_word(rest, "LEFT")) {
        turn = 1;
      } else if (has_word(rest, "RIGHT")) {
        turn = -1;
      }
      const auto cb = chord_text(rest);
      std::optional<double> delta;
      if (dm) {
        const auto a = parse_angle(Value(*dm));
        if (a) {
          delta = (*a * kPi) / 180;
        }
      }
      const bool has_r = R && *R > 0;
      if (!has_r) {
        err("pc.err.radius");
        continue;
      }
      if (turn == 0) {
        err("pc.err.turn");
        continue;
      }
      if (L && *L > 0) {
        delta = *L / *R;
      }
      const bool has_delta = delta && *delta > 0 && *delta < kTau;
      if (!has_delta) {
        err("pc.err.delta");
        continue;
      }
      Value chord_dir = nullptr;
      if (cb) {
        const auto d = parse_bearing(Value(*cb));
        if (!d) {
          err("pc.err.bearing");
          continue;
        }
        chord_dir = *d;
      }
      calls.push_back(
          make_object({{"kind", "curve"}, {"R", *R}, {"delta", *delta}, {"turn", turn}, {"chordDir", chord_dir}}));
      continue;
    }
    std::optional<std::pair<std::string, std::string>> m;
    if (no_line_end(line)) {
      for (std::size_t k = 0; k < line.size() && !m; ++k) {  // /^(.*?[EW])\s+(\d+…)\s*'?\s*$/i
        if (up(line[k]) == 'E' || up(line[k]) == 'W') {
          if (auto d = dist_tail(line, k + 1)) {
            m = std::pair{line.substr(0, k + 1), *d};
          }
        }
      }
      if (!m && ihas(line, 0, "AZ")) {  // /^(AZ(?:IMUTH)?\s*[0-9°'".\-\s]+?)\s+(\d+…)\s*'?\s*$/i
        for (const std::size_t after : {std::size_t{7}, std::size_t{2}}) {
          if (m || (after == 7 && !ihas(line, 0, "AZIMUTH"))) {
            continue;
          }
          const std::size_t j = skip_ws(line, after);
          for (std::size_t e = j; e < line.size() && !m;) {
            const std::size_t cn = delta_class(line, e);
            if (cn == 0) {
              break;
            }
            e += cn;
            if (auto d = dist_tail(line, e)) {
              m = std::pair{line.substr(0, e), *d};
            }
          }
        }
      }
    }
    if (!m) {
      err("pc.err.read");
      continue;
    }
    const auto dir = parse_bearing(Value(m->first));
    const double dist = base::js_number(m->second);
    if (!dir) {
      err("pc.err.bearing");
      continue;
    }
    if (!(dist > 0)) {
      err("pc.err.dist");
      continue;
    }
    calls.push_back(make_object({{"kind", "line"}, {"dir", *dir}, {"dist", dist}}));
  }
  return make_object({{"calls", std::move(calls)}, {"errors", std::move(errors)}});
}

Value run_calls(const Value& start, const Value& calls) {
  const double sx = num(get(start, "x"));
  const double sy = num(get(start, "y"));
  std::vector<Value> pts{make_object({{"x", get(start, "x")}, {"y", get(start, "y")}})};
  double px = sx;
  double py = sy;
  std::optional<double> heading;
  double length = 0;
  for (const Value& c : arr(calls)) {
    const Value kind = get(c, "kind");
    if (kind.is_string() && kind.as_string() == "line") {
      const double dist = num(get(c, "dist"));
      const double dir = num(get(c, "dir"));
      px = px + dist * base::js_cos(dir);
      py = py + dist * base::js_sin(dir);
      heading = dir;
      length += dist;
      pts.push_back(base::pt2(px, py));
      continue;
    }
    const double turn = num(get(c, "turn"));
    const double delta = num(get(c, "delta"));
    const double R = num(get(c, "R"));
    const Value cd = get(c, "chordDir");
    const double chord_dir = !(cd.is_null() || cd.is_undefined())
                                 ? num(cd)
                                 : heading.transform([&](double h) { return h + (turn * delta) / 2; }).value_or(0);
    const double chord = 2 * R * base::js_sin(delta / 2);
    pts.back().set("b", turn * base::js_tan(delta / 4));
    px = px + chord * base::js_cos(chord_dir);
    py = py + chord * base::js_sin(chord_dir);
    heading = chord_dir + (turn * delta) / 2;
    length += R * delta;
    pts.push_back(base::pt2(px, py));
  }
  const double dx = px - sx;
  const double dy = py - sy;
  const double dist = base::js_hypot(dx, dy);
  return make_object(
      {{"pts", Array(pts.begin(), pts.end())},
       {"misclosure", make_object({{"dx", dx}, {"dy", dy}, {"dist", dist}, {"dir", base::js_atan2(dy, dx)}})},
       {"length", length},
       {"precision", dist > 1e-9 ? length / dist : std::numeric_limits<double>::infinity()}});
}

namespace {
struct Arc {
  double R, L, delta, turn, chord_dir, chord, dir_in, dir_out, cx, cy;
};
Arc arc_of(double ax, double ay, double bx, double by, double bulge) {
  const double chord = base::js_hypot(bx - ax, by - ay);
  const double delta = 4 * base::js_atan(std::abs(bulge));
  const double turn = bulge > 0 ? 1 : -1;
  const double R = chord / (2 * base::js_sin(delta / 2));
  const double chord_dir = base::js_atan2(by - ay, bx - ax);
  const double dir_in = chord_dir - (turn * delta) / 2;
  const double dir_out = chord_dir + (turn * delta) / 2;
  const double to_c = dir_in + (turn * kPi) / 2;
  return {R,
          R * delta,
          delta,
          turn,
          chord_dir,
          chord,
          dir_in,
          dir_out,
          ax + R * base::js_cos(to_c),
          ay + R * base::js_sin(to_c)};
}
double px_(const Value& p) {
  return num(get(p, "x"));
}
double py_(const Value& p) {
  return num(get(p, "y"));
}
double signed_area(const Array& pts) {
  double a = 0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const Value& p = pts[i];
    const Value& q = pts[(i + 1) % pts.size()];
    a += px_(p) * py_(q) - px_(q) * py_(p);
  }
  return a / 2;
}
}  // namespace

Value courses(const Value& pts_v) {
  const Array& pts = arr(pts_v);
  Array out;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const Value& a = pts[i];
    const Value& b = pts[(i + 1) % pts.size()];
    const Value bulge = get(a, "b");
    if (bulge.truthy()) {
      const Arc c = arc_of(px_(a), py_(a), px_(b), py_(b), num(bulge));
      out.push_back(make_object({{"kind", "curve"},
                                 {"from", a},
                                 {"to", b},
                                 {"R", c.R},
                                 {"L", c.L},
                                 {"delta", c.delta},
                                 {"turn", c.turn},
                                 {"chordDir", c.chord_dir},
                                 {"chord", c.chord},
                                 {"dirIn", c.dir_in},
                                 {"dirOut", c.dir_out},
                                 {"c", base::pt2(c.cx, c.cy)}}));
    } else {
      out.push_back(make_object({{"kind", "line"},
                                 {"from", a},
                                 {"to", b},
                                 {"dir", base::js_atan2(py_(b) - py_(a), px_(b) - px_(a))},
                                 {"dist", base::js_hypot(px_(b) - px_(a), py_(b) - py_(a))}}));
    }
  }
  return out;
}

double parcel_area(const Value& pts_v) {
  const Array& pts = arr(pts_v);
  double a = 0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const Value& p = pts[i];
    const Value& q = pts[(i + 1) % pts.size()];
    a += px_(p) * py_(q) - px_(q) * py_(p);
    const Value bulge = get(p, "b");
    if (bulge.truthy()) {
      const double b = num(bulge);
      const Arc c = arc_of(px_(p), py_(p), px_(q), py_(q), b);
      double sg = b;  // Math.sign
      if (b > 0) {
        sg = 1;
      } else if (b < 0) {
        sg = -1;
      }
      a += 2 * sg * ((c.R * c.R) / 2) * (c.delta - base::js_sin(c.delta));
    }
  }
  return std::abs(a / 2);
}

double perimeter(const Value& pts) {
  double m = 0;
  for (const Value& c : arr(courses(pts))) {
    m += num(c.get(c.get("kind").as_string() == "line" ? "dist" : "L"));
  }
  return m;
}

Value centroid(const Value& pts_v) {
  const Array& pts = arr(pts_v);
  const double A = signed_area(pts);
  if (std::abs(A) < 1e-12) {
    double sx = 0;
    double sy = 0;
    for (const Value& p : pts) {
      sx += px_(p);
      sy += py_(p);
    }
    const auto n = static_cast<double>(pts.size());
    return base::pt2(sx / n, sy / n);
  }
  double cx = 0;
  double cy = 0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const Value& p = pts[i];
    const Value& q = pts[(i + 1) % pts.size()];
    const double k = px_(p) * py_(q) - px_(q) * py_(p);
    cx += (px_(p) + px_(q)) * k;
    cy += (py_(p) + py_(q)) * k;
  }
  return base::pt2(cx / (6 * A), cy / (6 * A));
}

std::string area_words(double a, std::string_view units, const i18n::Catalog& w) {
  if (units == "m") {
    return w.t("pc.area.m", {{"a", grouped(a, 0)}, {"ha", fixed(a / 10000, 4)}});
  }
  return w.t("pc.area.ft", {{"a", grouped(a, 0)}, {"ac", fixed(a / 43560, 3)}});
}

std::string legal_description(const Value& parcel, const Value& opts, const i18n::Catalog& w) {
  const Value uv = get(opts, "units");
  const std::string units = uv.is_undefined() ? std::string("ft") : sv(uv);
  const Value pob = get(opts, "pob");
  const Value pts = get(parcel, "pts");
  const Value cs = courses(pts);
  const std::string L = w.t(units == "m" ? "pc.lw.m" : "pc.lw.ft");
  auto d = [&](double v) { return fixed(v, 2) + " " + L; };
  const Array& P = arr(pts);
  const std::string start = pob.truthy() ? base::js_string(pob)
                                         : w.t("pc.pob.at", {{"n", fixed(P.empty() ? std::nan("") : py_(P[0]), 3)},
                                                             {"e", fixed(P.empty() ? std::nan("") : px_(P[0]), 3)}});
  std::vector<std::string> lines{sv(get(parcel, "name")), "", w.t("pc.begin", {{"at", start}}) + ";"};
  const Array& C = arr(cs);
  for (std::size_t i = 0; i < C.size(); ++i) {
    const Value& c = C[i];
    const bool last = i == C.size() - 1;
    std::string s;
    if (c.get("kind").as_string() == "line") {
      s = w.t("pc.call.line", {{"b", bearing(num(c.get("dir")))}, {"d", d(num(c.get("dist")))}});
    } else {
      s = w.t(num(c.get("turn")) > 0 ? "pc.call.left" : "pc.call.right",
              {{"r", d(num(c.get("R")))},
               {"l", d(num(c.get("L")))},
               {"delta", dms((num(c.get("delta")) * 180) / kPi)},
               {"cb", bearing(num(c.get("chordDir")))},
               {"cd", d(num(c.get("chord")))}});
    }
    lines.push_back(w.t("pc.thence", {{"call", s}}) + (last ? " " + w.t("pc.to.pob") + "." : ";"));
  }
  lines.emplace_back();
  lines.push_back(w.t("pc.containing", {{"area", area_words(parcel_area(pts), units, w)}}));
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    out += (i != 0 ? "\n" : "") + lines[i];
  }
  return out;
}

Value parcel_labels(const Value& parcel, const Value& opts) {
  const Value uv = get(opts, "units");
  const std::string units = uv.is_undefined() ? std::string("ft") : sv(uv);
  const Value upm = get(opts, "unitPerMm");
  const double unit_per_mm = upm.is_undefined() ? 1 : num(upm);
  const Value pts = get(parcel, "pts");
  const Array& P = arr(pts);
  const bool ft = units != "m";
  auto len = [&](double v) { return fixed(v, 2) + (ft ? "'" : " m"); };
  const bool ccw = signed_area(P) > 0;
  Array items;
  for (const Value& c : arr(courses(pts))) {
    const bool line = c.get("kind").as_string() == "line";
    const Value from = c.get("from");
    const Value to = c.get("to");
    double mx = 0;
    double my = 0;
    double dir = 0;
    if (line) {
      items.push_back(make_object({{"layer", "V-PROP"}, {"kind", "poly"}, {"pts", Array{from, to}}}));
      mx = (px_(from) + px_(to)) / 2;
      my = (py_(from) + py_(to)) / 2;
      dir = num(c.get("dir"));
    } else {
      const Value cc = c.get("c");
      const double ccx = num(cc.get("x"));
      const double ccy = num(cc.get("y"));
      const double R = num(c.get("R"));
      const double turn = num(c.get("turn"));
      const double delta = num(c.get("delta"));
      const double a0 = base::js_atan2(py_(from) - ccy, px_(from) - ccx);
      const double a1 = base::js_atan2(py_(to) - ccy, px_(to) - ccx);
      items.push_back(make_object({{"layer", "V-PROP"},
                                   {"kind", "arc"},
                                   {"c", cc},
                                   {"r", R},
                                   {"a0", turn > 0 ? a0 : a1},
                                   {"a1", turn > 0 ? a1 : a0}}));
      const double am = base::js_atan2(py_(from) - ccy, px_(from) - ccx) + (turn * delta) / 2;
      mx = ccx + R * base::js_cos(am);
      my = ccy + R * base::js_sin(am);
      dir = am + (turn * kPi) / 2;
    }
    const double out = dir + (ccw ? -kPi / 2 : kPi / 2);
    const double off = 1.6 * unit_per_mm;
    double rot = dir;
    if (base::js_cos(rot) < -1e-9 || (std::abs(base::js_cos(rot)) <= 1e-9 && base::js_sin(rot) < 0)) {
      rot += kPi;
    }
    const std::string text = line ? bearing(num(c.get("dir"))) + "  " + len(num(c.get("dist")))
                                  : "R=" + len(num(c.get("R"))) + "  L=" + len(num(c.get("L"))) +
                                        "  Delta=" + dms((num(c.get("delta")) * 180) / kPi);
    items.push_back(make_object({{"layer", "V-PROP-TEXT"},
                                 {"kind", "text"},
                                 {"x", mx + base::js_cos(out) * off},
                                 {"y", my + base::js_sin(out) * off},
                                 {"rot", rot},
                                 {"mm", 1.6},
                                 {"align", "center"},
                                 {"middle", true},
                                 {"text", text}}));
  }
  const Value ctr = centroid(pts);
  const double a = parcel_area(pts);
  const double cx = num(ctr.get("x"));
  const double cy = num(ctr.get("y"));
  items.push_back(make_object({{"layer", "V-PROP-TEXT"},
                               {"kind", "text"},
                               {"x", cx},
                               {"y", cy + 1.6 * unit_per_mm},
                               {"rot", 0},
                               {"mm", 2.2},
                               {"align", "center"},
                               {"middle", true},
                               {"text", get(parcel, "name")},
                               {"mask", true}}));
  items.push_back(make_object({{"layer", "V-PROP-TEXT"},
                               {"kind", "text"},
                               {"x", cx},
                               {"y", cy - 1.6 * unit_per_mm},
                               {"rot", 0},
                               {"mm", 1.8},
                               {"align", "center"},
                               {"middle", true},
                               {"text", grouped(a, 0) + " " + std::string(unit_of(units).area) + "  " +
                                            (ft ? fixed(a / 43560, 3) + " ac" : fixed(a / 10000, 4) + " ha")},
                               {"mask", true}}));
  return items;
}

// ---------------------------------------------------------------------------------------------- traverse.js

double dir2az(double d) {
  double a = 90 - d / (kPi / 180);
  a = std::fmod(a, 360);
  return a < 0 ? a + 360 : a;
}

Value parse_traverse(const Value& text, std::string_view mode, const i18n::Catalog& w) {
  Array legs;
  Array errors;
  Value closing = nullptr;
  const std::vector<std::string> lines = split_lines(sv(text));
  for (std::size_t li = 0; li < lines.size() && li < 2000; ++li) {
    const std::string line(base::js_trim(semis_to_space(drop_units(lines[li], false))));
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    const auto n = static_cast<double>(li + 1);
    auto err = [&](const char* key) { errors.push_back(make_object({{"line", n}, {"msg", w.t(key)}})); };
    // /^close\s+(.+)$/i
    if (ihas(line, 0, "CLOSE")) {
      const std::size_t g = skip_ws(line, 5);
      if (g > 5 && g < line.size() && no_line_end(std::string_view(line).substr(g))) {
        if (mode != "angles") {
          err("tv.err.closebearing");
          continue;
        }
        const auto a = parse_angle(Value(line.substr(g)));
        if (!a || *a >= 360) {
          err("tv.err.angle");
          continue;
        }
        closing = *a;
        continue;
      }
    }
    double ang = 0;
    std::string rest;
    if (mode == "bearings") {
      // /^(.*?[EW]|AZ(?:IMUTH)?\s*\S+)\s+(.+)$/i
      std::optional<std::pair<std::string, std::string>> m;
      auto tail = [&](std::size_t i) -> std::optional<std::string> {  // \s+(.+)$
        const std::size_t j = skip_ws(line, i);
        if (j == i || j >= line.size() || !no_line_end(std::string_view(line).substr(j))) {
          return std::nullopt;
        }
        return line.substr(j);
      };
      if (no_line_end(line)) {
        for (std::size_t k = 0; k < line.size() && !m; ++k) {
          if (up(line[k]) == 'E' || up(line[k]) == 'W') {
            if (auto r = tail(k + 1)) {
              m = std::pair{line.substr(0, k + 1), *r};
            }
          }
        }
      }
      if (!m && ihas(line, 0, "AZ")) {
        for (const std::size_t after : {std::size_t{7}, std::size_t{2}}) {
          if (m || (after == 7 && !ihas(line, 0, "AZIMUTH"))) {
            continue;
          }
          const std::size_t j = skip_ws(line, after);
          std::size_t e = j;
          while (e < line.size() && base::js_space_at(line, e) == 0) {
            ++e;
          }
          if (e > j) {
            if (auto r = tail(e)) {
              m = std::pair{line.substr(0, e), *r};
            }
          }
        }
      }
      if (!m) {
        err("tv.err.read");
        continue;
      }
      const auto b = parse_bearing(Value(m->first));
      if (!b) {
        err("tv.err.bearing");
        continue;
      }
      ang = *b;
      rest = m->second;
    } else {
      std::vector<std::string> tok;
      {
        std::string cur;
        for (std::size_t i = 0; i < line.size();) {
          const std::size_t s = base::js_space_at(line, i);
          if (s != 0) {
            tok.push_back(cur);
            cur.clear();
            i = skip_ws(line, i);
            continue;
          }
          cur.push_back(line[i]);
          ++i;
        }
        tok.push_back(cur);
      }
      auto plain = [](const std::string& x) { return !x.empty() && number_end(x, 0) == x.size(); };
      const std::size_t three = tok.size() >= 4 && plain(tok[0]) && plain(tok[1]) && plain(tok[2]) ? 3 : 1;
      std::string first;
      for (std::size_t i = 0; i < three && i < tok.size(); ++i) {
        first += (i != 0 ? " " : "") + tok[i];
      }
      for (std::size_t i = three; i < tok.size(); ++i) {
        rest += (i != three ? " " : "") + tok[i];
      }
      const auto a = parse_angle(Value(first));
      if (!a || *a >= 360) {
        err("tv.err.angle");
        continue;
      }
      ang = *a;
    }
    // /^(\d+(?:\.\d+)?)\s*'?\s*(.*)$/
    const std::string r(base::js_trim(rest));
    const std::size_t e = number_end(r, 0);
    std::optional<std::pair<double, std::string>> m2;
    if (e != std::string_view::npos) {
      std::size_t k = skip_ws(r, e);
      if (k < r.size() && r[k] == '\'') {
        ++k;
      }
      k = skip_ws(r, k);
      if (no_line_end(std::string_view(r).substr(k))) {
        m2 = std::pair{base::js_number(std::string_view(r).substr(0, e)), r.substr(k)};
      }
    }
    if (!m2 || !(m2->first > 0)) {
      err("tv.err.dist");
      continue;
    }
    const std::string name = slice16(std::string(base::js_trim(ctrl_to_space(m2->second))), 40);
    if (mode == "bearings") {
      legs.push_back(make_object({{"dir", ang}, {"dist", m2->first}, {"name", name}}));
    } else {
      legs.push_back(make_object({{"angle", ang}, {"dist", m2->first}, {"name", name}}));
    }
  }
  return make_object({{"legs", std::move(legs)}, {"errors", std::move(errors)}, {"closingAngle", closing}});
}

Value run_traverse(const Value& o, const i18n::Catalog& w) {
  Array problems;
  const Value start = get(o, "start");
  const Value mv = get(o, "mode");
  const std::string mode = mv.is_undefined() ? std::string("angles") : sv(mv);
  const Value bv = get(o, "backsight");
  const double backsight = bv.is_undefined() ? 0 : num(bv);
  const Array& legs = arr(get(o, "legs"));
  const Value cv = get(o, "close");
  const Value close = cv.is_undefined() ? Value("loop") : cv;
  const bool loop = close.is_string() && close.as_string() == "loop";
  const Value ca = get(o, "closingAngle");
  const std::size_t n = legs.size();
  constexpr double kD2R = kPi / 180;
  std::vector<double> dirs;
  double mis = 0;
  bool have_mis = false;
  double per_angle = 0;
  if (mode == "angles") {
    std::vector<double> a;
    for (const Value& l : legs) {
      a.push_back(num(get(l, "angle")));
    }
    if (loop && !(ca.is_null() || ca.is_undefined()) && n >= 2) {
      double az = backsight + a[0];
      for (std::size_t i = 1; i < n; ++i) {
        az += 180 + a[i];
      }
      const double again = az + 180 + num(ca);
      const double first = backsight + a[0];
      mis = std::fmod(std::fmod(again - first, 360) + 540, 360) - 180;
      have_mis = true;
      per_angle = -mis / static_cast<double>(n);
    }
    if (n > 0) {
      double az = backsight + a[0];
      dirs.push_back((90 - az) * kD2R);
      for (std::size_t i = 1; i < n; ++i) {
        az += 180 + a[i] + per_angle;
        dirs.push_back((90 - az) * kD2R);
      }
    }
  } else {
    for (const Value& l : legs) {
      dirs.push_back(num(get(l, "dir")));
    }
  }
  struct St {
    std::string name;
    double x, y, dir, dist, run, cx = 0, cy = 0, ax = 0, ay = 0;
  };
  std::vector<St> st;
  double x = num(get(start, "x"));
  double y = num(get(start, "y"));
  double length = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double dist = num(get(legs[i], "dist"));
    x += dist * base::js_cos(dirs[i]);
    y += dist * base::js_sin(dirs[i]);
    length += dist;
    const Value nm = get(legs[i], "name");
    st.push_back({nm.truthy() ? base::js_string(nm) : std::string(), x, y, dirs[i], dist, length});
  }
  const Value end = loop ? start : close;
  const double dx = x - num(get(end, "x"));
  const double dy = y - num(get(end, "y"));
  const double dist = base::js_hypot(dx, dy);
  for (St& s : st) {
    s.cx = -dx * (s.run / length);
    s.cy = -dy * (s.run / length);
    s.ax = s.x + s.cx;
    s.ay = s.y + s.cy;
  }
  if (n < 2) {
    problems.emplace_back(w.t("tv.few"));
  }
  Array stations;
  for (const St& s : st) {
    stations.push_back(make_object({{"name", s.name},
                                    {"x", s.x},
                                    {"y", s.y},
                                    {"dir", s.dir},
                                    {"dist", s.dist},
                                    {"run", s.run},
                                    {"cx", s.cx},
                                    {"cy", s.cy},
                                    {"ax", s.ax},
                                    {"ay", s.ay}}));
  }
  return make_object(
      {{"stations", std::move(stations)},
       {"angular", make_object({{"misclosure", have_mis ? Value(mis) : Value(nullptr)}, {"perAngle", per_angle}})},
       {"linear", make_object({{"dx", dx}, {"dy", dy}, {"dist", dist}, {"dir", base::js_atan2(dy, dx)}})},
       {"length", length},
       {"precision", dist > 1e-12 ? length / dist : std::numeric_limits<double>::infinity()},
       {"problems", std::move(problems)},
       {"start", start},
       {"close", close}});
}

std::string traverse_report(const Value& r, std::string_view units, const i18n::Catalog& w) {
  const std::string u = units == "m" ? "m" : "ft";
  auto q = [](const std::string& v) {
    if (!v.empty() && (digit(v[0]) || (v.size() > 1 && v[0] == '-' && digit(v[1])))) {
      return v;
    }
    if (!v.empty() && (v[0] == '=' || v[0] == '+' || v[0] == '-' || v[0] == '@' || v[0] == '\t' || v[0] == '\r')) {
      return "'" + v;
    }
    return v;
  };
  auto c = [](const std::string& v) {
    if (v.find_first_of("\",\r\n") == std::string::npos) {
      return v;
    }
    std::string o = "\"";
    for (const char ch : v) {
      if (ch == '"') {
        o += "\"\"";
      } else {
        o.push_back(ch);
      }
    }
    return o + "\"";
  };
  const Value ang = get(r, "angular");
  const Value mis = get(ang, "misclosure");
  const Value lin = get(r, "linear");
  const double precision = num(get(r, "precision"));
  const Value start = get(r, "start");
  std::vector<std::vector<std::string>> rows{
      {w.t("tv.r.angular"),
       mis.is_null() ? std::string() : std::string(num(mis) < 0 ? "-" : "") + dms(std::abs(num(mis)))},
      {w.t("tv.r.linear", {{"u", u}}), fixed(num(get(lin, "dist")), 4), bearing(num(get(lin, "dir")))},
      {w.t("tv.r.length", {{"u", u}}), fixed(num(get(r, "length")), 3)},
      {w.t("tv.r.precision"),
       std::isfinite(precision) ? "1 : " + appkit::num_str(base::js_round(precision)) : w.t("tv.r.exact")},
      {},
      {w.t("tv.r.station"), w.t("tv.r.dir"), w.t("tv.r.dist"), w.t("tv.r.n"), w.t("tv.r.e"), w.t("tv.r.cn"),
       w.t("tv.r.ce"), w.t("tv.r.an"), w.t("tv.r.ae")},
      {w.t("tv.r.start"), "", "", fixed(num(get(start, "y")), 4), fixed(num(get(start, "x")), 4), "0.0000", "0.0000",
       fixed(num(get(start, "y")), 4), fixed(num(get(start, "x")), 4)}};
  const Array& S = arr(get(r, "stations"));
  for (std::size_t i = 0; i < S.size(); ++i) {
    const Value& s = S[i];
    const Value nm = s.get("name");
    rows.push_back({nm.truthy() ? base::js_string(nm) : appkit::num_str(static_cast<double>(i + 1)),
                    bearing(num(s.get("dir"))), fixed(num(s.get("dist")), 3), fixed(num(s.get("y")), 4),
                    fixed(num(s.get("x")), 4), fixed(num(s.get("cy")), 4), fixed(num(s.get("cx")), 4),
                    fixed(num(s.get("ay")), 4), fixed(num(s.get("ax")), 4)});
  }
  std::string out;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (i != 0) {
      out += "\r\n";
    }
    for (std::size_t k = 0; k < rows[i].size(); ++k) {
      out += (k != 0 ? "," : "") + c(q(rows[i][k]));
    }
  }
  return out + "\r\n";
}

}  // namespace cc::civil
