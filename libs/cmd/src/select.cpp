// SPDX-License-Identifier: Apache-2.0
// commands.js (CadCraft 0.5) in C++, M5.4b: SELECT, QSELECT and SELECTSIMILAR (qselect.js), FIND (findtext.js), on
// the command line (a host with dialogs may add its own later, as the JS does).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cad/edit.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

std::string type_of(const Value& e) {
  return base::js_string(e.get("type"));
}
std::string text_of(const Answer& a) {
  if (a.kind == Answer::Kind::text) {
    return a.text;
  }
  return a.kind == Answer::Kind::typed ? base::js_string(a.typed) : std::string();
}
std::vector<Value> editable_all(const Ctx& c) {
  std::vector<Value> out;
  c.doc.entities().each([&](const Id&, const Value& e) {
    if (c.doc.editable(e)) {
      out.push_back(e);
    }
  });
  return out;
}

// ---- a small matcher for findtext.js's and qselect.js's patterns (UTF-8 → code points)

std::vector<char32_t> to_cps(std::string_view s) {
  std::vector<char32_t> out;
  for (std::size_t i = 0; i < s.size();) {
    const auto b = static_cast<unsigned char>(s[i]);
    std::size_t n = 1;
    char32_t cp = b;
    if (b >= 0xF0) {
      n = 4;
      cp = b & 0x07U;
    } else if (b >= 0xE0) {
      n = 3;
      cp = b & 0x0FU;
    } else if (b >= 0xC0) {
      n = 2;
      cp = b & 0x1FU;
    }
    for (std::size_t k = 1; k < n && i + k < s.size(); ++k) {
      cp = (cp << 6U) | (static_cast<unsigned char>(s[i + k]) & 0x3FU);
    }
    out.push_back(cp);
    i += n;
  }
  return out;
}
std::string from_cps(const std::vector<char32_t>& cps, std::size_t from, std::size_t to) {
  std::string out;
  for (std::size_t i = from; i < to; ++i) {
    const char32_t cp = cps[i];
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0U | (cp >> 6U));
      out += static_cast<char>(0x80U | (cp & 0x3FU));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0U | (cp >> 12U));
      out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
      out += static_cast<char>(0x80U | (cp & 0x3FU));
    } else {
      out += static_cast<char>(0xF0U | (cp >> 18U));
      out += static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
      out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
      out += static_cast<char>(0x80U | (cp & 0x3FU));
    }
  }
  return out;
}
/// A case-insensitive comparison key for one code point (the JS 'i' flag's simple case folding, through the same
/// lower-casing tables as String#toLowerCase).
char32_t fold(char32_t cp) {
  if (cp < 0x80) {
    return cp >= 'A' && cp <= 'Z' ? cp + 32 : cp;
  }
  const std::vector<char32_t> low = to_cps(base::js_to_lower(from_cps({cp}, 0, 1)));
  return low.size() == 1 ? low[0] : cp;
}
/// \p{L}\p{N}_ (a word character for findtext.js's whole-word search)
bool word_char(char32_t cp) {
  if (cp < 0x80) {
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '_';
  }
  // letters and digits beyond ASCII: what changes case is a letter; the rest of Latin-1's symbols are not
  return cp != 0xD7 && cp != 0xF7 && (cp >= 0xC0 || fold(cp) != cp);
}

struct Pattern {
  enum class Tok : std::uint8_t { lit, one, run };
  std::vector<std::pair<Tok, char32_t>> toks;
  bool match_case = false;
  bool whole = false;
  bool dot_all = false;   // ? and * also take a line break (qselect.js's wildcards)
  bool anchored = false;  // ^…$: the whole text
  bool usable = true;     // within the limits below (typed text is untrusted)
};
// matching recurses once per token and backtracks per * run: bounded so typed text cannot exhaust the stack or the
// time (a pattern beyond them matches nothing; docs/gaps.md)
constexpr std::size_t kMaxTokens = 2048;
constexpr std::size_t kMaxRuns = 8;

/// The end of the leftmost-shortest match of p at i (the JS lazy *), or nothing.
std::optional<std::size_t> match_at(  // NOLINT(misc-no-recursion) — bounded by kMaxTokens
    const Pattern& p, const std::vector<char32_t>& s, std::size_t i, std::size_t k, std::size_t start) {
  if (!p.usable) {
    return std::nullopt;
  }
  if (k == p.toks.size()) {
    if ((p.whole && i < s.size() && word_char(s[i])) || (p.anchored && i != s.size())) {
      return std::nullopt;
    }
    (void)start;
    return i;
  }
  const auto [tok, cp] = p.toks[k];
  if (tok == Pattern::Tok::lit) {
    if (i < s.size() && (p.match_case ? s[i] == cp : fold(s[i]) == fold(cp))) {
      return match_at(p, s, i + 1, k + 1, start);
    }
    return std::nullopt;
  }
  if (tok == Pattern::Tok::one) {
    if (i < s.size() && (p.dot_all || s[i] != U'\n')) {
      return match_at(p, s, i + 1, k + 1, start);
    }
    return std::nullopt;
  }
  for (std::size_t j = i; j <= s.size(); ++j) {  // a lazy run: the shortest first
    if (const auto r = match_at(p, s, j, k + 1, start)) {
      return r;
    }
    if (j < s.size() && !p.dot_all && s[j] == U'\n') {
      break;
    }
  }
  return std::nullopt;
}

Pattern make_pattern(const std::string& find, bool wildcard, bool match_case, bool whole, bool dot_all) {
  Pattern p;
  p.match_case = match_case;
  p.whole = whole;
  p.dot_all = dot_all;
  std::size_t runs = 0;
  for (const char32_t cp : to_cps(find)) {
    if (wildcard && cp == U'*') {
      ++runs;
      p.toks.emplace_back(Pattern::Tok::run, 0);
    } else if (wildcard && cp == U'?') {
      p.toks.emplace_back(Pattern::Tok::one, 0);
    } else {
      p.toks.emplace_back(Pattern::Tok::lit, cp);
    }
  }
  p.usable = p.toks.size() <= kMaxTokens && runs <= kMaxRuns;
  return p;
}

/// String(s).match(re) count and s.replace(re, () => rep) for a global pattern
struct Found {
  std::size_t count = 0;
  std::string after;
};
Found find_all(const Pattern& p, const std::string& text, const std::optional<std::string>& rep) {
  const std::vector<char32_t> s = to_cps(text);
  Found f;
  std::size_t i = 0;
  std::size_t copied = 0;
  while (i <= s.size()) {
    if (p.whole && i > 0 && word_char(s[i - 1])) {  // (?<![\p{L}\p{N}_])
      ++i;
      continue;
    }
    const auto end = match_at(p, s, i, 0, i);
    if (!end) {
      ++i;
      continue;
    }
    ++f.count;
    if (rep) {
      f.after += from_cps(s, copied, i) + *rep;
      copied = *end;
    }
    i = *end > i ? *end : i + 1;  // an empty match moves on one character
  }
  if (rep) {
    f.after += from_cps(s, copied, s.size());
  }
  return f;
}

// ---- qselect.js

struct QsProp {
  const char* key;
  std::vector<std::string> types;  // empty = every type
  const char* kind;
};
const std::vector<QsProp>& qs_props() {
  static const std::vector<QsProp> kProps{{"layer", {}, "choice"},
                                          {"color", {}, "color"},
                                          {"height", {"text"}, "number"},
                                          {"contents", {"text"}, "text"},
                                          {"block", {"insert"}, "choice"},
                                          {"radius", {"circle", "arc"}, "number"},
                                          {"length", {"line", "arc", "poly"}, "number"},
                                          {"area", {"circle", "poly", "ellipse", "hatch"}, "number"},
                                          {"pattern", {"hatch"}, "choice"}};
  return kProps;
}
const QsProp* qs_prop(const std::string& k) {
  for (const QsProp& p : qs_props()) {
    if (k == p.key) {
      return &p;
    }
  }
  return nullptr;
}
bool prop_has_type(const QsProp& p, const std::string& t) {
  return p.types.empty() || std::ranges::find(p.types, t) != p.types.end();
}
/// Number.isFinite(v) ? v : null
Value finite_or_null(std::optional<double> v) {
  return v && std::isfinite(*v) ? Value(*v) : Value(nullptr);
}
Value hatch_pattern_name(const Value& e) {
  return e.get("solid").truthy()
             ? Value("SOLID")
             : Value(base::js_to_upper(base::js_string(e.get("pattern").truthy() ? e.get("pattern") : Value(""))));
}
/// QS_PROPS[k].get(e)
Value qs_get(const std::string& k, const Value& e) {
  if (k == "layer") {
    const Value l = e.get("layer");
    return l.is_undefined() || l.is_null() ? Value("0") : l;
  }
  if (k == "color") {
    const Value col = e.get("color");
    return col.is_undefined() ? Value(nullptr) : col;
  }
  if (k == "height") {
    const Value h = e.get("h");
    return h.is_number() ? finite_or_null(h.as_number()) : Value(nullptr);
  }
  if (k == "contents") {
    const Value s = e.get("s");
    return s.is_undefined() || s.is_null() ? Value("") : Value(base::js_string(s));
  }
  if (k == "block") {
    return e.get("block");
  }
  if (k == "radius") {
    const Value r = e.get("r");
    return r.is_number() ? finite_or_null(r.as_number()) : Value(nullptr);
  }
  if (k == "length") {
    return finite_or_null(cad::curve_length(e));
  }
  if (k == "area") {
    const auto a = cad::entity_area(e);
    return finite_or_null(a ? std::optional<double>(a->area) : std::nullopt);
  }
  return hatch_pattern_name(e);
}
std::vector<std::string> props_for(const std::string& type) {
  std::vector<std::string> out;
  for (const QsProp& p : qs_props()) {
    if (p.types.empty() || (type != "any" && prop_has_type(p, type))) {
      out.emplace_back(p.key);
    }
  }
  return out;
}
bool same_color(const Value& a, const Value& b) {
  const Value x = a.is_undefined() ? Value(nullptr) : a;
  const Value y = b.is_undefined() ? Value(nullptr) : b;
  if (x == y && x.is_string() == y.is_string()) {
    return true;
  }
  return x.is_string() && y.is_string() && base::js_to_lower(x.as_string()) == base::js_to_lower(y.as_string());
}

struct QsSpec {
  std::string type, prop, op;
  Value value;
};
bool qs_matches(const Value& e, const QsSpec& spec) {
  const std::string t = type_of(e);
  if (spec.type != "any" && t != spec.type) {
    return false;
  }
  if (spec.op == "all") {
    return true;
  }
  const QsProp* p = qs_prop(spec.prop);
  if (p == nullptr || !prop_has_type(*p, t)) {
    return false;
  }
  const Value v = qs_get(spec.prop, e);
  const std::string kind = p->kind;
  if (v.is_undefined() || v.is_null()) {
    return kind == "color" && spec.value.is_null() && (spec.op == "=" || spec.op == "*");
  }
  if (kind == "color") {
    const bool eq = same_color(v, spec.value);
    return spec.op == "<>" ? !eq : eq;
  }
  if (kind == "number") {
    const double x = base::js_to_number(spec.value);
    if (!std::isfinite(x)) {
      return false;
    }
    const double tol = 1e-9 * std::max(1.0, std::abs(x));
    const double d = base::js_to_number(v);
    if (spec.op == "=") {
      return std::abs(d - x) <= tol;
    }
    if (spec.op == "<>") {
      return std::abs(d - x) > tol;
    }
    if (spec.op == ">") {
      return d > x + tol;
    }
    return spec.op == "<" && d < x - tol;
  }
  const std::string s = base::js_string(v);
  const std::string w = spec.value.is_undefined() || spec.value.is_null() ? std::string() : base::js_string(spec.value);
  if (spec.op == "=") {
    return base::js_to_lower(s) == base::js_to_lower(w);
  }
  if (spec.op == "<>") {
    return base::js_to_lower(s) != base::js_to_lower(w);
  }
  if (spec.op == "*") {  // wild(w): the whole text, * any run, ? one character, case-insensitive, dotAll
    Pattern pat = make_pattern(w, true, false, false, true);
    pat.anchored = true;
    return match_at(pat, to_cps(s), 0, 0, 0).has_value();
  }
  if (spec.op == ">") {
    return base::js_locale_compare_numeric(s, w) > 0;
  }
  return spec.op == "<" && base::js_locale_compare_numeric(s, w) < 0;
}

std::string qs_type_label(const Ctx& c, const std::string& t) {
  return t == "any" ? c.t("qs.any") : c.t("type." + t);
}

Task<void> qselect_cmd(Ctx& c) {
  const std::vector<Value> pool = editable_all(c);
  // the command line: type, property, operator, value
  static constexpr std::array<const char*, 10> kTypes{"line", "poly",  "circle", "arc",   "ellipse",
                                                      "text", "point", "insert", "hatch", "dim"};
  std::vector<std::string> types{"any"};
  for (const char* ty : kTypes) {
    if (std::ranges::any_of(pool, [ty](const Value& e) { return type_of(e) == ty; })) {
      types.emplace_back(ty);
    }
  }
  std::string list;
  for (const std::string& ty : types) {
    list += (list.empty() ? "" : ", ") + qs_type_label(c, ty);
  }
  std::string type;
  for (;;) {
    Ask a;
    a.def = qs_type_label(c, "any");
    const std::string v = base::js_to_lower(
        base::js_trim(text_of(co_await c.string(c.t("cmd.qs.type", {{"list", list}}), std::move(a)))));
    const auto hit = std::ranges::find_if(
        types, [&](const std::string& ty) { return base::js_to_lower(qs_type_label(c, ty)) == v || ty == v; });
    if (hit != types.end()) {
      type = *hit;
      break;
    }
    c.print(c.t("cmd.qs.type.bad"));
  }
  const std::vector<std::string> props = props_for(type);
  std::string plist;
  for (const std::string& k : props) {
    plist += (plist.empty() ? "" : ", ") + c.t("qs.prop." + k);
  }
  std::string prop;
  for (;;) {
    Ask a;
    a.def = c.t("qs.prop.layer");
    const std::string v = base::js_to_lower(
        base::js_trim(text_of(co_await c.string(c.t("cmd.qs.prop", {{"list", plist}}), std::move(a)))));
    const auto hit = std::ranges::find_if(
        props, [&](const std::string& k) { return base::js_to_lower(c.t("qs.prop." + k)) == v || k == v; });
    if (hit != props.end()) {
      prop = *hit;
      break;
    }
    c.print(c.t("cmd.qs.prop.bad"));
  }
  Ask ao;
  ao.options = {c.opt("=", "="), c.opt("<>", "<>"), c.opt(">", ">"),
                c.opt("<", "<"), c.opt("*", "*"),   c.opt("ALL", c.t("qs.op.all"))};
  ao.def = "=";
  const Answer k = co_await c.keyword(c.t("cmd.qs.op"), std::move(ao));
  const std::string op = k.key() == "ALL" ? "all" : k.key();
  Value value(nullptr);
  if (op != "all") {
    const std::string raw(base::js_trim(text_of(co_await c.string(c.t("cmd.qs.value")))));
    const std::string kind = qs_prop(prop)->kind;
    if (kind == "number") {
      const auto len = parse_length(raw, c.doc.meta.get("lunits"));
      value = len ? Value(*len) : Value(base::js_parse_float(raw));
    } else if (kind == "color") {
      const std::string low = base::js_to_lower(raw);
      if (raw.empty() || low == "bylayer" || low == "by layer") {
        value = nullptr;
      } else if (low == "byblock" || low == "by block") {
        value = 0;
      } else if (raw[0] == '#') {
        value = raw;
      } else {
        value = base::js_parse_int(raw);
      }
    } else {
      value = raw;
    }
  }
  const QsSpec spec{.type = type, .prop = prop, .op = op, .value = value};
  IdSet found;
  for (const Value& e : pool) {
    if (qs_matches(e, spec)) {
      found.add(id_of(e));
    }
  }
  const std::size_t n = found.size();
  c.host.set_selection(std::move(found));
  c.print(c.words.tn("cmd.qs.done", static_cast<double>(n)));
}

// ---- SELECTSIMILAR

struct SimKey {
  const char* key;
  bool on;
};
constexpr std::array<SimKey, 5> kSimilar{
    {{"layer", true}, {"color", true}, {"name", true}, {"pattern", true}, {"height", false}}};

Task<void> selectsimilar_cmd(Ctx& c) {
  Value on = c.engine.state.get("similarOn");
  if (!on.is_object()) {
    on = Value::object();
    for (const SimKey& k : kSimilar) {
      on.set(k.key, k.on);
    }
    c.engine.state.set("similarOn", on);
  }
  Answer ids;
  for (;;) {
    Ask as;
    as.options = {c.opt("SE", c.t("opt.settings"))};
    ids = co_await c.select(c.t("cmd.sim.select"), std::move(as));
    if (!ids.is("SE")) {
      break;
    }
    for (;;) {
      std::string list;
      for (const SimKey& k : kSimilar) {
        if (on.get(k.key).truthy()) {
          list += (list.empty() ? "" : ", ") + c.t(std::string("sim.") + k.key);
        }
      }
      c.print(c.t("cmd.sim.now", {{"list", list.empty() ? c.t("cmd.ma.nothing") : list}}));
      Ask ak;
      for (const SimKey& k : kSimilar) {
        ak.options.push_back(
            c.opt(base::js_to_upper(std::string(k.key).substr(0, 2)), c.t(std::string("sim.") + k.key)));
      }
      ak.none = true;
      const Answer kk = co_await c.keyword(c.t("cmd.ma.toggle"), std::move(ak));
      const SimKey* hit = nullptr;
      for (const SimKey& k : kSimilar) {
        if (kk.kind == Answer::Kind::option && base::js_to_upper(std::string(k.key).substr(0, 2)) == kk.option) {
          hit = &k;
          break;
        }
      }
      if (hit == nullptr) {
        break;
      }
      on.set(hit->key, !on.get(hit->key).truthy());
      c.engine.state.set("similarOn", on);
    }
  }
  if (ids.kind != Answer::Kind::set || ids.set.empty()) {
    co_return;
  }
  std::vector<Value> refs;
  for (const Id id : ids.set) {
    if (const Value* e = c.doc.entities().get(id)) {
      refs.push_back(*e);
    }
  }
  IdSet found;
  const auto layer = [](const Value& e) {
    const Value l = e.get("layer");
    return l.is_undefined() || l.is_null() ? Value("0") : l;
  };
  for (const Value& e : editable_all(c)) {
    for (const Value& r : refs) {
      const std::string t = type_of(e);
      if (t != type_of(r)) {
        continue;
      }
      if (on.get("layer").truthy() && !(layer(e) == layer(r))) {
        continue;
      }
      if (on.get("color").truthy() && !same_color(e.get("color"), r.get("color"))) {
        continue;
      }
      if (on.get("name").truthy() && t == "insert" && !(e.get("block") == r.get("block"))) {
        continue;
      }
      if (on.get("pattern").truthy() && t == "hatch" && !(hatch_pattern_name(e) == hatch_pattern_name(r))) {
        continue;
      }
      if (on.get("height").truthy() && t == "text" &&
          std::abs(base::js_to_number(e.get("h")) - base::js_to_number(r.get("h"))) >
              1e-9 * std::max(1.0, base::js_to_number(r.get("h")))) {
        continue;
      }
      found.add(id_of(e));
      break;
    }
  }
  const std::size_t n = found.size();
  c.host.set_selection(std::move(found));
  c.print(c.words.tn("cmd.sim.done", static_cast<double>(n)));
}

// ---- FIND (findtext.js findMatches / applyMatches)

struct Match {
  Id id = 0;
  std::string kind, tag;
  std::size_t count = 0;
  std::optional<std::string> after;
};

std::vector<Match> find_matches(const std::vector<Value>& entities, const std::string& find,
                                const std::optional<std::string>& rep) {
  std::vector<Match> out;
  if (find.empty()) {
    return out;
  }
  const Pattern p = make_pattern(find, false, false, false, false);
  const auto test = [&](const std::string& s) -> std::optional<Found> {
    Found f = find_all(p, s, rep);
    if (f.count == 0) {
      return std::nullopt;
    }
    return f;
  };
  for (const Value& e : entities) {
    const std::string t = type_of(e);
    if (t == "text" || t == "mleader") {
      const Value s = e.get("s");
      if (auto r = test(s.is_undefined() || s.is_null() ? std::string() : base::js_string(s))) {
        out.push_back({.id = id_of(e),
                       .kind = "text",
                       .tag = {},
                       .count = r->count,
                       .after = rep ? std::optional<std::string>(r->after) : std::nullopt});
      }
    } else if (t == "insert" && e.get("attrs").truthy()) {
      const Value attrs = e.get("attrs");
      for (const base::Member* m : base::js_members(attrs.as_object())) {
        if (auto r = test(base::js_string(m->value))) {
          out.push_back({.id = id_of(e),
                         .kind = "attr",
                         .tag = m->key,
                         .count = r->count,
                         .after = rep ? std::optional<std::string>(r->after) : std::nullopt});
        }
      }
    } else if (t == "dim" && e.get("text").truthy()) {
      // only the words, never the <> that stands for the measurement
      const std::string text = base::js_string(e.get("text"));
      std::vector<std::string> parts;
      for (std::size_t from = 0;;) {
        const std::size_t at = text.find("<>", from);
        parts.push_back(text.substr(from, at == std::string::npos ? std::string::npos : at - from));
        if (at == std::string::npos) {
          break;
        }
        from = at + 2;
      }
      std::size_t count = 0;
      std::string after;
      bool any = false;
      for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto r = test(parts[i]);
        any = any || r.has_value();
        count += r ? r->count : 0;
        after += (i != 0 ? "<>" : "") + (r && rep ? r->after : parts[i]);
      }
      if (any) {
        out.push_back({.id = id_of(e),
                       .kind = "dim",
                       .tag = {},
                       .count = count,
                       .after = rep ? std::optional<std::string>(after) : std::nullopt});
      }
    }
  }
  return out;
}

std::size_t apply_matches(Ctx& c, const std::vector<Match>& ms) {
  std::vector<Id> order;
  for (const Match& m : ms) {
    if (m.after && std::ranges::find(order, m.id) == order.end()) {
      order.push_back(m.id);
    }
  }
  std::size_t n = 0;
  for (const Id id : order) {
    const Value* ep = c.doc.entities().get(id);
    if (ep == nullptr || !c.doc.editable(*ep)) {
      continue;
    }
    Value next = *ep;
    for (const Match& m : ms) {
      if (m.id != id || !m.after) {
        continue;
      }
      if (m.kind == "text") {
        next.set("s", *m.after);
      } else if (m.kind == "dim") {
        next.set("text", *m.after);
        next.erase("block");  // drawn again with its new words
      } else {
        Value attrs = next.get("attrs");
        attrs.set(m.tag, *m.after);
        next.set("attrs", attrs);
      }
    }
    // a text cannot end up empty (it would vanish): such a replacement is skipped
    if (type_of(next) == "text" && base::js_blank(base::js_string(next.get("s")))) {
      continue;
    }
    (void)c.doc.update(id, next);
    ++n;
  }
  return n;
}

Task<void> find_cmd(Ctx& c) {
  std::vector<Value> pool;
  for (const Id id : c.host.selection()) {
    if (const Value* e = c.doc.entities().get(id)) {
      pool.push_back(*e);
    }
  }
  if (pool.empty()) {
    c.doc.entities().each([&](const Id&, const Value& e) { pool.push_back(e); });
  }
  std::erase_if(pool, [&c](const Value& e) { return !c.doc.editable(e); });
  const std::string find = text_of(co_await c.string(c.t("cmd.find.what")));
  if (find.empty()) {
    co_return;
  }
  const std::string rep = text_of(co_await c.string(c.t("cmd.find.with")));
  const std::optional<std::string> replace = rep.empty() ? std::nullopt : std::optional<std::string>(rep);
  const std::vector<Match> ms = find_matches(pool, find, replace);
  if (ms.empty()) {
    c.print(c.t("cmd.find.none", {{"find", find}}));
    co_return;
  }
  std::size_t total = 0;
  for (const Match& m : ms) {
    total += m.count;
  }
  if (replace) {
    std::size_t n = 0;
    c.doc.change("FIND", [&] { n = apply_matches(c, ms); });
    c.print(c.words.tn("cmd.find.replaced", static_cast<double>(total), {{"m", std::to_string(n)}}));
    co_return;
  }
  IdSet sel;
  for (const Match& m : ms) {
    sel.add(m.id);
  }
  const std::size_t objs = sel.size();
  c.host.set_selection(std::move(sel));
  c.print(c.words.tn("cmd.find.found", static_cast<double>(total), {{"m", std::to_string(objs)}}));
}

// ---- SELECT

Task<void> select_cmd(Ctx& c) {
  Ask a;
  a.fresh = true;
  const Answer ids = co_await c.select({}, std::move(a));
  const std::size_t n = ids.set.size();
  c.host.set_selection(ids.set);
  c.print(c.words.tn("sel.select.done", static_cast<double>(n)));
}

}  // namespace

void add_select(std::vector<CommandDef>& t) {
  const auto keep = [&t](CommandDef d) {
    d.keep_selection = true;
    t.push_back(std::move(d));
  };
  keep(def("QSELECT", {"QS"}, qselect_cmd, "cmd.qs.help", true));
  keep(def("SELECTSIMILAR", {"SIM"}, selectsimilar_cmd, "cmd.sim.help"));
  keep(def("FIND", {}, find_cmd, "cmd.find.help", true));
  keep(def("SELECT", {}, select_cmd, "sel.select.help", true));
}

}  // namespace cc::cmd::detail
