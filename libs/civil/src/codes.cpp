// SPDX-License-Identifier: Apache-2.0
// desckeys.js, clip.js and point-edit.js (CivilCraft 0.5), ported function by function.
#include "cc/civil/codes.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include "cc/appkit/jsops.hpp"
#include "cc/base/js.hpp"
#include "cc/civil/jsmap.hpp"
#include "cc/civil/points.hpp"
#include "cc/civil/units.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::make_object;
using base::Value;

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
std::string sv(const Value& v) {  // String(v ?? '')
  return (v.is_null() || v.is_undefined()) ? std::string() : base::js_string(v);
}
std::string slice16(const std::string& s, std::size_t n) {
  if (s.size() <= n) {
    return s;
  }
  const auto u = base::to_utf16(s);
  if (u.size() <= n) {
    return s;
  }
  std::string out;
  base::append_utf16(out, std::span(u).first(n));
  return out;
}
/// .replace(/[\u0000-\u001f\u007f]/g, with)
std::string ctrl(const std::string& s, const char* with) {
  std::string o;
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) {
      o += with;
    } else {
      o.push_back(c);
    }
  }
  return o;
}
std::string clean(const Value& v, std::size_t n) {
  return slice16(std::string(base::js_trim(ctrl(sv(v), " "))), n);
}
/// s.split(/\s+/) on a trimmed string (no empty pieces unless s is empty)
std::vector<std::string> words_of(std::string_view s) {
  std::vector<std::string> out;
  std::string cur;
  bool any = false;
  for (std::size_t i = 0; i < s.size();) {
    const std::size_t n = base::js_space_at(s, i);
    if (n != 0) {
      if (any) {
        out.push_back(cur);
      }
      cur.clear();
      any = false;
      i += n;
      continue;
    }
    cur.push_back(s[i]);
    any = true;
    ++i;
  }
  if (any) {
    out.push_back(cur);
  }
  return out;
}
std::string drop_ws(std::string_view s) {
  std::string o;
  for (std::size_t i = 0; i < s.size();) {
    const std::size_t n = base::js_space_at(s, i);
    if (n != 0) {
      i += n;
      continue;
    }
    o.push_back(s[i]);
    ++i;
  }
  return o;
}
std::string tok(const Value& v, std::size_t max) {
  return slice16(base::js_to_upper(base::js_trim(ctrl(sv(v), ""))), max);
}
bool all_chars(std::string_view s, std::string_view allowed_punct) {
  return !s.empty() && std::ranges::all_of(s, [&](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || allowed_punct.contains(c);
  });
}
Value str_list(const std::vector<std::string>& v) {
  Array a;
  for (const auto& s : v) {
    a.emplace_back(s);
  }
  return a;
}
bool includes(const Value& list, const std::string& w) {
  return std::ranges::any_of(arr(list), [&](const Value& x) { return x.is_string() && x.as_string() == w; });
}

/// The JS regex '^' + escaped code with * → .*, ? → . + '$', flag i, as a matcher over UTF-16 units.
bool glob_match(const std::vector<std::uint16_t>& pat, const std::vector<std::uint16_t>& s) {
  auto fold = [](std::uint16_t c) -> std::uint16_t {  // /i (non-unicode): simple upper-casing of one unit
    if (c >= 'a' && c <= 'z') {
      return static_cast<std::uint16_t>(c - 32);
    }
    if (c < 0x80) {
      return c;
    }
    std::string one;
    base::append_utf16(one, std::span(&c, 1));
    const auto up = base::to_utf16(base::js_to_upper(one));
    return up.size() == 1 ? up[0] : c;
  };
  auto line_end = [](std::uint16_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; };
  // dp over (pattern position, string position)
  const std::size_t n = pat.size();
  const std::size_t m = s.size();
  std::vector<std::uint8_t> cur(m + 1, 0);
  std::vector<std::uint8_t> nxt(m + 1, 0);
  // process pattern backwards: ok[i][j] = pat[i..] matches s[j..]
  std::ranges::fill(cur, 0);
  cur[m] = 1;
  for (std::size_t i = n; i-- > 0;) {
    std::ranges::fill(nxt, 0);
    if (pat[i] == '*') {
      for (std::size_t j = m + 1; j-- > 0;) {
        nxt[j] = static_cast<std::uint8_t>(cur[j] != 0 || (j < m && !line_end(s[j]) && nxt[j + 1] != 0));
      }
    } else {
      for (std::size_t j = 0; j < m; ++j) {
        const bool one = pat[i] == '?' ? !line_end(s[j]) : fold(pat[i]) == fold(s[j]);
        nxt[j] = static_cast<std::uint8_t>(one && cur[j + 1] != 0);
      }
    }
    std::swap(cur, nxt);
  }
  return cur[0] != 0;
}

double by_number(const Value& a, const Value& b) {
  const Value an = get(a, "n");
  const Value bn = get(b, "n");
  const double na = base::js_to_number(an);
  const double nb = base::js_to_number(bn);
  if (std::isfinite(na) && std::isfinite(nb)) {
    return na - nb;
  }
  return static_cast<double>(base::js_locale_compare_numeric(base::js_string(an), base::js_string(bn)));
}

}  // namespace

// ---------------------------------------------------------------------------------------------- desckeys.js

Value default_line_codes() {
  return make_object({{"on", true},
                      {"begin", Array{Value("B"), Value("BEG"), Value("START")}},
                      {"end", Array{Value("E"), Value("END")}},
                      {"close", Array{Value("C"), Value("CLO"), Value("CLOSE")}}});
}

Value clean_desc_keys(const Value& list) {
  Array out;
  if (!list.is_array()) {
    return out;
  }
  const Array& in = list.as_array();
  for (std::size_t i = 0; i < in.size() && i < 500; ++i) {
    const Value& k = in[i];
    if (!k.is_object() && !k.is_array()) {
      continue;
    }
    const std::string code = drop_ws(tok(get(k, "code"), 40));
    if (!all_chars(code, "*?_.-/#")) {
      continue;
    }
    const Value use = get(k, "use");
    const bool use_ok =
        use.is_string() && (use.as_string() == "points" || use.as_string() == "breakline" || use.as_string() == "off");
    out.push_back(make_object({{"code", code},
                               {"group", drop_ws(tok(get(k, "group"), 40))},
                               {"use", use_ok ? use : Value("")},
                               {"label", slice16(std::string(base::js_trim(ctrl(sv(get(k, "label")), " "))), 120)}}));
  }
  return out;
}

Value clean_line_codes(const Value& o) {
  Value d = default_line_codes();
  if (!o.is_object() && !o.is_array()) {
    return d;
  }
  auto list = [&](const Value& v, const Value& dv) {
    std::vector<Value> src;
    if (v.is_array()) {
      src = v.as_array();
    } else {
      std::string s = sv(v);
      std::string cur;
      for (std::size_t i = 0; i < s.size();) {
        const std::size_t n = base::js_space_at(s, i);
        if (n != 0 || s[i] == ',' || s[i] == ';') {
          src.emplace_back(cur);
          cur.clear();
          i += n != 0 ? n : 1;
          while (i < s.size()) {
            const std::size_t q = base::js_space_at(s, i);
            if (q != 0) {
              i += q;
            } else if (s[i] == ',' || s[i] == ';') {
              ++i;
            } else {
              break;
            }
          }
          continue;
        }
        cur.push_back(s[i]);
        ++i;
      }
      src.emplace_back(cur);
    }
    std::vector<std::string> a;
    for (const Value& x : src) {
      const std::string t = tok(x, 12);
      if (all_chars(t, "_.-") && std::ranges::find(a, t) == a.end()) {
        a.push_back(t);
      }
    }
    if (a.size() > 10) {
      a.resize(10);
    }
    return a.empty() ? arr(dv) : arr(str_list(a));
  };
  Array begin = list(get(o, "begin"), d.get("begin"));
  Array end = list(get(o, "end"), d.get("end"));
  Array close = list(get(o, "close"), d.get("close"));
  auto has = [](const Array& a, const Value& x) {
    return std::ranges::any_of(a, [&](const Value& y) { return y == x; });
  };
  Array end2;
  for (const Value& x : end) {
    if (!has(begin, x)) {
      end2.push_back(x);
    }
  }
  Array close2;
  for (const Value& x : close) {
    if (!has(begin, x) && !has(end2, x)) {
      close2.push_back(x);
    }
  }
  const Value on = get(o, "on");
  const bool off = on.is_bool() && !on.as_bool();  // o.on !== false
  return make_object({{"on", !off}, {"begin", begin}, {"end", end2}, {"close", close2}});
}

Value parse_desc(const Value& desc, const Value& lc_in) {
  const Value lc = lc_in.is_undefined() ? default_line_codes() : lc_in;
  const std::vector<std::string> words = words_of(base::js_trim(sv(desc)));
  const std::string code = words.empty() ? std::string() : base::js_to_upper(words[0]);
  Value ctrl_v = nullptr;
  std::ptrdiff_t at = -1;
  const Value on = get(lc, "on");
  const bool off = on.is_bool() && !on.as_bool();  // lc.on === false
  if (!off && words.size() > 1) {
    const std::string w = base::js_to_upper(words[1]);
    if (includes(get(lc, "begin"), w)) {
      ctrl_v = "begin";
    } else if (includes(get(lc, "end"), w)) {
      ctrl_v = "end";
    } else if (includes(get(lc, "close"), w)) {
      ctrl_v = "close";
    }
    if (!ctrl_v.is_null()) {
      at = 1;
    }
  }
  std::string rest;
  bool first = true;
  for (std::size_t i = 1; i < words.size(); ++i) {
    if (std::cmp_equal(i, at)) {
      continue;
    }
    rest += (first ? "" : " ") + words[i];
    first = false;
  }
  return make_object({{"code", code}, {"ctrl", ctrl_v}, {"rest", rest}});
}

long key_index(const Value& keys, std::string_view code) {
  const auto s = base::to_utf16(code);
  const Array& ks = arr(keys);
  for (std::size_t i = 0; i < ks.size(); ++i) {
    const Value c = get(ks[i], "code");
    if (!c.is_string()) {
      throw std::runtime_error("k.code.replace is not a function");
    }
    if (glob_match(base::to_utf16(c.as_string()), s)) {
      return static_cast<long>(i);
    }
  }
  return -1;
}

std::string group_of(const Value& keys, const Value& p) {
  const std::vector<std::string> w = words_of(base::js_trim(sv(get(p, "d"))));
  std::string code = w.empty() ? std::string() : base::js_to_upper(w[0]);
  if (code.empty()) {
    return "(no description)";
  }
  const long k = key_index(keys, code);
  if (k >= 0) {
    const Value g = get(arr(keys)[static_cast<std::size_t>(k)], "group");
    if (g.truthy()) {
      return base::js_string(g);
    }
  }
  return code;
}

Value figures(const Value& points, const Value& keys, const Value& line_codes, const std::vector<std::string>* only) {
  struct Item {
    Value p;
    Value ctrl;
  };
  struct Entry {
    std::string group;
    std::vector<Item> list;
  };
  std::vector<std::pair<std::string, Entry>> by_code;
  std::map<std::string, std::size_t> where;
  std::map<std::string, std::string> cache;
  for (const Value& p : arr(points)) {
    const Value d = parse_desc(get(p, "d"), line_codes);
    const std::string code = d.get("code").as_string();
    if (code.empty()) {
      continue;
    }
    const std::string g = group_of(keys, p);
    if (only != nullptr && std::ranges::find(*only, g) == only->end()) {
      continue;
    }
    auto it = where.find(code);
    if (it == where.end()) {
      it = where.emplace(code, by_code.size()).first;
      by_code.emplace_back(code, Entry{g, {}});
    }
    by_code[it->second].second.list.push_back({p, d.get("ctrl")});
  }
  Array out;
  for (auto& [code, e] : by_code) {
    appkit::js_sort(e.list, [](const Item& a, const Item& b) { return by_number(a.p, b.p); });
    const bool coded = std::ranges::any_of(e.list, [](const Item& x) { return x.ctrl.truthy(); });
    std::optional<Array> cur;
    auto flush = [&](bool closed) {
      if (cur && cur->size() >= 2) {
        const bool cl = closed && cur->size() >= 3;
        out.push_back(
            make_object({{"code", code}, {"group", e.group}, {"pts", *cur}, {"closed", cl}, {"coded", coded}}));
      }
      cur.reset();
    };
    for (const Item& it : e.list) {
      const std::string c = it.ctrl.is_string() ? it.ctrl.as_string() : std::string();
      if (c == "begin") {
        flush(false);
        cur = Array{it.p};
        continue;
      }
      if (!cur) {
        cur = Array{};
      }
      cur->push_back(it.p);
      if (c == "end") {
        flush(false);
      } else if (c == "close") {
        flush(true);
      }
    }
    flush(false);
  }
  return out;
}

Value key_stats(const Value& points, const Value& keys) {
  std::vector<double> n(arr(keys).size(), 0);
  for (const Value& p : arr(points)) {
    const std::vector<std::string> w = words_of(base::js_trim(sv(get(p, "d"))));
    if (w.empty()) {
      continue;
    }
    const long k = key_index(keys, base::js_to_upper(w[0]));
    if (k >= 0) {
      n[static_cast<std::size_t>(k)]++;
    }
  }
  Array a;
  for (const double v : n) {
    a.emplace_back(v);
  }
  return a;
}

// ---------------------------------------------------------------------------------------------- clip.js

namespace {
constexpr double kMaxCoord = 1e9;
constexpr std::size_t kMaxClip = 200000;
bool fin(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number()) && std::abs(v.as_number()) < kMaxCoord;
}
std::string cell(const Value& s) {
  std::string t = base::js_string(s);
  const bool formula = !t.empty() && (t[0] == '=' || t[0] == '+' || t[0] == '-' || t[0] == '@');
  std::size_t i = 0;
  if (!t.empty() && (t[0] == '+' || t[0] == '-')) {
    i = 1;
  }
  const bool numeric = i < t.size() && t[i] >= '0' && t[i] <= '9';
  if (formula && !numeric) {
    t = "'" + t;
  }
  if (t.find_first_of("\",\r\n") == std::string::npos) {
    return t;
  }
  std::string o = "\"";
  for (const char c : t) {
    if (c == '"') {
      o += "\"\"";
    } else {
      o.push_back(c);
    }
  }
  return o + "\"";
}
std::string num3(const Value& v) {
  if (v.is_null() || v.is_undefined()) {
    return {};
  }
  return appkit::num_str(base::js_number(base::js_to_fixed(base::js_to_number(v), 3)));
}
/// .replace(/^'(?=[=+\-@])/, '')
std::string unquote(const std::string& d) {
  if (d.size() >= 2 && d[0] == '\'' && (d[1] == '=' || d[1] == '+' || d[1] == '-' || d[1] == '@')) {
    return d.substr(1);
  }
  return d;
}
std::string norm_text(const Value& v) {
  std::string s = v.truthy() ? base::js_string(v) : std::string();
  std::string o;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\r') {
      o.push_back('\n');
      if (i + 1 < s.size() && s[i + 1] == '\n') {
        ++i;
      }
    } else {
      o.push_back(s[i]);
    }
  }
  return std::string(base::js_trim(o));
}
}  // namespace

std::string points_text(const Value& points, std::string_view order) {
  const bool pene = order == "PENZD";
  std::string out;
  const Array& ps = arr(points);
  for (std::size_t i = 0; i < ps.size(); ++i) {
    const Value& p = ps[i];
    const Value d = get(p, "d");
    out += (i != 0 ? "\r\n" : "") + cell(get(p, "n")) + "," + (pene ? num3(get(p, "x")) : num3(get(p, "y"))) + "," +
           (pene ? num3(get(p, "y")) : num3(get(p, "x"))) + "," + num3(get(p, "z")) + "," +
           cell(d.truthy() ? d : Value(""));
  }
  return out + "\r\n";
}

Value make_clip(const Value& points, const Value& opts, double at) {
  const Array& ps = arr(points);
  Array pts;
  Array first;
  for (std::size_t i = 0; i < ps.size() && i < kMaxClip; ++i) {
    const Value& p = ps[i];
    const Value z = get(p, "z");
    const Value d = get(p, "d");
    pts.emplace_back(Array{Value(base::js_string(get(p, "n"))), get(p, "x"), get(p, "y"),
                           (z.is_null() || z.is_undefined()) ? Value(nullptr) : z, d.truthy() ? d : Value("")});
    first.push_back(p);
  }
  const Value u = get(opts, "units");
  const Value order = get(opts, "order");
  return make_object({{"format", "civilcraft-clip"},
                      {"v", 1},
                      {"units", u.is_string() && u.as_string() == "m" ? "m" : "ft"},
                      {"at", at},
                      {"source", clean(get(opts, "source"), 40)},
                      {"pts", std::move(pts)},
                      {"csv", points_text(Value(std::move(first)), order.is_undefined() ? "PNEZD" : sv(order))}});
}

Value read_clip(const Value& obj) {
  if (!obj.is_object() || obj.is_point()) {
    return nullptr;
  }
  const Value fmt = get(obj, "format");
  const Value v = get(obj, "v");
  const bool ours = fmt.is_string() && fmt.as_string() == "civilcraft-clip";
  const bool v1 = v.is_number() && v.as_number() == 1;
  if (!ours || !v1 || !get(obj, "pts").is_array()) {
    return nullptr;
  }
  std::set<std::string> seen;
  Array pts;
  const Array& rows = arr(get(obj, "pts"));
  for (std::size_t i = 0; i < rows.size() && i < kMaxClip; ++i) {
    const Value& r = rows[i];
    if (!r.is_array()) {
      continue;
    }
    const Array& c = r.as_array();
    auto at = [&](std::size_t k) { return k < c.size() ? c[k] : Value(); };
    if (!fin(at(1)) || !fin(at(2))) {
      continue;
    }
    const std::string n = clean(at(0), 40);
    if (n.empty() || !seen.insert(n).second) {
      continue;
    }
    pts.push_back(make_object({{"n", n},
                               {"x", at(1)},
                               {"y", at(2)},
                               {"z", fin(at(3)) ? at(3) : Value(nullptr)},
                               {"d", unquote(clean(at(4), 200))}}));
  }
  if (pts.empty()) {
    return nullptr;
  }
  const Value u = get(obj, "units");
  const Value at = get(obj, "at");
  const Value csv = get(obj, "csv");
  return make_object({{"format", "civilcraft-clip"},
                      {"v", 1},
                      {"units", u.is_string() && u.as_string() == "m" ? "m" : "ft"},
                      {"at", fin(at) ? at : Value(0)},
                      {"source", clean(get(obj, "source"), 40)},
                      {"points", std::move(pts)},
                      {"csv", csv.is_string() ? csv : Value("")}});
}

Value text_points(const Value& text, std::string_view order, const i18n::Catalog& w) {
  ParsedPoints res = parse_points(sv(text), "auto", order, w);
  for (SurveyPoint& p : res.points) {
    p.d = unquote(p.d);
  }
  Value rv = points_value(res);
  return make_object({{"points", rv.get("points")}, {"res", rv}});
}

Value choose_paste(const Value& own, const Value& system_text, std::string_view order, const i18n::Catalog& w) {
  const bool has_text = system_text.is_string() && !base::js_blank(system_text.as_string());
  if (has_text && !(own.truthy() && norm_text(system_text) == norm_text(get(own, "csv")))) {
    const Value tp = text_points(system_text, order, w);
    if (!arr(tp.get("points")).empty()) {
      return make_object({{"kind", "text"},
                          {"points", tp.get("points")},
                          {"res", tp.get("res")},
                          {"why", w.t(own.truthy() ? "clip.why.newer" : "clip.why.text")}});
    }
    if (!own.truthy()) {
      const Value res = tp.get("res");
      const std::string why = base::js_to_number(res.get("badCount")) != 0
                                  ? base::js_string(arr(res.get("bad"))[0].get("why"))
                                  : w.t("clip.why.norows");
      return make_object({{"kind", "none"}, {"points", Array{}}, {"why", w.t("clip.why.unread", {{"why", why}})}});
    }
  }
  if (own.truthy()) {
    return make_object(
        {{"kind", "clip"}, {"points", get(own, "points")}, {"units", get(own, "units")}, {"why", w.t("clip.why.own")}});
  }
  return make_object({{"kind", "none"}, {"points", Array{}}, {"why", w.t("clip.why.nothing")}});
}

namespace {
std::optional<double> whole(const std::string& n) {
  if (n.empty() || n.size() > 15 || !std::ranges::all_of(n, [](char c) { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }
  return base::js_number(n);
}
}  // namespace

Value renumber(const Value& incoming, const Value& existing, const Value& opts) {
  std::set<std::string> used;
  double high = 0;
  for (const Value& p : arr(existing)) {
    const Value pn = get(p, "n");
    const std::string n = base::js_string((pn.is_null() || pn.is_undefined()) ? p : pn);
    used.insert(n);
    if (const auto w = whole(n); w && *w > high) {
      high = *w;
    }
  }
  const Value mv = get(opts, "mode");
  const bool always = mv.is_string() && mv.as_string() == "always";
  const Array& in = arr(incoming);
  Array out(in.size());
  Array changed;
  std::vector<std::size_t> todo;
  double kept = 0;
  auto with_n = [](const Value& p, const std::string& n) {
    Value o = p.is_object() ? p : Value::object();
    o.set("n", n);
    return o;
  };
  for (std::size_t i = 0; i < in.size(); ++i) {
    const std::string n = base::js_string(get(in[i], "n"));
    if (!always && !used.contains(n)) {
      used.insert(n);
      out[i] = with_n(in[i], n);
      kept++;
      if (const auto w = whole(n); w && *w > high) {
        high = *w;
      }
    } else {
      todo.push_back(i);
    }
  }
  const Value st = get(opts, "start");
  double next = st.is_number() && std::isfinite(st.as_number()) && std::floor(st.as_number()) == st.as_number() &&
                        st.as_number() > 0
                    ? st.as_number()
                    : high + 1;
  for (const std::size_t i : todo) {
    while (used.contains(appkit::num_str(next))) {
      next += 1;
    }
    const std::string n = appkit::num_str(next);
    next += 1;
    used.insert(n);
    changed.emplace_back(Array{Value(base::js_string(get(in[i], "n"))), Value(n)});
    out[i] = with_n(in[i], n);
  }
  return make_object({{"points", std::move(out)}, {"changed", std::move(changed)}, {"kept", kept}});
}

namespace {
std::string ranges(const std::vector<std::string>& list) {
  std::vector<std::string> parts;
  for (std::size_t i = 0; i < list.size();) {
    std::size_t j = i;
    if (whole(list[i])) {
      while (j + 1 < list.size()) {
        const auto a = whole(list[j + 1]);
        const auto b = whole(list[j]);
        const bool next_one = a && b && *a == *b + 1;
        if (!next_one) {
          break;
        }
        ++j;
      }
    }
    parts.push_back(j > i ? list[i] + "–" + list[j] : list[i]);
    i = j + 1;
  }
  std::string out;
  const std::size_t n = std::min<std::size_t>(parts.size(), 4);
  for (std::size_t i = 0; i < n; ++i) {
    out += (i != 0 ? ", " : "") + parts[i];
  }
  return parts.size() > 4 ? out + " …" : out;
}
}  // namespace

std::string renumber_text(const Value& changed, const Value& kept, const i18n::Catalog& w) {
  std::vector<std::string> bits;
  if (kept.truthy()) {
    bits.push_back(w.tn("clip.kept", base::js_to_number(kept), {{"n", base::js_string(kept)}}));
  }
  const Array& c = arr(changed);
  if (!c.empty()) {
    std::vector<std::string> was;
    std::vector<std::string> now;
    for (const Value& x : c) {
      const Array& p = arr(x);
      was.push_back(p.empty() ? "undefined" : base::js_string(p[0]));
      now.push_back(p.size() < 2 ? "undefined" : base::js_string(p[1]));
    }
    const auto n = static_cast<double>(c.size());
    bits.push_back(w.tn("clip.changed", n, {{"n", appkit::num_str(n)}, {"was", ranges(was)}, {"now", ranges(now)}}));
  }
  std::string out;
  for (std::size_t i = 0; i < bits.size(); ++i) {
    out += (i != 0 ? "; " : "") + bits[i];
  }
  return out;
}

Value move_by(const Value& points, const Value& dx, const Value& dy) {
  Array out;
  for (const Value& p : arr(points)) {
    Value o = p.is_object() ? p : Value::object();
    o.set("x", base::js_to_number(get(p, "x")) + base::js_to_number(dx));
    o.set("y", base::js_to_number(get(p, "y")) + base::js_to_number(dy));
    out.push_back(std::move(o));
  }
  return out;
}

Value centre(const Value& points) {
  const Array& ps = arr(points);
  if (ps.empty()) {
    return base::pt2(0, 0);
  }
  auto jmin = [](double a, double b) { return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::min(a, b); };
  auto jmax = [](double a, double b) { return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::max(a, b); };
  double a = std::numeric_limits<double>::infinity();
  double b = std::numeric_limits<double>::infinity();
  double c = -std::numeric_limits<double>::infinity();
  double d = -std::numeric_limits<double>::infinity();
  for (const Value& p : ps) {
    const double x = base::js_to_number(get(p, "x"));
    const double y = base::js_to_number(get(p, "y"));
    a = jmin(a, x);
    b = jmin(b, y);
    c = jmax(c, x);
    d = jmax(d, y);
  }
  return base::pt2((a + c) / 2, (b + d) / 2);
}

// ---------------------------------------------------------------------------------------------- point-edit.js

namespace {
double number_of(const Value& v) {
  if (v.is_number()) {
    return std::isfinite(v.as_number()) ? v.as_number() : std::nan("");
  }
  std::string s(base::js_trim(sv(v)));
  std::erase(s, ',');
  return is_num(s) && s == std::string(base::js_trim(s)) ? base::js_number(s) : std::nan("");
}
}  // namespace

Value check_point_edit(const Value& points, const Value& n, const Value& patch_in, const i18n::Catalog& w) {
  const Value patch = patch_in.is_undefined() ? Value::object() : patch_in;
  const Array& ps = arr(points);
  std::optional<std::size_t> idx;
  for (std::size_t i = 0; i < ps.size(); ++i) {
    if (strict_equal(get(ps[i], "n"), n)) {
      idx = i;
      break;
    }
  }
  if (!idx) {
    const std::string msg = w.t("pt.edit.gone", {{"n", base::js_string(n)}});
    return make_object({{"ok", false}, {"errors", make_object({{"n", msg}})}, {"message", msg}});
  }
  const Value& old = ps[*idx];
  Value next = old.is_object() ? old : Value::object();
  Value errors = Value::object();
  auto has = [&](std::string_view k) { return patch.is_object() && patch.has(k); };
  if (has("n")) {
    const std::string v(base::js_trim(ctrl(sv(patch.get("n")), "")));
    if (v.empty()) {
      errors.set("n", w.t("pt.edit.n.empty"));
    } else if (base::utf16_length(v) > 40) {
      errors.set("n", w.t("pt.edit.n.long"));
    } else if (!strict_equal(Value(v), get(old, "n")) &&
               std::ranges::any_of(ps, [&](const Value& p) { return strict_equal(get(p, "n"), Value(v)); })) {
      errors.set("n", w.t("pt.edit.n.taken", {{"n", v}}));
    } else {
      next.set("n", v);
    }
  }
  for (const char* k : {"x", "y"}) {
    if (!has(k)) {
      continue;
    }
    const double v = number_of(patch.get(k));
    if (!std::isfinite(v) || std::abs(v) >= 1e9) {
      errors.set(k, w.t("pt.edit.coord"));
    } else {
      next.set(k, v);
    }
  }
  if (has("z")) {
    const Value raw = patch.get("z");
    if (raw.is_null() || (raw.is_string() && base::js_blank(raw.as_string()))) {
      next.set("z", nullptr);
    } else {
      const double v = number_of(raw);
      if (!std::isfinite(v) || std::abs(v) >= 1e6) {
        errors.set("z", w.t("pt.edit.z"));
      } else {
        next.set("z", v);
      }
    }
  }
  if (has("d")) {
    const std::string v(base::js_trim(ctrl(sv(patch.get("d")), " ")));
    if (base::utf16_length(v) > 200) {
      errors.set("d", w.t("pt.edit.d.long"));
    } else {
      next.set("d", v);
    }
  }
  if (!errors.as_object().empty()) {
    const Value first = errors.as_object().front().value;
    return make_object({{"ok", false}, {"errors", errors}, {"message", first}});
  }
  auto dd = [&](const Value& o) {
    const Value d = get(o, "d");
    return d.truthy() ? d : Value("");
  };
  if (strict_equal(get(next, "n"), get(old, "n")) && strict_equal(get(next, "x"), get(old, "x")) &&
      strict_equal(get(next, "y"), get(old, "y")) && strict_equal(get(next, "z"), get(old, "z")) &&
      strict_equal(dd(next), dd(old))) {
    return make_object({{"ok", false}, {"errors", Value::object()}, {"message", ""}});
  }
  return make_object(
      {{"ok", true}, {"index", *idx}, {"old", old}, {"point", next}, {"errors", Value::object()}, {"message", ""}});
}

}  // namespace cc::civil
