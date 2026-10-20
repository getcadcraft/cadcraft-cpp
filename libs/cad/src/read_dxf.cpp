// SPDX-License-Identifier: Apache-2.0
// dxf.js readDxf (CadCraft 0.5) in C++: an ASCII DXF → the drawing doc.load() takes ({ entities, layers, blocks,
// layouts, dimStyle, meta, report, … }). LINE, CIRCLE, ARC, LWPOLYLINE, POLYLINE (2D), POINT, TEXT, MTEXT, ELLIPSE,
// SPLINE (as a polyline), SOLID / TRACE, HATCH, INSERT with ATTRIBs, ATTDEF in blocks, DIMENSION, XLINE / RAY, 3DFACE
// and VIEWPORT in layouts; multileaders, pictures and wipeouts through the drawing model the DWG import uses
// (dxf-model.js + importDwg, handed in as `read_model`). Anything else is counted in report.skipped.
#include "cc/cad/read_dxf.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/base/json.hpp"
#include "cc/cad/dims.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/import.hpp"
#include "parts.hpp"
#include "props.hpp"
#include "util.hpp"

namespace cc::cad {

namespace {

using namespace detail;  // NOLINT(google-build-using-namespace) — the reader is written against the shared helpers

using Pair = std::pair<int, std::string>;
using Group = std::vector<Pair>;

constexpr double kPi = std::numbers::pi;

double rad(double d) {
  return d * kPi / 180;
}

/// val(g, code): the first value of a code, trimmed, or nothing
std::optional<std::string> val(const Group& g, int code) {
  for (const auto& [c, v] : g) {
    if (c == code) {
      return std::string(base::js_trim(v));
    }
  }
  return std::nullopt;
}
double numv(const Group& g, int code, double d = 0) {
  const auto v = val(g, code);
  return v ? base::js_parse_float(*v) : d;
}
/// parseInt(val(g, code) ?? dflt, 10)
double int_of(const Group& g, int code, std::string_view dflt) {
  const auto v = val(g, code);
  return base::js_parse_int(v ? std::string_view(*v) : dflt);
}
/// an integer for bit tests (NaN as 0, as `x & n` does)
std::uint32_t bits(double v) {
  return static_cast<std::uint32_t>(base::js_to_int32(v));
}
bool upper_is(std::string_view s, std::string_view upper) {
  return base::js_to_upper(s) == upper;
}
std::string true_color(std::string_view v) {
  const auto n = static_cast<std::uint32_t>(base::js_to_int32(base::js_parse_int(v))) & 0xffffffU;
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string s = "#";
  for (int k = 20; k >= 0; k -= 4) {
    s += kHex[(n >> static_cast<unsigned>(k)) & 0xfU];
  }
  return s;
}

/// One UTF-16 unit as WTF-8 (a lone surrogate kept as it is, as String.fromCharCode leaves it)
void put_wtf8(std::string& o, std::uint32_t c) {
  if (c < 0x80) {
    o += static_cast<char>(c);
  } else if (c < 0x800) {
    o += static_cast<char>(0xC0U | (c >> 6U));
    o += static_cast<char>(0x80U | (c & 0x3FU));
  } else {
    o += static_cast<char>(0xE0U | (c >> 12U));
    o += static_cast<char>(0x80U | ((c >> 6U) & 0x3FU));
    o += static_cast<char>(0x80U | (c & 0x3FU));
  }
}

/// decodeText: %%c ⌀, %%d °, %%p ±, %%u / %%o / %%k dropped, %%% → %, \U+XXXX
std::string decode_text(std::string_view s) {
  std::string o;
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '%' && i + 2 < s.size() && s[i + 1] == '%') {
      const char c = s[i + 2];
      if (c == 'c' || c == 'C') {
        o += "\xE2\x8C\x80";
        i += 3;
        continue;
      }
      if (c == 'd' || c == 'D') {
        o += "\xC2\xB0";
        i += 3;
        continue;
      }
      if (c == 'p' || c == 'P') {
        o += "\xC2\xB1";
        i += 3;
        continue;
      }
      if (c == 'u' || c == 'U' || c == 'o' || c == 'O' || c == 'k' || c == 'K') {
        i += 3;
        continue;
      }
      if (c == '%') {
        o += '%';
        i += 3;
        continue;
      }
    }
    o += s[i++];
  }
  // \U+XXXX (one UTF-16 unit each, as String.fromCharCode)
  std::string r;
  for (std::size_t i = 0; i < o.size();) {
    if (o[i] == '\\' && i + 6 < o.size() + 0 && o.compare(i, 3, "\\U+") == 0) {
      bool hex = true;
      for (std::size_t k = i + 3; k < i + 7; ++k) {
        hex = hex && std::isxdigit(static_cast<unsigned char>(o[k])) != 0;
      }
      if (hex) {
        std::uint32_t cp = 0;
        for (std::size_t k = i + 3; k < i + 7; ++k) {
          const auto ch = static_cast<unsigned char>(o[k]);
          cp = cp * 16 + static_cast<std::uint32_t>(ch <= '9' ? ch - '0' : (ch | 0x20U) - 'a' + 10);
        }
        put_wtf8(r, cp);
        i += 7;
        continue;
      }
    }
    r += o[i++];
  }
  return r;
}

/// XDATA by application: { ACAD: [[code, value], …], CADCRAFT: … } (values trimmed)
std::map<std::string, std::vector<Pair>> xdata(const Group& g) {
  std::map<std::string, std::vector<Pair>> x;
  std::string app;
  bool have = false;
  for (const auto& [c, v] : g) {
    if (c == 1001) {
      app = base::js_to_upper(base::js_trim(v));
      x[app] = {};
      have = true;
    } else if (have && c >= 1000) {
      x[app].emplace_back(c, std::string(base::js_trim(v)));
    }
  }
  return x;
}
/// How many times `for (let j = 0; j < n; j++)` runs, capped (a count from a file is hostile).
std::size_t times(double n, std::size_t cap) {
  if (!(n > 0)) {
    return 0;
  }
  return n >= static_cast<double>(cap) ? cap : static_cast<std::size_t>(std::ceil(n));
}

std::vector<Pair> app_of(const std::map<std::string, std::vector<Pair>>& x, const std::string& app) {
  const auto it = x.find(app);
  return it == x.end() ? std::vector<Pair>{} : it->second;
}
bool has_xdata(const Group& g) {
  return std::ranges::any_of(g, [](const Pair& p) { return p.first == 1001; });
}
/// xlist(list, name): the list inside an ACAD "<name> { … }" section, or nothing
std::optional<std::vector<Pair>> xlist(const std::vector<Pair>& list, std::string_view name) {
  std::size_t k = 0;
  const auto is_name = [&](std::size_t q) { return list[q].first == 1000 && list[q].second == name; };
  while (k < list.size() && !is_name(k)) {
    ++k;
  }
  if (k >= list.size()) {
    return std::nullopt;
  }
  std::vector<Pair> outv;
  int depth = 0;
  for (std::size_t j = k + 1; j < list.size(); ++j) {
    const auto& [c, v] = list[j];
    if (c == 1002) {
      depth += v == "{" ? 1 : -1;
      if (depth == 0) {
        break;
      }
      if (v == "{" && depth == 1) {
        continue;
      }
    }
    outv.emplace_back(c, v);
  }
  return outv;
}
/// The xdata as the drawing model keeps it ({ APP: [[code, value]…] }), for propsFromXdata
Value xdata_value(const std::map<std::string, std::vector<Pair>>& x) {
  Value o = Value::object();
  for (const auto& [app, list] : x) {
    base::Array a;
    for (const auto& [c, v] : list) {
      a.emplace_back(base::Array{static_cast<double>(c), v});
    }
    o.set(app, Value(std::move(a)));
  }
  return o;
}

/// /\n\s*0\r?\n(MULTILEADER|IMAGE|WIPEOUT)\r?\n/ in the text
bool has_extras(std::string_view text) {
  for (std::size_t i = text.find('\n'); i != std::string_view::npos; i = text.find('\n', i + 1)) {
    std::size_t k = i + 1;
    while (k < text.size() && (text[k] == ' ' || text[k] == '\t' || text[k] == '\r' || text[k] == '\n' ||
                               text[k] == '\f' || text[k] == '\v')) {
      ++k;
    }
    // \s* is greedy and may give back a \n it took: the 0 can follow any of the newlines skipped
    for (std::size_t z = i + 1; z <= k && z < text.size(); ++z) {
      if (text[z] != '0' || (z > i + 1 && text[z - 1] != ' ' && text[z - 1] != '\t' && text[z - 1] != '\r' &&
                             text[z - 1] != '\n' && text[z - 1] != '\f' && text[z - 1] != '\v')) {
        continue;
      }
      std::size_t q = z + 1;
      if (q < text.size() && text[q] == '\r') {
        ++q;
      }
      if (q >= text.size() || text[q] != '\n') {
        continue;
      }
      ++q;
      for (const std::string_view w : {"MULTILEADER", "IMAGE", "WIPEOUT"}) {
        if (text.substr(q, w.size()) == w) {
          std::size_t e = q + w.size();
          if (e < text.size() && text[e] == '\r') {
            ++e;
          }
          if (e < text.size() && text[e] == '\n') {
            return true;
          }
        }
      }
    }
  }
  return false;
}

void skip(Value& rep, const std::string& t) {
  Value sk = rep.get("skipped");
  sk.set(t, num(sk.get(t)) > 0 ? num(sk.get(t)) + 1 : 1);
  rep.set("skipped", sk);
}
void bump(Value& rep, std::string_view k) {
  rep.set(k, num(rep.get(k)) > 0 ? num(rep.get(k)) + 1 : 1);
}

/// fixStyle(st): a dimension style made usable
Value& fix_style(Value& st) {
  if (!(num(st.get("scale")) > 0)) {
    st.set("scale", 1);
  }
  if (!st.get("unit").is_undefined()) {
    const double u = base::js_round(num(st.get("unit")));
    if (st.get("lunit").is_undefined()) {
      st.set("lunit", u == 6 ? 4 : u == 7 ? 5 : u);  // NOLINT(readability-avoid-nested-conditional-operator)
    }
    st.erase("unit");
  }
  if (!st.get("lunit").is_undefined()) {
    const double u = base::js_round(num(st.get("lunit")));
    if (u >= 1 && u <= 5) {
      st.set("lunit", u);
    } else if (u == 6) {
      st.set("lunit", 2);
    } else {
      st.erase("lunit");
    }
  }
  for (const char* k : {"dec", "adec", "tad", "zin"}) {
    const Value v = st.get(k);
    st.set(k, std::max(0.0, base::js_round(v.truthy() ? num(v) : 0)));
  }
  if (num(st.get("dec")) > 8) {
    st.set("dec", 8);
  }
  return st;
}

/// common(g): layer, colour, linetype, lineweight, linetype scale, transparency, thickness
Value common(const Group& g) {
  const auto tc = val(g, 420);
  const auto aci = val(g, 62);
  Value color(nullptr);
  if (tc) {
    color = Value(true_color(*tc));
  } else if (aci) {
    const double n = base::js_parse_int(*aci);
    color = n == 256 ? Value(nullptr) : Value(std::abs(n));
  }
  const auto layer = val(g, 8);
  Value o = base::make_object({{"layer", layer && !layer->empty() ? *layer : std::string("0")}, {"color", color}});
  if (const auto lt = val(g, 6); lt && !lt->empty() && !upper_is(*lt, "BYLAYER")) {
    if (upper_is(*lt, "BYBLOCK")) {
      o.set("lt", "ByBlock");
    } else if (upper_is(*lt, "CONTINUOUS")) {
      o.set("lt", "Continuous");
    } else {
      o.set("lt", *lt);
    }
  }
  if (const auto lw = val(g, 370)) {
    const double n = base::js_parse_int(*lw);
    if (n == -2 || n == -3) {
      o.set("lw", n);
    } else if (n >= 0 && n <= 211) {
      o.set("lw", n / 100);
    }
  }
  const auto ls = val(g, 48);
  const double s = ls ? base::js_parse_float(*ls) : std::nan("");
  if (s > 0 && std::abs(s - 1) > 1e-12) {
    o.set("lts", s);
  }
  const auto w440 = val(g, 440);
  const Value tr = tr_from(w440 ? Value(*w440) : Value());
  if (!tr.is_undefined()) {
    o.set("tr", tr);
  }
  const double th = numv(g, 39);
  if (th != 0 && !std::isnan(th)) {
    o.set("th", th);
  }
  return o;
}
Value with(Value base, std::initializer_list<std::pair<std::string_view, Value>> kv) {
  for (const auto& [k, v] : kv) {
    base.set(k, v);
  }
  return base;
}

// ---- hatches, splines

void arc_into(std::vector<double>& ring, double px, double py, double qx, double qy, double b) {
  const double theta = 4 * base::js_atan(b);
  const double steps = std::max(2.0, std::ceil(16 * std::abs(theta) / kPi));
  const double c = base::js_hypot(qx - px, qy - py);
  const double r = std::abs(c / (2 * base::js_sin(theta / 2)));
  const double mx = (px + qx) / 2;
  const double my = (py + qy) / 2;
  const double h = r * base::js_cos(theta / 2) * (b > 0 ? 1 : -1);
  const double ux = (qx - px) / c;
  const double uy = (qy - py) / c;
  const double cx = mx - uy * h;
  const double cy = my + ux * h;
  const double a0 = base::js_atan2(py - cy, px - cx);
  const std::size_t ns = times(steps, 1U << 16U);
  for (std::size_t si = 1; si < ns; ++si) {
    const double a = a0 + theta * static_cast<double>(si) / steps;
    ring.push_back(cx + r * base::js_cos(a));
    ring.push_back(cy + r * base::js_sin(a));
  }
}

base::Array hatch_loops(const Group& g, Value& rep, const i18n::Catalog& words) {
  std::size_t k = 0;
  while (k < g.size() && g[k].first != 91) {
    ++k;
  }
  base::Array loops;
  if (k >= g.size()) {
    return loops;
  }
  const double n = base::js_parse_int(g[k].second);
  ++k;
  const auto next = [&](int code) {
    while (k < g.size() && g[k].first != code) {
      ++k;
    }
    return k < g.size() ? base::js_parse_float(g[k++].second) : std::nan("");
  };
  for (std::size_t p = 0, np = times(n, g.size() + 1); p < np && k < g.size(); ++p) {
    const double flags = next(92);
    std::vector<double> ring;
    if ((bits(flags) & 2U) != 0) {
      const bool has_bulge = next(72) == 1;
      const bool closed = next(73) == 1;
      const double cnt = next(93);
      struct V {
        double x, y, b;
      };
      std::vector<V> vs;
      for (std::size_t j = 0, nj = times(cnt, g.size() + 1); j < nj; ++j) {
        const double x = next(10);
        const double y = next(20);
        const double b = has_bulge ? next(42) : 0;
        vs.push_back({x, y, b});
      }
      for (std::size_t j = 0; j < vs.size(); ++j) {
        const V& a = vs[j];
        const V& b2 = vs[(j + 1) % vs.size()];
        ring.push_back(a.x);
        ring.push_back(a.y);
        if (std::abs(a.b) > 1e-12 && (closed || j + 1 < vs.size())) {
          arc_into(ring, a.x, a.y, b2.x, b2.y, a.b);
        }
      }
      if (!vs.empty()) {
        ring.push_back(vs[0].x);
        ring.push_back(vs[0].y);
      }
    } else {
      const double edges = next(93);
      bool ok = true;
      for (std::size_t j = 0, nj = times(edges, g.size() + 1); j < nj; ++j) {
        const double t = next(72);
        if (t == 1) {
          const double x1 = next(10);
          const double y1 = next(20);
          const double x2 = next(11);
          const double y2 = next(21);
          ring.insert(ring.end(), {x1, y1, x2, y2});
        } else if (t == 2) {
          const double cx = next(10);
          const double cy = next(20);
          const double r = next(40);
          const double s = rad(next(50));
          const double e = rad(next(51));
          const bool ccw = next(73) == 1;
          double a0 = s;
          double a1 = e;
          if (!ccw) {
            a0 = -s;
            a1 = -e;
          }
          if (a1 <= a0) {
            a1 += kTau;
          }
          const double steps = std::max(4.0, std::ceil(32 * (a1 - a0) / kTau));
          const std::size_t nq = times(steps, 1U << 16U);
          for (std::size_t q = 0; q <= nq; ++q) {
            const double a = a0 + (a1 - a0) * static_cast<double>(q) / steps;
            ring.push_back(cx + r * base::js_cos(a));
            ring.push_back(cy + r * base::js_sin(a));
          }
        } else {
          ok = false;  // ellipse / spline edges: not yet
          break;
        }
      }
      if (!ok) {
        skip(rep, words.t("dxf.hatch_boundary_ellipse_spline"));
        continue;
      }
    }
    if (ring.size() >= 6) {
      base::Array a;
      a.reserve(ring.size());
      for (const double v : ring) {
        a.emplace_back(v);
      }
      loops.emplace_back(std::move(a));
    }
  }
  return loops;
}

/// hatchPattern(g): a pattern hatch's scale, angle and its own pattern lines
Value hatch_pattern(const Group& g) {
  Value out = Value::object();
  const double sc = numv(g, 41, 1);
  const double an = numv(g, 52, 0);
  if (sc > 0 && sc != 1) {
    out.set("pscale", sc);
  }
  if (an != 0 && !std::isnan(an)) {
    out.set("pangle", norm_angle(rad(an)));
  }
  std::size_t k = 0;
  while (k < g.size() && g[k].first != 78) {
    ++k;
  }
  if (k >= g.size()) {
    return out;
  }
  struct L {
    double angle = 0, x = 0, y = 0, ox = 0, oy = 0;
    std::vector<double> dashes;
  };
  std::vector<L> lines;
  bool cur = false;
  for (std::size_t j = k + 1; j < g.size(); ++j) {
    const auto& [c, v] = g[j];
    const double x = base::js_parse_float(v);
    if (c == 53) {
      lines.push_back({.angle = x, .x = 0, .y = 0, .ox = 0, .oy = 0, .dashes = {}});
      cur = true;
    } else if (!cur) {  // NOLINT(bugprone-branch-clone) — the JS's order: a code before the first 53 ends it too
      break;
    } else if (c == 43) {
      lines.back().x = x;
    } else if (c == 44) {
      lines.back().y = x;
    } else if (c == 45) {
      lines.back().ox = x;
    } else if (c == 46) {
      lines.back().oy = x;
    } else if (c == 49) {
      lines.back().dashes.push_back(x);
    } else if (c != 79) {
      break;
    }
  }
  std::vector<L> ok;
  for (const L& l : lines) {
    bool fin = std::isfinite(l.angle) && std::isfinite(l.x) && std::isfinite(l.y) && std::isfinite(l.ox) &&
               std::isfinite(l.oy);
    for (const double d : l.dashes) {
      fin = fin && std::isfinite(d);
    }
    if (fin) {
      ok.push_back(l);
    }
  }
  if (!ok.empty() && ok.size() <= 64) {
    base::Array pl;
    for (const L& l : ok) {
      const double a = l.angle * kPi / 180;
      const double ca = base::js_cos(a);
      const double sa = base::js_sin(a);
      base::Array d;
      for (std::size_t i = 0; i < l.dashes.size() && i < 32; ++i) {
        d.emplace_back(l.dashes[i]);
      }
      pl.emplace_back(base::make_object({{"angle", l.angle},
                                         {"x", l.x},
                                         {"y", l.y},
                                         {"dx", l.ox * ca + l.oy * sa},
                                         {"dy", -l.ox * sa + l.oy * ca},
                                         {"dashes", std::move(d)}}));
    }
    out.set("patlines", Value(std::move(pl)));
  }
  return out;
}

struct XY {
  double x = 0, y = 0;
};
std::vector<XY> de_boor_sample(const std::vector<XY>& p, const std::vector<double>& u, int deg,
                               const std::vector<double>& w) {
  const std::size_t n = p.size();
  const auto pd = static_cast<std::size_t>(deg);
  std::vector<XY> out;
  const double lo = u[pd];
  const double hi = u[n];
  const double steps = std::min(400.0, static_cast<double>(n) * 8);
  const auto ns = static_cast<std::size_t>(steps);
  for (std::size_t si = 0; si <= ns; ++si) {
    const auto sd = static_cast<double>(si);
    const double t = sd == steps ? hi - 1e-12 * std::abs(hi != 0 ? hi : 1) : lo + (hi - lo) * sd / steps;
    std::size_t k = pd;
    while (k < n - 1 && u[k + 1] <= t) {
      ++k;
    }
    struct D {
      double x, y, w;
    };
    std::vector<D> d;
    for (std::size_t j = 0; j <= pd; ++j) {
      const std::size_t idx = k - pd + j;
      const double wt = idx < w.size() ? w[idx] : 1;
      d.push_back({p[idx].x * wt, p[idx].y * wt, wt});
    }
    for (std::size_t r = 1; r <= pd; ++r) {
      for (std::size_t j = pd; j >= r; --j) {
        const std::size_t i = k - pd + j;
        const double den = u[i + pd - r + 1] - u[i];
        const double a = den != 0 ? (t - u[i]) / den : 0;
        d[j] = {(1 - a) * d[j - 1].x + a * d[j].x, (1 - a) * d[j - 1].y + a * d[j].y,
                (1 - a) * d[j - 1].w + a * d[j].w};
        if (j == 0) {
          break;
        }
      }
    }
    out.push_back({d[pd].x / d[pd].w, d[pd].y / d[pd].w});
  }
  return out;
}
std::vector<XY> spline_points(const Group& g) {
  std::vector<XY> ctrl;
  std::vector<XY> fit;
  std::vector<double> knots;
  std::vector<double> weights;
  for (const auto& [c, v] : g) {
    if (c == 10) {
      ctrl.push_back({base::js_parse_float(v), 0});
    } else if (c == 20 && !ctrl.empty()) {
      ctrl.back().y = base::js_parse_float(v);
    } else if (c == 11) {
      fit.push_back({base::js_parse_float(v), 0});
    } else if (c == 21 && !fit.empty()) {
      fit.back().y = base::js_parse_float(v);
    } else if (c == 40) {
      knots.push_back(base::js_parse_float(v));
    } else if (c == 41) {
      weights.push_back(base::js_parse_float(v));
    }
  }
  const double deg = int_of(g, 71, "3");
  if (deg >= 0 && deg < 64 && static_cast<double>(ctrl.size()) > deg &&
      static_cast<double>(knots.size()) == static_cast<double>(ctrl.size()) + deg + 1) {
    return de_boor_sample(ctrl, knots, static_cast<int>(deg), weights);
  }
  return fit.size() >= 2 ? fit : ctrl;
}

/// dimUsable(d): finite points, something to measure
bool dim_usable(const Value& d) {
  for (const char* k : {"p1", "p2", "c", "loc"}) {
    const Value p = d.get(k);
    if (p.truthy() && (!std::isfinite(num(p.get("x"))) || !std::isfinite(num(p.get("y"))))) {
      return false;
    }
  }
  const auto far = [](const Value& a, const Value& b) {
    return base::js_hypot(num(b.get("x")) - num(a.get("x")), num(b.get("y")) - num(a.get("y"))) > 1e-12;
  };
  const std::string kind = str(d.get("kind"));
  if (kind == "linear" || kind == "aligned") {
    const Value rot = d.get("rot");
    return far(d.get("p1"), d.get("p2")) && std::isfinite(nullish(rot) ? 0 : num(rot));
  }
  if (kind == "radius" || kind == "diameter") {
    return far(d.get("c"), d.get("p1"));
  }
  return far(d.get("c"), d.get("p1")) && far(d.get("c"), d.get("p2")) && far(d.get("c"), d.get("loc"));
}

struct Follow {
  std::string type;
  Group g;
};

/// model3d.js read3dText(ents): CadCraft's 3D JSON from the CADCRAFT_3D block's points, or null
Value read3d_text(const base::Array& ents) {
  std::vector<std::pair<double, std::string>> parts;
  for (const Value& e : ents) {
    const Value cc = e.get("cc");
    if (!cc.is_array() || cc.as_array().empty()) {
      continue;
    }
    const std::string s = str(cc.as_array().front());
    if (!s.starts_with("CC3D:")) {
      continue;
    }
    const std::size_t colon = s.find(':', 5);
    if (colon == std::string::npos || colon == 5 || colon - 5 > 7) {
      continue;
    }
    const std::string digits = s.substr(5, colon - 5);
    if (!std::ranges::all_of(digits, [](char ch) { return ch >= '0' && ch <= '9'; })) {
      continue;
    }
    parts.emplace_back(base::js_number(digits), s.substr(colon + 1));
  }
  if (parts.empty()) {
    return {nullptr};
  }
  std::ranges::stable_sort(parts, [](const auto& a, const auto& b) { return a.first < b.first; });
  std::string out;
  for (const auto& [n, t] : parts) {
    out += t;
  }
  return {out};
}
/// model3d.js read3dView(ents): the 3D tab's saved camera (CCCAM:<json>), or null
Value read3d_view(const base::Array& ents) {
  for (const Value& e : ents) {
    const Value cc = e.get("cc");
    const std::string s = cc.is_array() && !cc.as_array().empty() ? str(cc.as_array().front()) : std::string();
    if (s.starts_with("CCCAM:") && base::utf16_length(s) < 2000) {
      const auto parsed = base::parse_json(std::string_view(s).substr(6));
      return parsed ? clean_view(*parsed) : Value(nullptr);
    }
  }
  return {nullptr};
}

/// layoutFromBlock(ents): a CCLAYOUT_n block (CadCraft's extra layouts in R12) → a layout, or null
Value layout_from_block(const base::Array& ents) {
  const Value* mark = nullptr;
  for (const Value& e : ents) {
    const Value cc = e.get("cc");
    if (cc.is_array() && !cc.as_array().empty() && cc.as_array().front() == Value("LAYOUT")) {
      mark = &e;
      break;
    }
  }
  if (mark == nullptr) {
    return {nullptr};
  }
  const base::Array& mc = mark->get("cc").as_array();
  const auto at = [&mc](std::size_t k) { return k < mc.size() ? mc[k] : Value(); };
  const Value name = at(1);
  Value paper = base::make_object({{"unit", at(2) == Value("in") ? "in" : "mm"},
                                   {"key", at(3).truthy() ? at(3) : Value()},
                                   {"x0", base::js_to_number(at(4))},
                                   {"y0", base::js_to_number(at(5))},
                                   {"w", base::js_to_number(at(6))},
                                   {"h", base::js_to_number(at(7))}});
  for (const char* k : {"x0", "y0"}) {
    const double v = num(paper.get(k));
    paper.set(k, v != 0 && !std::isnan(v) ? v : 0);
  }
  base::Array list;
  for (const Value& e : ents) {
    if (&e == mark) {
      continue;
    }
    const Value cc = e.get("cc");
    if (cc.is_array() && !cc.as_array().empty() && cc.as_array().front() == Value("VIEWPORT") &&
        e.get("type") == Value("poly") && e.get("pts").as_array().size() >= 4) {
      double x1 = std::numeric_limits<double>::infinity();
      double y1 = x1;
      double x2 = -x1;
      double y2 = -x1;
      for (const Value& p : e.get("pts").as_array()) {
        x1 = js_min(x1, num(p.get("x")));
        x2 = js_max(x2, num(p.get("x")));
        y1 = js_min(y1, num(p.get("y")));
        y2 = js_max(y2, num(p.get("y")));
      }
      const base::Array& c = cc.as_array();
      const auto ca = [&c](std::size_t k) { return k < c.size() ? c[k] : Value(); };
      base::Array frz;
      if (ca(4) == Value("FRZ")) {
        for (std::size_t k = 5; k < c.size(); ++k) {
          if (c[k].truthy()) {
            frz.push_back(c[k]);
          }
        }
      }
      Value v = base::make_object({{"type", "viewport"},
                                   {"layer", e.get("layer")},
                                   {"color", e.get("color")},
                                   {"x", (x1 + x2) / 2},
                                   {"y", (y1 + y2) / 2},
                                   {"w", x2 - x1},
                                   {"h", y2 - y1},
                                   {"cx", base::js_to_number(ca(1))},
                                   {"cy", base::js_to_number(ca(2))},
                                   {"s", base::js_to_number(ca(3))}});
      if (!frz.empty()) {
        v.set("frz", Value(std::move(frz)));
      }
      list.push_back(std::move(v));
      continue;
    }
    Value c = e;
    c.erase("cc");
    list.push_back(std::move(c));
  }
  return base::make_object({{"name", decode_text(name.truthy() ? str(name) : std::string("Layout"))},
                            {"paper", std::move(paper)},
                            {"entities", Value(std::move(list))}});
}

class DxfReader {
 public:
  DxfReader(std::vector<Pair> pairs, const i18n::Catalog& words) : pairs_(std::move(pairs)), words_(words) {}
  Value run(std::string_view text, const std::function<base::Result<Value>(std::string_view)>& read_model);

 private:
  std::vector<Pair> pairs_;
  const i18n::Catalog& words_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) — lives for one read
  std::size_t i_ = 0;
  Value out_;
  std::map<std::string, std::map<int, std::string>> header_;
  std::map<std::string, Value> dim_styles_;           // name (upper case) → style
  std::map<std::string, std::string> block_records_;  // handle → block name
  std::map<std::string, std::string> style_handles_;  // STYLE handle → name
  struct LayoutObj {
    std::string name;
    double tab = 0;
    std::string record;
    XY min, max;
    double paper_w = 0, paper_h = 0, units = -1, rot = 0;
  };
  std::vector<LayoutObj> layout_objs_;
  std::vector<std::pair<std::string, base::Array>> paper_blocks_;  // *Paper_SpaceN (upper case) → raw entities
  base::Array paper_raw_;
  std::set<std::string> no_plot_;
  std::map<std::string, std::string> layer_by_h_;
  std::map<std::string, std::map<std::string, std::string>> dict_by_h_;
  std::map<std::string, Value> filter_by_h_;
  base::Array vports_;  // out._vports

  [[nodiscard]] const Pair& at() const {
    static const Pair kEof{0, "EOF"};
    return i_ < pairs_.size() ? pairs_[i_] : kEof;
  }
  [[nodiscard]] bool at0(std::string_view s) const { return at().first == 0 && base::js_trim(at().second) == s; }
  Group group() {
    Group g;
    while (i_ < pairs_.size() && at().first != 0) {
      g.push_back(pairs_[i_]);
      ++i_;
    }
    return g;
  }
  Value& report() { return *out_.find("report"); }

  void skip_section() {
    while (i_ < pairs_.size() && !at0("ENDSEC")) {
      ++i_;
    }
    ++i_;
  }
  void read_header();
  void read_tables();
  void read_blocks();
  void read_objects();
  void resolve_clips();
  void build_layouts();
  void merge_extras(std::string_view text, const std::function<base::Result<Value>(std::string_view)>& read_model);
  struct Lists {
    base::Array model, paper;
  };
  Lists read_entity_list(Value* rep_in, bool top, bool in_block);
  std::vector<Follow> read_followers();
  Value entity_from(const std::string& type, const Group& g, const std::vector<Follow>* follow, Value& rep);
  Value entity_from0(const std::string& type, const Group& g, const std::vector<Follow>* follow, Value& rep);
  Value layout_from(const std::string& name, base::Array list, const std::optional<std::pair<XY, XY>>& lim,
                    const std::string& unit);
};

void DxfReader::read_header() {
  std::string var_name;
  Value& meta = *out_.find("meta");
  Value& dim_style = *out_.find("dimStyle");
  static const std::map<std::string, std::string, std::less<>> kHeaderDimvars{
      {"$DIMSCALE", "scale"}, {"$DIMASZ", "asz"},     {"$DIMEXO", "exo"},  {"$DIMDLI", "dli"}, {"$DIMEXE", "exe"},
      {"$DIMTXT", "txt"},     {"$DIMGAP", "gap"},     {"$DIMTAD", "tad"},  {"$DIMZIN", "zin"}, {"$DIMDEC", "dec"},
      {"$DIMADEC", "adec"},   {"$DIMLUNIT", "lunit"}, {"$DIMUNIT", "unit"}};
  static const std::map<double, std::string> kUnits{
      {0, ""},       {1, "inches"},     {2, "feet"},        {3, "miles"}, {4, "millimeters"}, {5, "centimeters"},
      {6, "meters"}, {7, "kilometers"}, {8, "microinches"}, {9, "mils"},  {10, "yards"}};
  while (i_ < pairs_.size() && !at0("ENDSEC")) {
    const auto& [c, v] = at();
    if (c == 9) {
      var_name = std::string(base::js_trim(v));
    } else {
      if (var_name == "$ACADVER" && c == 1) {
        meta.set("version", std::string(base::js_trim(v)));
      } else if (var_name == "$INSUNITS" && c == 70) {
        const auto it = kUnits.find(base::js_number(v));
        meta.set("units", it != kUnits.end() ? it->second : std::string());
      } else if (var_name == "$LTSCALE" && c == 40 && base::js_parse_float(v) > 0 && base::js_parse_float(v) != 1) {
        meta.set("ltscale", base::js_parse_float(v));
      } else if (var_name == "$LUNITS" && c == 70) {
        const double u = base::js_number(v);
        if (u >= 1 && u <= 5 && u != 2) {
          meta.set("lunits", u);
        }
      } else if (const auto it = kHeaderDimvars.find(var_name); it != kHeaderDimvars.end()) {
        dim_style.set(it->second, base::js_parse_float(v));
      }
      header_[var_name][c] = std::string(base::js_trim(v));
    }
    ++i_;
  }
  ++i_;
  fix_style(dim_style);
}

void DxfReader::read_tables() {
  Value& out = out_;
  while (i_ < pairs_.size() && !at0("ENDSEC")) {
    const std::string t = at().first == 0 ? std::string(base::js_trim(at().second)) : std::string();
    if (t == "LAYER") {
      ++i_;
      const Group g = group();
      const auto name = val(g, 2);
      if (name && !name->empty()) {
        const double col = int_of(g, 62, "7");
        const std::uint32_t flags = bits(int_of(g, 70, "0"));
        const auto tc = val(g, 420);
        const double lw = int_of(g, 370, "-3");
        if (const auto lh = val(g, 5); lh && !lh->empty()) {
          layer_by_h_[base::js_to_upper(*lh)] = *name;
        }
        Value l = base::make_object({{"name", *name}});
        l.set("color", tc ? Value(true_color(*tc)) : Value(std::abs(col) != 0 && !std::isnan(col) ? std::abs(col) : 7));
        l.set("on", col >= 0);
        l.set("frozen", (flags & 1U) != 0);
        l.set("locked", (flags & 4U) != 0);
        if ((flags & 2U) != 0) {
          l.set("newVpFrozen", true);
        }
        l.set("plot", val(g, 290) != std::optional<std::string>("0"));
        const auto lt = val(g, 6);
        l.set("linetype", lt && !lt->empty() ? *lt : std::string("Continuous"));
        l.set("lineweight", lw >= 0 && lw <= 211 ? lw / 100 : -3);
        out.find("layers")->as_array().push_back(std::move(l));
      }
    } else if (t == "DIMSTYLE") {
      ++i_;
      const Group g = group();
      const auto name = val(g, 2);
      if (name && !name->empty()) {
        Value st = out.get("dimStyle");
        static const std::array<std::pair<int, const char*>, 13> kCodes{{{40, "scale"},
                                                                         {41, "asz"},
                                                                         {42, "exo"},
                                                                         {43, "dli"},
                                                                         {44, "exe"},
                                                                         {140, "txt"},
                                                                         {147, "gap"},
                                                                         {77, "tad"},
                                                                         {78, "zin"},
                                                                         {271, "dec"},
                                                                         {179, "adec"},
                                                                         {277, "lunit"},
                                                                         {270, "unit"}}};
        for (const auto& [code, key] : kCodes) {
          if (const auto x = val(g, code)) {
            st.set(key, base::js_parse_float(*x));
          }
        }
        st_from_codes(
            [&g](double c) -> Value {
              const int code = static_cast<int>(c);
              if (code == 3 || code == 4) {  // the raw value (not trimmed)
                for (const auto& [k, v] : g) {
                  if (k == code) {
                    return {v};
                  }
                }
                return {};
              }
              const auto v = val(g, code);
              return v ? Value(*v) : Value();
            },
            st);
        if (has_xdata(g)) {
          std::vector<std::string> strs;
          for (const auto& [c, v] : app_of(xdata(g), "CADCRAFT")) {
            strs.push_back(decode_text(v));
          }
          cc_dim_from(strs, st);
        }
        if (const auto h = val(g, 340)) {
          const auto it = style_handles_.find(base::js_to_upper(*h));
          if (it != style_handles_.end() && !it->second.empty() && !upper_is(it->second, "STANDARD")) {
            st.set("txsty", it->second);
          }
        }
        fix_style(st);
        dim_styles_[base::js_to_upper(*name)] = st;
        if (!upper_is(*name, "STANDARD") && (bits(int_of(g, 70, "0")) & 16U) == 0) {
          if (!out.get("dimStyles").is_array()) {
            out.set("dimStyles", Value::array());
          }
          out.find("dimStyles")->as_array().emplace_back(base::Array{*name, st});
        }
      }
    } else if (t == "STYLE") {
      ++i_;
      const Group g = group();
      const auto name = val(g, 2);
      const std::uint32_t fl = bits(int_of(g, 70, "0"));
      if (const auto h = val(g, 5); h && !h->empty()) {
        style_handles_[base::js_to_upper(*h)] = name.value_or("");
      }
      if (name && !name->empty() && (fl & 1U) == 0 && (fl & 16U) == 0 && !upper_is(*name, "STANDARD")) {
        if (!out.get("textStyles").is_array()) {
          out.set("textStyles", Value::array());
        }
        const double wf = numv(g, 41, 1);
        out.find("textStyles")
            ->as_array()
            .emplace_back(base::Array{*name, base::make_object({{"font", val(g, 3).value_or("")},
                                                                {"h", numv(g, 40)},
                                                                {"wf", wf != 0 && !std::isnan(wf) ? wf : 1},
                                                                {"obl", rad(numv(g, 50))}})});
      }
    } else if (t == "LTYPE") {
      ++i_;
      const Group g = group();
      const auto name = val(g, 2);
      base::Array pattern;
      for (const auto& [c, v] : g) {
        if (c == 49 && std::isfinite(base::js_parse_float(v))) {
          pattern.emplace_back(base::js_parse_float(v));
        }
      }
      if (name && !name->empty() && !pattern.empty() && !upper_is(*name, "CONTINUOUS") && !upper_is(*name, "BYLAYER") &&
          !upper_is(*name, "BYBLOCK")) {
        if (!out.get("linetypes").is_array()) {
          out.set("linetypes", Value::array());
        }
        out.find("linetypes")
            ->as_array()
            .emplace_back(base::make_object(
                {{"name", *name}, {"description", val(g, 3).value_or("")}, {"pattern", std::move(pattern)}}));
      }
    } else if (t == "VIEW") {
      ++i_;
      const Group g = group();
      const auto name = val(g, 2);
      const double w = numv(g, 41);
      const double h = numv(g, 40);
      if (name && !name->empty() && w > 0 && h > 0) {
        if (!out.get("views").is_array()) {
          out.set("views", Value::array());
        }
        out.find("views")->as_array().emplace_back(
            base::make_object({{"name", *name},
                               {"cx", numv(g, 10) + numv(g, 12)},
                               {"cy", numv(g, 20) + numv(g, 22)},
                               {"w", w},
                               {"h", h},
                               {"paper", (bits(int_of(g, 70, "0")) & 1U) != 0}}));
      }
    } else if (t == "VPORT") {
      ++i_;
      const Group g = group();
      if (upper_is(val(g, 2).value_or(""), "*ACTIVE")) {
        std::optional<std::string> sheet;
        for (const auto& [c, v] : app_of(xdata(g), "CADCRAFT")) {
          const std::string d = decode_text(v);
          if (!sheet && d.starts_with("PANESHEET:")) {
            sheet = d.substr(10);
          }
        }
        Value r = base::make_object({{"x0", numv(g, 10)},
                                     {"y0", numv(g, 20)},
                                     {"x1", numv(g, 11)},
                                     {"y1", numv(g, 21)},
                                     {"cx", numv(g, 12)},
                                     {"cy", numv(g, 22)},
                                     {"h", numv(g, 40)},
                                     {"tw", rad(numv(g, 51))}});
        if (sheet) {
          r.set("sheet", *sheet);
        }
        vports_.push_back(std::move(r));
      }
    } else if (t == "UCS") {
      ++i_;
      const Group g = group();
      const auto name = val(g, 2);
      const double x = numv(g, 11);
      const double y = numv(g, 21);
      if (name && !name->empty() && ((x != 0 && !std::isnan(x)) || (y != 0 && !std::isnan(y)))) {
        if (!out.get("ucs").is_object()) {
          out.set("ucs", base::make_object({{"current", nullptr}, {"named", Value::array()}}));
        }
        out.find("ucs")->find("named")->as_array().emplace_back(base::make_object(
            {{"name", *name}, {"x", numv(g, 10)}, {"y", numv(g, 20)}, {"ang", base::js_atan2(y, x)}}));
      }
    } else if (t == "BLOCK_RECORD") {
      ++i_;
      const Group g = group();
      const auto h = val(g, 5);
      const auto n = val(g, 2);
      if (h && !h->empty() && n && !n->empty()) {
        block_records_[base::js_to_upper(*h)] = *n;
      }
    } else {
      ++i_;
    }
  }
  ++i_;
}

void DxfReader::read_blocks() {
  Value& out = out_;
  while (i_ < pairs_.size() && !at0("ENDSEC")) {
    if (!at0("BLOCK")) {
      ++i_;
      continue;
    }
    ++i_;
    const Group g = group();
    const std::string name = val(g, 2).value_or("");
    const std::uint32_t flags = bits(int_of(g, 70, "0"));
    const Value base_pt = base::make_object({{"x", numv(g, 10)}, {"y", numv(g, 20)}});
    Lists all = read_entity_list(nullptr, false, true);
    base::Array ents;
    base::Array attdefs;
    for (Value& e : all.model) {
      if (e.get("type") == Value("attdef")) {
        attdefs.push_back(e.get("def"));
      } else {
        ents.push_back(std::move(e));
      }
    }
    if (at0("ENDBLK")) {
      ++i_;
      (void)group();
    }
    if (is_param_block(Value(name))) {
      out.set("parametric", parametric_text(ents));
      continue;
    }
    if (is_m3d_block(Value(name))) {
      if (const Value t = read3d_text(ents); !t.is_null()) {
        out.set("model3d", t);
      }
      if (const Value v = read3d_view(ents); !v.is_null()) {
        out.set("view3d", v);
      }
      continue;
    }
    const std::string up = base::js_to_upper(name);
    if (up.starts_with("*PAPER_SPACE") && up.size() > 12 &&
        std::ranges::all_of(up.substr(12), [](char ch) { return ch >= '0' && ch <= '9'; })) {
      paper_blocks_.emplace_back(up, std::move(ents));
      continue;
    }
    if (up.starts_with("*MODEL_SPACE") || up.starts_with("*PAPER_SPACE") || up.starts_with("$MODEL_SPACE") ||
        up.starts_with("$PAPER_SPACE")) {
      continue;  // layout records
    }
    if ((flags & 4U) != 0) {  // a linked drawing (xref.js)
      const auto path = val(g, 1);
      out.find("blocks")->as_array().emplace_back(base::make_object(
          {{"name", name},
           {"base", base_pt},
           {"entities", Value::array()},
           {"xref", base::make_object({{"path", path && !path->empty() ? *path : name}, {"loaded", false}})}}));
      bump(report(), "xrefs");
      continue;
    }
    if (up.starts_with("CCLAYOUT_") && up.size() > 9 &&
        std::ranges::all_of(up.substr(9), [](char ch) { return ch >= '0' && ch <= '9'; })) {
      if (Value lay = layout_from_block(ents); !lay.is_null()) {
        out.find("layouts")->as_array().push_back(std::move(lay));
        continue;
      }
    }
    Value b = base::make_object({{"name", name}, {"base", base_pt}, {"entities", std::move(ents)}});
    if (!attdefs.empty()) {
      b.set("attdefs", Value(std::move(attdefs)));
    }
    out.find("blocks")->as_array().push_back(std::move(b));
  }
  ++i_;
}

void DxfReader::read_objects() {
  while (i_ < pairs_.size() && !at0("ENDSEC")) {
    if (at0("LAYOUT")) {
      ++i_;
      const Group g = group();
      std::size_t k = 0;
      const auto layout_mark = [&g](std::size_t q) {
        return g[q].first == 100 && base::js_trim(g[q].second) == "AcDbLayout";
      };
      while (k < g.size() && !layout_mark(k)) {
        ++k;
      }
      const Group lg = k < g.size() ? Group(g.begin() + static_cast<std::ptrdiff_t>(k), g.end()) : g;
      const Group pg = k < g.size() ? Group(g.begin(), g.begin() + static_cast<std::ptrdiff_t>(k)) : g;
      const auto nm = val(lg, 1);
      layout_objs_.push_back({.name = nm && !nm->empty() ? *nm : std::string("Layout"),
                              .tab = int_of(lg, 71, "0"),
                              .record = base::js_to_upper(val(lg, 330).value_or("")),
                              .min = {numv(lg, 10), numv(lg, 20)},
                              .max = {numv(lg, 11), numv(lg, 21)},
                              .paper_w = numv(pg, 44),
                              .paper_h = numv(pg, 45),
                              .units = int_of(pg, 72, "-1"),
                              .rot = int_of(pg, 73, "0")});
    } else if (at0("DICTIONARY") || at0("SPATIAL_FILTER")) {
      const std::string type(base::js_trim(at().second));
      ++i_;
      const Group g = group();
      const std::string hd = base::js_to_upper(val(g, 5).value_or(""));
      if (type == "DICTIONARY") {
        std::map<std::string, std::string> e;
        for (std::size_t k = 0; k + 1 < g.size(); ++k) {
          if (g[k].first == 3 && g[k + 1].first == 350) {
            e[std::string(base::js_trim(g[k].second))] = base::js_to_upper(base::js_trim(g[k + 1].second));
          }
        }
        dict_by_h_[hd] = std::move(e);
      } else {
        std::size_t k0 = 0;
        while (k0 < g.size() && g[k0].first != 70) {
          ++k0;
        }
        base::Array pts;
        for (std::size_t k = k0 + 1; k0 < g.size() && k + 1 < g.size() && g[k].first == 10; k += 2) {
          pts.emplace_back(base::make_object(
              {{"x", base::js_parse_float(g[k].second)}, {"y", base::js_parse_float(g[k + 1].second)}}));
        }
        std::vector<double> m;
        for (const auto& [c, v] : g) {
          if (c == 40) {
            m.push_back(base::js_parse_float(v));
          }
        }
        Value f = base::make_object({{"points", std::move(pts)}});
        if (m.size() == 24) {
          base::Array a;
          base::Array b;
          for (std::size_t q = 0; q < 24; ++q) {
            (q < 12 ? a : b).emplace_back(m[q]);
          }
          f.set("inverseBlockTransform", Value(std::move(a)));
          f.set("clipTransform", Value(std::move(b)));
        }
        filter_by_h_[hd] = std::move(f);
      }
    } else {
      ++i_;
    }
  }
  ++i_;
}

DxfReader::Lists DxfReader::read_entity_list(Value* rep_in, bool top, bool in_block) {
  Lists r;
  Value local =
      base::make_object({{"read", 0}, {"skipped", Value::object()}, {"paperSpace", 0}, {"notes", Value::array()}});
  Value& rep = rep_in != nullptr ? *rep_in : local;
  while (i_ < pairs_.size()) {
    const auto& [c, v] = at();
    const std::string type(base::js_trim(v));
    if (c != 0) {
      ++i_;
      continue;
    }
    if (type == "ENDSEC" || type == "ENDBLK") {
      break;
    }
    ++i_;
    const Group g = group();
    std::vector<Follow> follow;
    const bool has_follow = type == "POLYLINE" || (type == "INSERT" && val(g, 66) == std::optional<std::string>("1"));
    if (has_follow) {
      follow = read_followers();
    }
    const bool in_paper = top && val(g, 67) == std::optional<std::string>("1");
    Value made = entity_from(type, g, has_follow ? &follow : nullptr, rep);
    if (!made.truthy()) {
      continue;
    }
    if (!made.is_array() && has_xdata(g)) {
      std::vector<std::string> strs;
      for (const auto& [k, x] : app_of(xdata(g), "CADCRAFT")) {
        strs.push_back(x);
      }
      for (const std::string& s : strs) {  // gcons.js refKeyIn: CCREF:<digits>
        if (s.starts_with("CCREF:") && s.size() > 6 && s.size() <= 15 &&
            std::ranges::all_of(s.substr(6), [](char ch) { return ch >= '0' && ch <= '9'; })) {
          made.set("ccref", s.substr(6));
          break;
        }
      }
    }
    if (made.get("type") == Value("attdef") && !in_block) {
      skip(rep, words_.t("dxf.attdef_outside_block"));
      continue;
    }
    base::Array& dest = in_paper ? r.paper : r.model;
    if (made.is_array()) {
      for (const Value& e : made.as_array()) {
        dest.push_back(e);
      }
    } else {
      dest.push_back(std::move(made));
    }
    bump(rep, in_paper ? "paperSpace" : "read");
  }
  return r;
}

std::vector<Follow> DxfReader::read_followers() {
  std::vector<Follow> subs;
  while (i_ < pairs_.size() && at().first == 0 && (at0("VERTEX") || at0("ATTRIB"))) {
    const std::string t(base::js_trim(at().second));
    ++i_;
    subs.push_back({.type = t, .g = group()});
  }
  if (at0("SEQEND")) {
    ++i_;
    (void)group();
  }
  return subs;
}

// NOLINTNEXTLINE(misc-no-recursion) — an INSERT's ATTRIBs come through here once; an ATTRIB has none of its own
Value DxfReader::entity_from(const std::string& type, const Group& g, const std::vector<Follow>* follow, Value& rep) {
  // group 60 = 1: an object the file hides (a dynamic block's other visibility states) is not drawn
  if (val(g, 60) == std::optional<std::string>("1") && type != "ATTDEF" && type != "VIEWPORT") {
    bump(rep, "hidden");
    return {nullptr};
  }
  Value made = entity_from0(type, g, follow, rep);
  if (!made.truthy()) {
    return made;
  }
  const bool many = made.is_array();
  if (many && made.as_array().empty()) {
    return made;
  }
  const Value one = many ? made.as_array().front() : made;
  if (!one.truthy() || one.get("type") == Value("attdef") || one.get("type") == Value("face3d")) {
    return made;
  }
  Value add = has_xdata(g) ? props_from_xdata(xdata_value(xdata(g))) : Value::object();
  if (!(one.get("type") == Value("hatch"))) {
    add.erase("bg");
    add.erase("ox");
    add.erase("oy");
    add.erase("assoc");
  }
  static const std::map<std::string, int, std::less<>> kZ{
      {"LINE", 0},   {"CIRCLE", 30}, {"ARC", 30},    {"POINT", 30},      {"ELLIPSE", 30},  {"INSERT", 30},
      {"MTEXT", 30}, {"TEXT", 30},   {"ATTRIB", 30}, {"LWPOLYLINE", 38}, {"POLYLINE", 30}, {"HATCH", 30}};
  if (const auto it = kZ.find(type); it != kZ.end() && it->second != 0) {
    const double z = numv(g, it->second);
    if (z != 0 && !std::isnan(z)) {
      add.set("z", z);
    }
  }
  if (type == "TEXT" || type == "ATTRIB" || type == "MTEXT") {
    if (const auto st = val(g, 7); st && !st->empty() && !upper_is(*st, "STANDARD")) {
      add.set("style", *st);
    }
    if (type != "MTEXT") {
      const double o = numv(g, 51);
      if (o != 0 && !std::isnan(o)) {
        add.set("obl", rad(o));
      }
    } else {
      std::string raw;
      for (const auto& [c, v] : g) {
        if (c == 3 || c == 1) {
          raw += v;
        }
      }
      // /^\\Q(-?[\d.]+);/
      if (raw.starts_with("\\Q")) {
        std::size_t k = 2;
        if (k < raw.size() && raw[k] == '-') {
          ++k;
        }
        const std::size_t d0 = k;
        while (k < raw.size() && ((raw[k] >= '0' && raw[k] <= '9') || raw[k] == '.')) {
          ++k;
        }
        if (k > d0 && k < raw.size() && raw[k] == ';') {
          const double q = base::js_number(raw.substr(2, k - 2));
          if (q != 0 && !std::isnan(q)) {
            add.set("obl", rad(q));
          }
        }
      }
    }
  }
  if (type == "INSERT") {
    const double sz = numv(g, 43, 1);
    if (sz != 0 && !std::isnan(sz) && sz != 1) {
      add.set("sz", sz);
    }
    for (std::size_t k = 0; k + 1 < g.size(); ++k) {  // its extension dictionary (XCLIP, WL.16)
      if (g[k].first == 102 && base::js_trim(g[k].second) == "{ACAD_XDICTIONARY") {
        if (g[k + 1].first == 360) {
          add.set("_xd", base::js_to_upper(base::js_trim(g[k + 1].second)));
        }
        break;
      }
    }
  }
  if (type == "HATCH") {
    const double isl = int_of(g, 75, "0");
    if (isl == 1) {
      add.set("island", "outer");
    } else if (isl == 2) {
      add.set("island", "ignore");
    }
    if (val(g, 71) == std::optional<std::string>("1")) {
      add.set("assoc", true);
    }
  }
  if (type == "DIMENSION" && one.get("type") == Value("dim")) {
    if (const auto ds = val(g, 3); ds && !ds->empty() && !upper_is(*ds, "STANDARD")) {
      add.set("dimstyle", *ds);
    }
    const double tr = numv(g, 53);
    if (tr != 0 && !std::isnan(tr)) {
      add.set("trot", norm_angle(rad(tr)));
    }
  }
  if (type == "VIEWPORT") {
    const auto stt = static_cast<std::uint32_t>(base::js_to_int32(int_of(g, 90, "0")));
    if ((stt & 16384U) != 0) {
      add.set("locked", true);
    }
    if ((stt & 131072U) != 0 || val(g, 68) == std::optional<std::string>("0")) {
      add.set("on", false);
    }
    for (const auto& [c, v] : app_of(xdata(g), "CADCRAFT")) {
      if (v == "CCVPLOCK") {
        add.set("locked", true);
      }
    }
  }
  static const std::set<std::string, std::less<>> kTh{"line", "circle", "arc", "poly", "point", "text"};
  const auto finish = [&](Value& e) {
    for (const base::Member* m : base::js_members(add.as_object())) {
      e.set(m->key, m->value);
    }
    if (!kTh.contains(str(e.get("type")))) {
      e.erase("th");
    }
    if (type == "LINE") {
      e.erase("z");
      const double z1 = numv(g, 30);
      const double z2 = numv(g, 31);
      if (z1 != 0 && !std::isnan(z1)) {
        e.set("z1", z1);
      }
      if (z2 != 0 && !std::isnan(z2)) {
        e.set("z2", z2);
      }
    }
    const bool shift = (add.get("ox").truthy() || add.get("oy").truthy());
    if (e.get("type") == Value("hatch") && shift && e.get("patlines").is_array()) {
      base::Array pl = e.get("patlines").as_array();
      for (Value& l : pl) {
        l.set("x", num(l.get("x")) - (add.get("ox").truthy() ? num(add.get("ox")) : 0));
        l.set("y", num(l.get("y")) - (add.get("oy").truthy() ? num(add.get("oy")) : 0));
      }
      e.set("patlines", Value(std::move(pl)));
    }
  };
  if (many) {
    for (Value& e : made.as_array()) {
      finish(e);
    }
  } else {
    finish(made);
  }
  return made;
}

// NOLINTNEXTLINE(misc-no-recursion) — see entity_from
Value DxfReader::entity_from0(const std::string& type, const Group& g, const std::vector<Follow>* follow, Value& rep) {
  const Value base = common(g);
  const bool f = numv(g, 230, 1) < 0;  // extrusion (0, 0, −1): the OCS x runs the other way
  const auto X = [f](double x) { return f ? -x : x; };
  const std::map<std::string, std::vector<Pair>> xd =
      has_xdata(g) ? xdata(g) : std::map<std::string, std::vector<Pair>>{};
  std::optional<std::vector<std::string>> cc;
  if (const auto it = xd.find("CADCRAFT"); it != xd.end()) {
    std::vector<std::string> v;
    for (const auto& [k, x] : it->second) {
      v.push_back(x);
    }
    cc = std::move(v);
  }
  const auto cc_value = [&cc] {
    base::Array a;
    for (const std::string& x : *cc) {
      a.emplace_back(x);
    }
    return Value(std::move(a));
  };
  const auto pt = [&g](int c) { return base::make_object({{"x", numv(g, c)}, {"y", numv(g, c + 10)}}); };
  if (type == "LINE") {
    return with(base,
                {{"type", "line"}, {"x1", numv(g, 10)}, {"y1", numv(g, 20)}, {"x2", numv(g, 11)}, {"y2", numv(g, 21)}});
  }
  if (type == "POINT") {
    Value e = with(base, {{"type", "point"}, {"x", numv(g, 10)}, {"y", numv(g, 20)}});
    if (cc) {
      e.set("cc", cc_value());
    }
    return e;
  }
  if (type == "XLINE" || type == "RAY") {
    const double dx = numv(g, 11);
    const double dy = numv(g, 21);
    const double n = base::js_hypot(dx, dy);
    if (!(n > 1e-12)) {
      skip(rep, type);
      return {nullptr};
    }
    return with(base, {{"type", type == "XLINE" ? "xline" : "ray"},
                       {"x", numv(g, 10)},
                       {"y", numv(g, 20)},
                       {"dx", dx / n},
                       {"dy", dy / n}});
  }
  if (type == "3DFACE") {
    base::Array pts;
    for (int k = 0; k < 4; ++k) {
      pts.emplace_back(base::Array{numv(g, 10 + k), numv(g, 20 + k), numv(g, 30 + k)});
    }
    return with(base, {{"type", "face3d"}, {"pts", Value(std::move(pts))}});
  }
  if (type == "CIRCLE") {
    return with(base, {{"type", "circle"}, {"cx", X(numv(g, 10))}, {"cy", numv(g, 20)}, {"r", numv(g, 40)}});
  }
  if (type == "ARC") {
    double a0 = rad(numv(g, 50));
    double a1 = rad(numv(g, 51));
    if (f) {
      const double t0 = kPi - a1;
      a1 = kPi - a0;
      a0 = t0;
    }
    return with(base, {{"type", "arc"},
                       {"cx", X(numv(g, 10))},
                       {"cy", numv(g, 20)},
                       {"r", numv(g, 40)},
                       {"a0", norm_angle(a0)},
                       {"a1", norm_angle(a1)}});
  }
  if (type == "LWPOLYLINE") {
    base::Array pts;
    bool wide = false;
    for (const auto& [c, v] : g) {
      if (c == 10) {
        pts.emplace_back(base::make_object({{"x", X(base::js_parse_float(v))}, {"y", 0}, {"b", 0}}));
      } else if (c == 20 && !pts.empty()) {
        pts.back().set("y", base::js_parse_float(v));
      } else if (c == 42 && !pts.empty()) {
        pts.back().set("b", (f ? -1 : 1) * base::js_parse_float(v));
      } else if ((c == 40 || c == 41) && !pts.empty()) {
        const double x = base::js_parse_float(v);
        if (x > 0) {
          wide = true;
        }
        pts.back().set(c == 40 ? "sw" : "ew", x > 0 ? x : 0);
      }
    }
    if (!wide) {
      for (Value& p : pts) {
        p.erase("sw");
        p.erase("ew");
      }
    }
    if (pts.size() < 2) {
      return {nullptr};
    }
    const double cw = numv(g, 43);
    Value e = with(
        base, {{"type", "poly"}, {"pts", Value(std::move(pts))}, {"closed", (bits(int_of(g, 70, "0")) & 1U) == 1}});
    if (cw > 0 && !wide) {
      e.set("w", cw);
    }
    return e;
  }
  if (type == "POLYLINE") {
    const std::uint32_t flags = bits(int_of(g, 70, "0"));
    if ((flags & (16U | 64U)) != 0) {
      skip(rep, "POLYLINE (mesh)");
      return {nullptr};
    }
    const double dsw = numv(g, 40);
    const double dew = numv(g, 41);
    base::Array pts;
    const Group* first_vertex = nullptr;
    if (follow != nullptr) {
      for (const Follow& s : *follow) {
        if (s.type != "VERTEX") {
          continue;
        }
        if (first_vertex == nullptr) {
          first_vertex = &s.g;
        }
        if ((bits(int_of(s.g, 70, "0")) & 16U) != 0) {
          continue;
        }
        pts.emplace_back(base::make_object({{"x", X(numv(s.g, 10))},
                                            {"y", numv(s.g, 20)},
                                            {"b", (f ? -1 : 1) * numv(s.g, 42)},
                                            {"sw", numv(s.g, 40, dsw)},
                                            {"ew", numv(s.g, 41, dew)}}));
      }
    }
    const bool wide =
        std::ranges::any_of(pts, [](const Value& p) { return num(p.get("sw")) > 0 || num(p.get("ew")) > 0; });
    const bool one =
        wide && std::ranges::all_of(pts, [&pts](const Value& p) {
          return num(p.get("sw")) == num(pts.front().get("sw")) && num(p.get("ew")) == num(pts.front().get("sw"));
        });
    for (Value& p : pts) {
      if (!wide || one) {
        p.erase("sw");
        p.erase("ew");
      }
    }
    if (pts.size() < 2) {
      return {nullptr};
    }
    Value e = with(base, {{"type", "poly"}, {"pts", Value(std::move(pts))}, {"closed", (flags & 1U) == 1}});
    if (one) {
      const double w = dsw != 0 && !std::isnan(dsw) ? dsw : numv(first_vertex != nullptr ? *first_vertex : Group{}, 40);
      e.set("w", w);
    }
    if (cc) {
      e.set("cc", cc_value());
    }
    return e;
  }
  if (type == "TEXT" || type == "ATTRIB" || type == "ATTDEF") {
    const double h72 = int_of(g, 72, "0");
    const double v73 = int_of(g, type == "TEXT" ? 73 : 74, "0");
    const bool aligned = h72 != 0 || v73 != 0;
    std::string ha = "left";
    if (h72 == 1 || h72 == 4) {
      ha = "center";
    } else if (h72 == 2) {
      ha = "right";
    }
    std::string va = "baseline";
    if (h72 == 4 || v73 == 2) {
      va = "middle";
    } else if (v73 == 3) {
      va = "top";
    } else if (v73 == 1) {
      va = "bottom";
    }
    const bool second = aligned && h72 != 3 && h72 != 5;
    const std::string s = decode_text(val(g, 1).value_or(""));
    const double h = numv(g, 40, 2.5);
    const double wf = numv(g, 41, 1);
    Value t = with(base, {{"type", "text"},
                          {"x", X(numv(g, second ? 11 : 10))},
                          {"y", numv(g, second ? 21 : 20)},
                          {"h", h != 0 && !std::isnan(h) ? h : 2.5},
                          {"rot", norm_angle(rad(numv(g, 50)))},
                          {"wf", wf != 0 && !std::isnan(wf) ? wf : 1},
                          {"s", s},
                          {"ha", ha},
                          {"va", va}});
    if (type != "TEXT") {
      const std::uint32_t fl = bits(int_of(g, 70, "0"));
      const std::string tag = val(g, 2).value_or("");
      if (tag.empty()) {
        return {nullptr};
      }
      if (type == "ATTDEF") {
        return base::make_object({{"type", "attdef"},
                                  {"def", attdef(t, {.tag = Value(tag),
                                                     .prompt = decode_text(val(g, 3).value_or("")),
                                                     .def = s,
                                                     .invisible = (fl & 1U) != 0,
                                                     .constant = (fl & 2U) != 0,
                                                     .preset = (fl & 8U) != 0})}});
      }
      t.set("tag", tag);
      if ((fl & 1U) != 0) {
        t.set("invisible", true);
      }
      return t;
    }
    if (s.empty()) {
      return {nullptr};
    }
    return t;
  }
  if (type == "MTEXT") {
    std::string raw;
    for (const auto& [c, v] : g) {
      if (c == 3) {
        raw += v;
      }
    }
    raw += val(g, 1).value_or("");
    const std::string s = mtext_plain(raw);
    if (s.empty()) {
      return {nullptr};
    }
    double att = int_of(g, 71, "1");
    if (att == 0 || std::isnan(att)) {
      att = 1;
    }
    double rot = rad(numv(g, 50));
    if (val(g, 11)) {
      rot = base::js_atan2(numv(g, 21), numv(g, 11));
    }
    const double ww = numv(g, 41);
    const double ls = numv(g, 44, 1);
    static constexpr std::array<const char*, 3> kHa{"left", "center", "right"};
    static constexpr std::array<const char*, 3> kVa{"top", "middle", "bottom"};
    const double ai = att - 1;
    const double hr = std::fmod(ai, 3);  // JS %: the sign of the dividend; a negative index is no alignment
    const double vi = std::floor(ai / 3);
    const double h = numv(g, 40, 2.5);
    Value e = with(base, {{"type", "text"},
                          {"x", numv(g, 10)},
                          {"y", numv(g, 20)},
                          {"h", h != 0 && !std::isnan(h) ? h : 2.5},
                          {"rot", norm_angle(rot)},
                          {"s", s},
                          {"ha", hr >= 0 && hr < 3 ? Value(kHa.at(static_cast<std::size_t>(hr))) : Value()},
                          {"va", vi >= 0 && vi < 3 ? Value(kVa.at(static_cast<std::size_t>(vi))) : Value()}});
    if (ww > 0) {
      e.set("ww", ww);
    }
    if (ls > 0 && ls != 1) {
      e.set("ls", ls);
    }
    Value bg_color = val(g, 421) ? base::make_object({{"rgb", int_of(g, 421, "0")}})
                                 : base::make_object({{"index", int_of(g, 63, "7")}});
    const Value mask = mask_of(base::make_object({{"backgroundFlags", int_of(g, 90, "0")},
                                                  {"backgroundScale", numv(g, 45, 1.5)},
                                                  {"backgroundColor", std::move(bg_color)}}));
    if (mask.is_object()) {
      for (const base::Member* m : base::js_members(mask.as_object())) {
        e.set(m->key, m->value);
      }
    }
    return e;
  }
  if (type == "ELLIPSE") {
    return with(base, {{"type", "ellipse"},
                       {"cx", numv(g, 10)},
                       {"cy", numv(g, 20)},
                       {"mx", numv(g, 11)},
                       {"my", numv(g, 21)},
                       {"ratio", numv(g, 40, 1)},
                       {"t0", numv(g, 41, 0)},
                       {"t1", numv(g, 42, kTau)}});
  }
  if (type == "SPLINE") {
    const std::vector<XY> pts = spline_points(g);
    if (pts.size() < 2) {
      skip(rep, "SPLINE");
      return {nullptr};
    }
    bump(rep, "splines");
    base::Array a;
    for (const XY& p : pts) {
      a.emplace_back(base::make_object({{"x", p.x}, {"y", p.y}, {"b", 0}}));
    }
    return with(base, {{"type", "poly"}, {"pts", Value(std::move(a))}, {"closed", false}});
  }
  if (type == "SOLID" || type == "TRACE") {
    base::Array ring;
    for (const auto& [a, b] : {std::pair{10, 20}, std::pair{11, 21}, std::pair{13, 23}, std::pair{12, 22}}) {
      ring.emplace_back(X(numv(g, a)));
      ring.emplace_back(numv(g, b));
    }
    ring.push_back(ring[0]);
    ring.push_back(ring[1]);
    return with(base, {{"type", "hatch"}, {"solid", true}, {"loops", Value(base::Array{Value(std::move(ring))})}});
  }
  if (type == "HATCH") {
    base::Array loops = hatch_loops(g, rep, words_);
    if (loops.empty()) {
      return {nullptr};
    }
    const bool solid = val(g, 70) == std::optional<std::string>("1");
    Value h = with(
        base,
        {{"type", "hatch"}, {"solid", solid}, {"pattern", val(g, 2).value_or("")}, {"loops", Value(std::move(loops))}});
    if (!solid) {
      const Value hp = hatch_pattern(g);
      for (const base::Member* m : base::js_members(hp.as_object())) {
        h.set(m->key, m->value);
      }
    }
    return h;
  }
  if (type == "INSERT") {
    const auto name = val(g, 2);
    if (!name || name->empty()) {
      return {nullptr};
    }
    double rot = rad(numv(g, 50));
    double sx = numv(g, 41, 1);
    double x = numv(g, 10);
    if (f) {
      x = -x;
      rot = kPi - rot;
      sx = -sx;
    }
    const Value ins = with(base, {{"type", "insert"},
                                  {"block", *name},
                                  {"x", x},
                                  {"y", numv(g, 20)},
                                  {"sx", sx},
                                  {"sy", numv(g, 42, 1)},
                                  {"rot", norm_angle(rot)}});
    const double cols = std::max(1.0, int_of(g, 70, "1"));
    const double rows = std::max(1.0, int_of(g, 71, "1"));
    base::Array out;
    if (cols * rows > 1 && cols * rows <= 100000) {
      const double dc = numv(g, 44);
      const double dr = numv(g, 45);
      const Mat m = m_mul(m_translate(num(ins.get("x")), num(ins.get("y"))), m_rotate(num(ins.get("rot"))));
      for (std::size_t ri = 0, nr = times(rows, 100000); ri < nr; ++ri) {
        for (std::size_t ci = 0, nc = times(cols, 100000); ci < nc; ++ci) {
          const auto r = static_cast<double>(ri);
          const auto c = static_cast<double>(ci);
          const double px = m[0] * (c * dc) + m[2] * (r * dr) + m[4];
          const double py = m[1] * (c * dc) + m[3] * (r * dr) + m[5];
          out.push_back(with(ins, {{"x", px}, {"y", py}}));
        }
      }
    } else {
      out.push_back(ins);
    }
    // attribute values go on the (first) reference; attrs.js resolveAttribs places them on its block
    base::Array attribs;
    if (follow != nullptr) {
      for (const Follow& a : *follow) {
        if (a.type == "ATTRIB") {
          if (Value t = entity_from("ATTRIB", a.g, nullptr, rep); t.truthy()) {
            attribs.push_back(std::move(t));
          }
        }
      }
    }
    out.front().set("_attribs", Value(std::move(attribs)));
    return {std::move(out)};
  }
  if (type == "DIMENSION") {
    if (upper_is(str(base.get("layer")), "*LAYERNAMEFORDYNAMICCONSTRAINT")) {
      skip(rep, words_.t("dxf.dimension_parametric_constraint"));
      return {nullptr};
    }
    const auto block_name = val(g, 2);
    const std::uint32_t kind = bits(int_of(g, 70, "0")) & 7U;
    const auto ds_name = val(g, 3);
    const std::string key = base::js_to_upper(ds_name && !ds_name->empty() ? *ds_name : std::string("STANDARD"));
    Value st = dim_styles_.contains(key) ? dim_styles_.at(key) : out_.get("dimStyle");
    if (const auto ds = xlist(app_of(xd, "ACAD"), "DSTYLE")) {
      std::map<double, std::string> m;
      for (std::size_t k = 0; k + 1 < ds->size(); k += 2) {
        if ((*ds)[k].first == 1070) {
          m[base::js_round(base::js_number((*ds)[k].second))] = (*ds)[k + 1].second;
        }
      }
      st_from_codes(
          [&m](double c) -> Value {
            const auto it = m.find(c);
            return it == m.end() ? Value() : Value(it->second);
          },
          st);
    }
    if (cc && !cc->empty() && (*cc)[0] == "DIM") {
      const auto at = [&cc](std::size_t k) { return k < cc->size() ? base::js_number((*cc)[k]) : std::nan(""); };
      st.set("dec", at(1));
      st.set("adec", at(2));
      st.set("zin", at(3));
      if (cc->size() > 4) {
        st.set("lunit", at(4));
      }
    }
    std::optional<double> trot;
    if (cc) {
      cc_dim_from(*cc, st);
      for (const std::string& x : *cc) {
        if (x.starts_with("CCTROT:")) {
          const double v = base::js_number(x.substr(7));
          if (std::isfinite(v) && v != 0) {
            trot = v;
          }
          break;
        }
      }
    }
    fix_style(st);
    std::string text = mtext_plain(val(g, 1).value_or(""));
    if (text == "<>") {
      text.clear();
    }
    Value d;
    if (kind == 0 || kind == 1) {
      d = base::make_object({{"kind", kind != 0 ? "aligned" : "linear"},
                             {"p1", pt(13)},
                             {"p2", pt(14)},
                             {"loc", pt(10)},
                             {"rot", norm_angle(rad(numv(g, 50)))}});
    } else if (kind == 4) {
      d = base::make_object({{"kind", "radius"}, {"c", pt(10)}, {"p1", pt(15)}, {"loc", pt(11)}});
    } else if (kind == 3) {
      const Value a = pt(15);
      const Value b = pt(10);
      d = base::make_object({{"kind", "diameter"},
                             {"c", base::make_object({{"x", (num(a.get("x")) + num(b.get("x"))) / 2},
                                                      {"y", (num(a.get("y")) + num(b.get("y"))) / 2}})},
                             {"p1", a},
                             {"loc", pt(11)}});
    } else if (kind == 5) {
      d = base::make_object({{"kind", "angular"}, {"c", pt(15)}, {"p1", pt(13)}, {"p2", pt(14)}, {"loc", pt(10)}});
    } else if (kind == 2) {
      const auto L = [&g](int a, int b) {
        return base::make_object(
            {{"x1", numv(g, a)}, {"y1", numv(g, a + 10)}, {"x2", numv(g, b)}, {"y2", numv(g, b + 10)}});
      };
      if (const auto r = angular_from_lines(L(13, 14), L(15, 10), numv(g, 16), numv(g, 26))) {
        d = base::make_object({{"kind", "angular"},
                               {"c", base::make_object({{"x", r->cx}, {"y", r->cy}})},
                               {"p1", base::make_object({{"x", r->x1}, {"y", r->y1}})},
                               {"p2", base::make_object({{"x", r->x2}, {"y", r->y2}})},
                               {"loc", pt(16)}});
      }
    }
    if (d.is_object() && dim_usable(d)) {
      Value e = with(base, {{"type", "dim"}});
      for (const base::Member* m : base::js_members(d.as_object())) {
        e.set(m->key, m->value);
      }
      e.set("text", text);
      e.set("st", st);
      if (block_name && !block_name->empty()) {
        e.set("block", *block_name);
      }
      if (trot) {
        e.set("trot", *trot);
      }
      return e;
    }
    if (!block_name || block_name->empty()) {
      skip(rep, "DIMENSION");
      return {nullptr};
    }
    return with(base, {{"type", "insert"},
                       {"block", *block_name},
                       {"x", 0},
                       {"y", 0},
                       {"sx", 1},
                       {"sy", 1},
                       {"rot", 0},
                       {"dim", true}});
  }
  if (type == "VIEWPORT") {
    Value v = with(base, {{"type", "viewport"},
                          {"x", numv(g, 10)},
                          {"y", numv(g, 20)},
                          {"w", numv(g, 40)},
                          {"h", numv(g, 41)},
                          {"id", int_of(g, 69, "0")}});
    std::optional<XY> vc;
    double vh = 0;
    const std::optional<std::vector<Pair>> mv = xlist(app_of(xd, "ACAD"), "MVIEW");
    if (val(g, 45)) {
      vh = numv(g, 45);
      vc = XY{numv(g, 12) + numv(g, 17), numv(g, 22) + numv(g, 27)};
    } else if (mv) {
      // R12: 1070 16, target (1010…), direction, then 1040 twist, height, centre x, centre y, …
      std::vector<double> reals;
      std::optional<double> tgt;
      std::optional<double> tgy;
      base::Array frz;
      for (const auto& [c, x] : *mv) {
        if (c == 1040) {
          reals.push_back(base::js_number(x));
        } else if (c == 1010 && !tgt) {
          tgt = base::js_number(x);
        } else if (c == 1020 && !tgy) {
          tgy = base::js_number(x);
        } else if (c == 1003 && !x.empty() &&
                   std::ranges::none_of(frz, [&x](const Value& n) { return n == Value(x); })) {
          frz.emplace_back(x);
        }
      }
      const auto r = [&reals](std::size_t k) { return k < reals.size() ? reals[k] : std::nan(""); };
      vh = r(1);
      vc = XY{r(2) + tgt.value_or(0), r(3) + tgy.value_or(0)};
      if (!frz.empty()) {
        v.set("frz", Value(std::move(frz)));
      }
    }
    base::Array fh;  // DXF 2000+: frozen layers' handles, named in buildLayouts
    for (const auto& [c, x] : g) {
      if (c == 331 && !base::js_trim(x).empty()) {
        fh.emplace_back(std::string(base::js_trim(x)));
      }
    }
    if (!fh.empty()) {
      v.set("_frzH", Value(std::move(fh)));
    }
    if (vc && vh > 0 && num(v.get("h")) > 0) {
      v.set("cx", vc->x);
      v.set("cy", vc->y);
      v.set("s", num(v.get("h")) / vh);
    } else {
      v.set("cx", v.get("x"));
      v.set("cy", v.get("y"));
      v.set("s", 1);
    }
    double tw = std::nan("");
    if (val(g, 51)) {
      tw = rad(numv(g, 51));
    } else if (mv) {
      for (const auto& [c, x] : *mv) {
        if (c == 1040) {
          tw = base::js_number(x);
          break;
        }
      }
    }
    if (std::isfinite(tw) && std::abs(tw) > 1e-12) {
      v.set("tw", tw);
    }
    if (cc && !cc->empty() && (*cc)[0] == "LAYOUT") {
      const auto at = [&cc](std::size_t k) { return k < cc->size() ? (*cc)[k] : std::string(); };
      base::Array noplot;
      if (at(8) == "NOPLOT") {
        for (std::size_t k = 9; k < cc->size(); ++k) {
          noplot.emplace_back((*cc)[k]);
        }
      }
      v.set("overall", true);
      v.set("cc", base::make_object({{"name", decode_text(at(1))},
                                     {"unit", at(2) == "in" ? "in" : "mm"},
                                     {"key", cc->size() > 3 ? Value(at(3)) : Value()},
                                     {"x0", base::js_number(at(4))},
                                     {"y0", base::js_number(at(5))},
                                     {"w", base::js_number(at(6))},
                                     {"h", base::js_number(at(7))},
                                     {"noplot", Value(std::move(noplot))}}));
    }
    return v;
  }
  skip(rep, type);
  return {nullptr};
}

void DxfReader::resolve_clips() {
  const auto fix = [this](base::Array& list) {
    for (Value& e : list) {
      const Value xd = e.get("_xd");
      if (xd.is_undefined()) {
        continue;
      }
      e.erase("_xd");
      const auto d1 = dict_by_h_.find(str(xd));
      std::string filter;
      if (d1 != dict_by_h_.end()) {
        const auto af = d1->second.find("ACAD_FILTER");
        const auto d2 = dict_by_h_.find(af != d1->second.end() ? af->second : std::string());
        if (d2 != dict_by_h_.end()) {
          const auto sp = d2->second.find("SPATIAL");
          filter = sp != d2->second.end() ? sp->second : std::string();
        }
      }
      const auto f = filter_by_h_.find(filter);
      if (f != filter_by_h_.end()) {
        if (Value c = clip_from_filter(f->second); c.truthy()) {
          e.set("clip", std::move(c));
        }
      }
    }
  };
  fix(out_.find("entities")->as_array());
  fix(paper_raw_);
  for (auto& [n, list] : paper_blocks_) {
    fix(list);
  }
  for (Value& b : out_.find("blocks")->as_array()) {
    if (Value* ents = b.find("entities"); ents != nullptr && ents->is_array()) {
      fix(ents->as_array());
    }
  }
  for (Value& l : out_.find("layouts")->as_array()) {
    if (Value* ents = l.find("entities"); ents != nullptr && ents->is_array()) {
      fix(ents->as_array());
    }
  }
}

Value DxfReader::layout_from(const std::string& name, base::Array list, const std::optional<std::pair<XY, XY>>& lim,
                             const std::string& unit) {
  // splitViewports: the first viewport (the sheet itself, id 1) is dropped; it may carry CadCraft's layout data
  std::optional<Value> overall;
  base::Array entities;
  for (Value& e : list) {
    const bool vp = e.get("type") == Value("viewport");
    if (vp && (e.get("overall").truthy() || (!overall && e.get("id") == Value(1)))) {
      if (!overall) {
        overall = e;
      }
      continue;
    }
    if (vp) {
      e.erase("id");
      e.erase("overall");
      e.erase("cc");
    }
    entities.push_back(std::move(e));
  }
  for (Value& e : entities) {  // frozen layers by handle (DXF 2000+) → names; unknown handles dropped
    if (!(e.get("type") == Value("viewport")) || !e.get("_frzH").is_array()) {
      continue;
    }
    base::Array frz;
    const auto add = [&frz](const Value& n) {
      if (std::ranges::none_of(frz, [&n](const Value& x) { return x == n; })) {
        frz.push_back(n);
      }
    };
    if (e.get("frz").is_array()) {
      for (const Value& n : e.get("frz").as_array()) {
        add(n);
      }
    }
    for (const Value& h : e.get("_frzH").as_array()) {
      const auto it = layer_by_h_.find(base::js_to_upper(str(h)));
      if (it != layer_by_h_.end() && !it->second.empty()) {
        add(Value(it->second));
      }
    }
    e.erase("_frzH");
    if (!frz.empty()) {
      e.set("frz", Value(std::move(frz)));
    } else {
      e.erase("frz");
    }
  }
  const Value cc = overall ? overall->get("cc") : Value();
  if (cc.is_object()) {
    for (const Value& n : cc.get("noplot").as_array()) {
      no_plot_.insert(base::js_to_upper(str(n)));
    }
  }
  Value paper;
  if (cc.is_object()) {
    paper = base::make_object({{"w", cc.get("w")},
                               {"h", cc.get("h")},
                               {"x0", cc.get("x0")},
                               {"y0", cc.get("y0")},
                               {"unit", cc.get("unit")},
                               {"key", cc.get("key").truthy() ? cc.get("key") : Value()}});
  } else if (lim) {
    paper = base::make_object({{"w", lim->second.x - lim->first.x},
                               {"h", lim->second.y - lim->first.y},
                               {"x0", lim->first.x},
                               {"y0", lim->first.y},
                               {"unit", unit}});
  } else {
    paper = base::make_object({{"w", 420}, {"h", 297}, {"x0", 0}, {"y0", 0}, {"unit", "mm"}});
  }
  const bool sized = num(paper.get("w")) > 0 && num(paper.get("h")) > 0;
  if (!sized) {
    if (overall) {
      const double w = num(overall->get("w"));
      const double h = num(overall->get("h"));
      paper.set("w", w);
      paper.set("h", h);
      paper.set("x0", num(overall->get("x")) - w / 2);
      paper.set("y0", num(overall->get("y")) - h / 2);
    } else {
      paper.set("w", 420);
      paper.set("h", 297);
      paper.set("x0", 0);
      paper.set("y0", 0);
      paper.set("unit", "mm");
    }
  }
  if (!paper.get("key").truthy()) {
    const std::string std_key = match_paper(num(paper.get("w")), num(paper.get("h")), str(paper.get("unit")));
    if (!std_key.empty()) {
      paper.set("key", std_key);
    }
  }
  const Value cname = cc.is_object() ? cc.get("name") : Value();
  return base::make_object({{"name", cname.truthy() ? cname : Value(name)},
                            {"paper", std::move(paper)},
                            {"entities", Value(std::move(entities))}});
}

void DxfReader::build_layouts() {
  const Value& meta = out_.get("meta");
  const auto guess_unit = [&]() -> std::string {
    const std::string u = str(meta.get("units"));
    if (!u.empty()) {
      static const std::set<std::string, std::less<>> kImp{"inches", "feet", "yards", "miles", "mils", "microinches"};
      return kImp.contains(u) ? "in" : "mm";
    }
    const auto m = header_.find("$MEASUREMENT");
    if (m != header_.end()) {
      const auto c70 = m->second.find(70);
      if (c70 != m->second.end() && c70->second == "0") {
        return "in";
      }
    }
    return "mm";
  };
  base::Array own = std::move(out_.find("layouts")->as_array());  // CCLAYOUT_n blocks, placed after the first
  out_.set("layouts", Value::array());
  base::Array& layouts = out_.find("layouts")->as_array();
  if (!layout_objs_.empty()) {
    std::vector<LayoutObj> paper_layouts;
    for (const LayoutObj& l : layout_objs_) {
      if (!upper_is(l.name, "MODEL")) {
        paper_layouts.push_back(l);
      }
    }
    std::ranges::stable_sort(paper_layouts, [](const LayoutObj& a, const LayoutObj& b) { return a.tab < b.tab; });
    for (const LayoutObj& l : paper_layouts) {
      const auto rec_it = block_records_.find(l.record);
      const std::string rec = rec_it != block_records_.end() ? rec_it->second : std::string();
      base::Array list;
      if (upper_is(rec, "*PAPER_SPACE")) {
        list = paper_raw_;
      } else {
        const std::string up = base::js_to_upper(rec);
        for (const auto& [n, ents] : paper_blocks_) {
          if (n == up) {
            list = ents;  // a later block of the same name wins, as a Map would keep the last
          }
        }
      }
      const std::string unit = l.units == 0 ? "in" : l.units == 1 ? "mm" : guess_unit();  // NOLINT
      std::optional<std::pair<XY, XY>> lim;
      if (l.max.x > l.min.x && l.max.y > l.min.y) {
        lim = std::pair{l.min, l.max};
      }
      if (!lim && l.paper_w > 0) {
        const double k = unit == "in" ? 25.4 : 1;
        const bool turned = std::fmod(l.rot, 2) != 0 && !std::isnan(l.rot);
        const double w = turned ? l.paper_h : l.paper_w;
        const double h = turned ? l.paper_w : l.paper_h;
        lim = std::pair{XY{0, 0}, XY{w / k, h / k}};
      }
      layouts.push_back(layout_from(l.name, std::move(list), lim, unit));
    }
  } else if (!paper_raw_.empty()) {
    std::optional<std::pair<XY, XY>> lim;
    const auto mn = header_.find("$PLIMMIN");
    const auto mx = header_.find("$PLIMMAX");
    if (mn != header_.end() && mx != header_.end()) {
      const auto get = [](const std::map<int, std::string>& m, int c) {
        const auto it = m.find(c);
        return it == m.end() ? std::nan("") : base::js_number(it->second);
      };
      const XY a{get(mn->second, 10), get(mn->second, 20)};
      const XY b{get(mx->second, 10), get(mx->second, 20)};
      if (b.x > a.x) {
        lim = std::pair{a, b};
      }
    }
    layouts.push_back(layout_from("Layout1", paper_raw_, lim, guess_unit()));
  }
  for (Value& l : own) {
    layouts.push_back(std::move(l));
  }
  if (!layouts.empty()) {
    report().set("layouts", static_cast<double>(layouts.size()));
  }
  if (out_.get("views").is_array()) {
    base::Array views;
    for (Value v : out_.get("views").as_array()) {
      const bool paper = v.get("paper").truthy();
      if (paper && layouts.empty()) {
        continue;
      }
      v.erase("paper");
      if (paper) {
        v.set("layout", layouts.front().get("name"));
      }
      views.push_back(std::move(v));
    }
    out_.set("views", Value(std::move(views)));
  }
  // AutoCAD never plots DEFPOINTS; CadCraft's own R12 files list their no-plot layers
  for (Value& l : out_.find("layers")->as_array()) {
    const std::string up = base::js_to_upper(str(l.get("name")));
    if (up == "DEFPOINTS" || no_plot_.contains(up)) {
      l.set("plot", false);
    }
  }
}

void DxfReader::merge_extras(std::string_view text,
                             const std::function<base::Result<Value>(std::string_view)>& read_model) {
  const auto model = read_model ? read_model(text) : base::Result<Value>(base::fail(base::Errc::invalid, ""));
  if (!model) {
    report().find("notes")->as_array().emplace_back(model.error().message);
    return;
  }
  Value x = extras_from(*model, words_);
  for (const char* t : {"MULTILEADER", "IMAGE", "WIPEOUT"}) {
    report().find("skipped")->erase(t);
  }
  base::Array& blocks = out_.find("blocks")->as_array();
  std::set<std::string> names;
  for (const Value& b : blocks) {
    names.insert(base::js_to_upper(str(b.get("name"))));
  }
  for (const Value& b : x.get("anon").as_array()) {
    if (!names.contains(base::js_to_upper(str(b.get("name"))))) {
      blocks.push_back(b);
    }
  }
  for (const Value& e : x.get("model").as_array()) {
    out_.find("entities")->as_array().push_back(e);
  }
  for (const Value& pair : x.get("blocks").as_array()) {
    const std::string up = base::js_to_upper(str(pair.as_array()[0]));
    for (Value& b : blocks) {
      if (base::js_to_upper(str(b.get("name"))) == up) {
        for (const Value& e : pair.as_array()[1].as_array()) {
          b.find("entities")->as_array().push_back(e);
        }
        break;
      }
    }
  }
  for (const Value& pair : x.get("layouts").as_array()) {
    for (Value& l : out_.find("layouts")->as_array()) {
      if (l.get("name") == pair.as_array()[0]) {
        for (const Value& e : pair.as_array()[1].as_array()) {
          l.find("entities")->as_array().push_back(e);
        }
        break;
      }
    }
  }
  for (const Value& n : x.get("notes").as_array()) {
    report().find("notes")->as_array().push_back(n);
  }
}

Value DxfReader::run(std::string_view text, const std::function<base::Result<Value>(std::string_view)>& read_model) {
  out_ = base::make_object(
      {{"entities", Value::array()},
       {"layers", Value::array()},
       {"blocks", Value::array()},
       {"layouts", Value::array()},
       {"dimStyle", dim_defaults()},
       {"meta", base::make_object({{"units", ""}, {"version", ""}})},
       {"report",
        base::make_object({{"read", 0}, {"skipped", Value::object()}, {"paperSpace", 0}, {"notes", Value::array()}})}});
  while (i_ < pairs_.size()) {
    if (at0("SECTION")) {
      const std::string name(i_ + 1 < pairs_.size() ? base::js_trim(pairs_[i_ + 1].second) : std::string_view());
      i_ += 2;
      if (name == "HEADER") {
        read_header();
      } else if (name == "TABLES") {
        read_tables();
      } else if (name == "BLOCKS") {
        read_blocks();
      } else if (name == "ENTITIES") {
        Lists r = read_entity_list(&report(), true, false);
        for (Value& e : r.model) {
          out_.find("entities")->as_array().push_back(std::move(e));
        }
        paper_raw_ = std::move(r.paper);
      } else if (name == "OBJECTS") {
        read_objects();
      } else {
        skip_section();
      }
    } else if (at0("EOF")) {
      break;
    } else {
      ++i_;
    }
  }
  {
    // every $DIM… header variable CadCraft has a key for (dwg-props.js), on top of the ones read above
    Value vars = Value::object();
    for (const auto& [k, v] : header_) {
      if (k.starts_with("$DIM") && !v.empty()) {
        vars.set(k.substr(1), v.begin()->second);
      }
    }
    Value& st = *out_.find("dimStyle");
    st_from_vars(vars, st);
    fix_style(st);
  }
  Value& meta = *out_.find("meta");
  if (const auto it = header_.find("$INSBASE"); it != header_.end()) {
    const auto get = [&it](int c) {
      const auto f = it->second.find(c);
      const double v = f == it->second.end() ? std::nan("") : base::js_number(f->second);
      return v != 0 && !std::isnan(v) ? v : 0;
    };
    meta.set("insbase", base::make_object({{"x", get(10)}, {"y", get(20)}}));
  }
  if (meta.get("lunits").truthy()) {
    if (const auto it = header_.find("$LUPREC"); it != header_.end()) {
      const auto f = it->second.find(70);
      const double p = f == it->second.end() ? std::nan("") : base::js_number(f->second);
      if (p >= 0 && p <= 8) {
        meta.set("luprec", p);
      }
    }
  }
  // attributes: values and positions relative to their block, now that every block is known
  {
    std::vector<base::Array*> lists{&out_.find("entities")->as_array(), &paper_raw_};
    for (auto& [n, list] : paper_blocks_) {
      lists.push_back(&list);
    }
    for (Value& b : out_.find("blocks")->as_array()) {
      lists.push_back(&b.find("entities")->as_array());
    }
    for (Value& l : out_.find("layouts")->as_array()) {
      lists.push_back(&l.find("entities")->as_array());
    }
    resolve_attribs(lists, out_.get("blocks").as_array());
  }
  resolve_clips();
  build_layouts();
  if (!vports_.empty()) {
    const Value v = vports_from(vports_);
    const double tw = num(vports_.front().get("tw"));
    if (v.truthy()) {
      out_.set("vports", v);
    } else if (std::isfinite(tw) && std::abs(tw) > 1e-12) {
      out_.set("viewTwist", tw);
    }
  }
  if (const auto o = header_.find("$UCSORG"), x = header_.find("$UCSXDIR"); o != header_.end() && x != header_.end()) {
    const auto get = [](const std::map<int, std::string>& m, int c) {
      const auto f = m.find(c);
      const double v = f == m.end() ? std::nan("") : base::js_number(f->second);
      return v != 0 && !std::isnan(v) ? v : 0;
    };
    const double ux = get(x->second, 10);
    const double uy = get(x->second, 20);
    const double ox = get(o->second, 10);
    const double oy = get(o->second, 20);
    if ((ux != 0 || uy != 0) && (ox != 0 || oy != 0 || std::abs(base::js_atan2(uy, ux)) > 1e-12)) {
      if (!out_.get("ucs").is_object()) {
        out_.set("ucs", base::make_object({{"current", nullptr}, {"named", Value::array()}}));
      }
      out_.find("ucs")->set("current", base::make_object({{"x", ox}, {"y", oy}, {"ang", base::js_atan2(uy, ux)}}));
    }
  }
  // 0.4 C6: multileaders, pictures and wipeouts come through the drawing model the DWG import uses (dxf-model.js)
  if (has_extras(text)) {
    merge_extras(text, read_model);
  }
  if (num(report().get("xrefs")) > 0) {
    report().find("notes")->as_array().emplace_back(words_.tn("dwg.read.xrefs", num(report().get("xrefs")), {}));
  }
  (void)split3d(out_, words_);  // 3DFACEs → out.faces3d (the 3D tab), never 2D objects
  return std::move(out_);
}

}  // namespace

base::Result<base::Value> read_dxf(std::string_view text, const i18n::Catalog& words,
                                   const std::function<base::Result<base::Value>(std::string_view)>& read_model) {
  if (text.starts_with(words.t("dxf.autocad_binary_dxf"))) {
    return base::fail(base::Errc::invalid, words.t("dxf.binary_dxf_save_ascii"));
  }
  // text.split(/\r?\n/), then pairs of (code, value)
  std::vector<std::string_view> raw;
  for (std::size_t start = 0;;) {
    const std::size_t nl = text.find('\n', start);
    std::string_view line = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
    if (nl != std::string_view::npos && line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    raw.push_back(line);
    if (nl == std::string_view::npos) {
      break;
    }
    start = nl + 1;
  }
  std::vector<Pair> pairs;
  pairs.reserve(raw.size() / 2);
  for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
    const double code = base::js_parse_int(raw[i]);
    if (std::isnan(code) || std::abs(code) > 2e9) {
      return base::fail(base::Errc::invalid, words.t("dxf.read.bad_group_code", {{"line", std::to_string(i + 1)}}));
    }
    pairs.emplace_back(static_cast<int>(code), std::string(raw[i + 1]));
  }
  try {
    DxfReader r(std::move(pairs), words);
    return r.run(text, read_model);
  } catch (const std::exception& e) {
    return base::fail(base::Errc::invalid, e.what());
  }
}

}  // namespace cc::cad
