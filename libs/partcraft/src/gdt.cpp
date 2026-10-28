// SPDX-License-Identifier: Apache-2.0
// gdt.js (PartCraft 0.5): GD&T and surface-finish annotations, as vector geometry.
#include "cc/partcraft/gdt.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/partcraft/font.hpp"
#include "jsx.hpp"

namespace cc::partcraft::gdt {
namespace {

using base::Value;
constexpr double PI = std::numbers::pi;
constexpr double kH = 3.5, kFrame = 0.35, kLeader = 0.25, kArrowL = 3, kArrowW = 0.9, kPad = 1.2;
constexpr double kDiaW = 0.9 * kH, kModW = 1.05 * kH;

struct Char {
  std::string_view key, label_key, datums;  // datums: none | optional | required
  bool dia, mod;
};
constexpr std::array<Char, 14> kChars = {{
    {"straightness", "model.gdt.char.straightness", "none", true, true},
    {"flatness", "model.gdt.char.flatness", "none", false, false},
    {"circularity", "model.gdt.char.circularity", "none", false, false},
    {"cylindricity", "model.gdt.char.cylindricity", "none", false, false},
    {"profileLine", "model.gdt.char.profile_line", "optional", false, false},
    {"profileSurface", "model.gdt.char.profile_surface", "optional", false, false},
    {"perpendicularity", "model.gdt.char.perpendicularity", "required", true, true},
    {"angularity", "model.gdt.char.angularity", "required", true, true},
    {"parallelism", "model.gdt.char.parallelism", "required", true, true},
    {"position", "model.gdt.char.position", "required", true, true},
    {"concentricity", "model.gdt.char.concentricity", "required", true, false},
    {"symmetry", "model.gdt.char.symmetry", "required", false, false},
    {"circularRunout", "model.gdt.char.circular_runout", "required", false, false},
    {"totalRunout", "model.gdt.char.total_runout", "required", false, false},
}};

/// What `obj[name]` gives on a plain object for a name it inherits: the function's String(), or nothing.
std::optional<std::string_view> inherited(const Value& name) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 12> kProto = {{
      {"constructor", "function Object() { [native code] }"},
      {"hasOwnProperty", "function hasOwnProperty() { [native code] }"},
      {"isPrototypeOf", "function isPrototypeOf() { [native code] }"},
      {"propertyIsEnumerable", "function propertyIsEnumerable() { [native code] }"},
      {"toLocaleString", "function toLocaleString() { [native code] }"},
      {"toString", "function toString() { [native code] }"},
      {"valueOf", "function valueOf() { [native code] }"},
      {"__proto__", "[object Object]"},
      {"__defineGetter__", "function __defineGetter__() { [native code] }"},
      {"__defineSetter__", "function __defineSetter__() { [native code] }"},
      {"__lookupGetter__", "function __lookupGetter__() { [native code] }"},
      {"__lookupSetter__", "function __lookupSetter__() { [native code] }"},
  }};
  const std::string k = base::js_string(name);
  for (const auto& [n, s] : kProto) {
    if (k == n) {
      return s;
    }
  }
  return std::nullopt;
}

/// CHARACTERISTICS[name]: a characteristic, a prototype member (truthy, every field undefined) or nothing.
struct CharRef {
  const Char* c = nullptr;
  bool proto = false;
  [[nodiscard]] bool found() const { return c != nullptr || proto; }
};
CharRef char_of(const Value& name) {
  const std::string k = base::js_string(name);
  for (const Char& c : kChars) {
    if (k == c.key) {
      return {.c = &c, .proto = false};
    }
  }
  return {.c = nullptr, .proto = inherited(name).has_value()};
}
/// label fill: the characteristic's words, or none (a prototype member's label is undefined: "{label}" stays).
std::vector<std::pair<std::string_view, std::string>> label_fill(const CharRef& c, const i18n::Catalog& w) {
  if (c.c != nullptr) {
    return {{"label", w.t(c.c->label_key)}};
  }
  return {};
}

/// PROCESSES[name]: its words, a prototype member's String(), or nothing.
std::optional<std::string> process_of(const Value& name, const i18n::Catalog& w) {
  const std::string k = base::js_string(name);
  if (k == "any") {
    return w.t("model.gdt.process.any");
  }
  if (k == "removal") {
    return w.t("model.gdt.process.removal");
  }
  if (k == "noRemoval") {
    return w.t("model.gdt.process.no_removal");
  }
  if (const auto p = inherited(name)) {
    return std::string(*p);
  }
  return std::nullopt;
}

constexpr std::string_view kDatumLetters = "ABCDEFGHJKLMNPRSTUVWXYZ";
bool datum_letter(const Value& v) {
  return v.is_string() && v.as_string().size() == 1 && kDatumLetters.contains(v.as_string()[0]);
}
bool view_name(const Value& v) {
  if (!v.is_string()) {
    return false;
  }
  constexpr std::array<std::string_view, 6> kViews{"front", "top", "right", "iso", "section", "detail"};
  return std::ranges::find(kViews, v.as_string()) != kViews.end();
}
bool mod_value(const Value& v) {
  return v.is_string() && (v.as_string().empty() || v.as_string() == "M" || v.as_string() == "L");
}
bool fin(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number());
}

/// The translation with fills built at run time (Catalog::t takes an initializer list).
std::string tr(const i18n::Catalog& w, std::string_view key,
               const std::vector<std::pair<std::string_view, std::string>>& f) {
  switch (f.size()) {
    case 0: return w.t(key);
    case 1: return w.t(key, {f[0]});
    case 2: return w.t(key, {f[0], f[1]});
    default: return w.t(key, {f[0], f[1], f[2]});
  }
}

/// String(Number(v.toFixed(4)))
std::string num4(double v) {
  return base::js_string(Value(base::js_number(base::js_to_fixed(v, 4))));
}

/// clean(s, max) with the C1 controls too: [\u0000-\u001f\u007f-\u009f] out, then max UTF-16 units.
std::string clean(const Value& s, std::size_t max) {
  const std::string in = jsx::text_of(s);
  std::string out;
  for (std::size_t i = 0; i < in.size(); ++i) {
    const auto u = static_cast<unsigned char>(in[i]);
    if (u < 0x20 || u == 0x7f) {
      continue;
    }
    if (u == 0xC2 && i + 1 < in.size()) {  // U+0080..U+009F
      const auto v = static_cast<unsigned char>(in[i + 1]);
      if (v >= 0x80 && v <= 0x9f) {
        ++i;
        continue;
      }
    }
    out += in[i];
  }
  return jsx::utf16_slice(out, max);
}

/// d && d[key]
Value and_get(const Value& d, std::string_view key) {
  return d.truthy() ? jsx::get(d, key) : d;
}
/// v ?? fallback
Value nullish(const Value& v, const Value& fallback) {
  return v.is_null() || v.is_undefined() ? fallback : v;
}

}  // namespace

base::Result<Value> new_annotation(const Value& kind, const Value& props, const i18n::Catalog& w) {
  const auto or_ = [&](std::string_view k, const Value& def) {
    const Value v = jsx::get(props, k);
    return v.truthy() ? v : def;
  };
  const auto copy = [&](std::string_view k, const Value& def) {
    const Value v = jsx::get(props, k);
    if (!v.truthy()) {
      return def;
    }
    return v.is_array() ? v : Value(base::Array{});
  };
  Value base = base::make_object({{"id", or_("id", "g1")},
                                  {"kind", kind},
                                  {"view", or_("view", "front")},
                                  {"at", copy("at", base::Array{0, 0})},
                                  {"off", copy("off", base::Array{12, 10})}});
  const std::string k = kind.is_string() ? kind.as_string() : std::string();
  if (kind.is_string() && k == "datum") {
    base.append("letter", or_("letter", "A"));
    return base;
  }
  if (kind.is_string() && k == "fcf") {
    const Value ch = or_("char", "position");
    const CharRef c = char_of(ch);
    base.append("char", ch);
    base.append("dia", nullish(jsx::get(props, "dia"), Value(ch.is_string() && ch.as_string() == "position")));
    base.append("tol", nullish(jsx::get(props, "tol"), Value(0.1)));
    base.append("mod", or_("mod", ""));
    const Value ds = jsx::get(props, "datums");
    base::Array datums;
    if (ds.truthy()) {
      if (!ds.is_array()) {
        return base::fail(base::Errc::invalid, "props.datums.map is not a function");
      }
      for (const Value& d : ds.as_array()) {
        if (d.is_null() || d.is_undefined()) {
          return base::fail(base::Errc::invalid, std::string("Cannot read properties of ") +
                                                     (d.is_null() ? "null" : "undefined") + " (reading 'letter')");
        }
        const Value m = jsx::get(d, "mod");
        datums.push_back(base::make_object({{"letter", jsx::get(d, "letter")}, {"mod", m.truthy() ? m : Value("")}}));
      }
    } else if (c.c != nullptr && c.c->datums == "required") {
      datums.push_back(base::make_object({{"letter", "A"}, {"mod", ""}}));
    }
    base.append("datums", std::move(datums));
    return base;
  }
  if (kind.is_string() && k == "finish") {
    base.append("ra", nullish(jsx::get(props, "ra"), Value(1.6)));
    base.append("process", or_("process", "removal"));
    base.append("note", or_("note", ""));
    return base;
  }
  return base::fail(base::Errc::invalid, w.t("model.gdt.unknown_kind"));
}

std::optional<std::string> validate(const Value& a, const i18n::Catalog& w) {
  if (!a.truthy() || (!a.is_object() && !a.is_array())) {
    return w.t("model.gdt.missing");
  }
  if (!view_name(jsx::get(a, "view"))) {
    return w.t("model.gdt.unknown_view");
  }
  const Value kind = jsx::get(a, "kind");
  const std::string k = kind.is_string() ? kind.as_string() : std::string();
  if (kind.is_string() && k == "datum") {
    return datum_letter(jsx::get(a, "letter")) ? std::nullopt : std::optional(w.t("model.gdt.datum_letter"));
  }
  if (kind.is_string() && k == "finish") {
    const Value ra = jsx::get(a, "ra");
    if (!fin(ra) || ra.as_number() <= 0 || ra.as_number() > 1000) {
      return w.t("model.gdt.ra_range");
    }
    if (!process_of(jsx::get(a, "process"), w)) {
      return w.t("model.gdt.process_unknown");
    }
    const Value note = jsx::get(a, "note");
    if (!note.is_string() || base::utf16_length(note.as_string()) > 40) {
      return w.t("model.gdt.note_long");
    }
    return std::nullopt;
  }
  if (!(kind.is_string() && k == "fcf")) {
    return w.t("model.gdt.unknown_kind");
  }
  const CharRef c = char_of(jsx::get(a, "char"));
  if (!c.found()) {
    return w.t("model.gdt.unknown_char");
  }
  const Value tol = jsx::get(a, "tol");
  if (!fin(tol) || tol.as_number() <= 0 || tol.as_number() > 1000) {
    return w.t("model.gdt.tol_range");
  }
  const bool cdia = c.c != nullptr && c.c->dia;
  const bool cmod = c.c != nullptr && c.c->mod;
  if (jsx::get(a, "dia").truthy() && !cdia) {
    return tr(w, "model.gdt.no_dia", label_fill(c, w));
  }
  const Value mod = jsx::get(a, "mod");
  if (!mod_value(mod)) {
    return w.t("model.gdt.mod_values");
  }
  if (mod.truthy() && !cmod) {
    return tr(w, "model.gdt.no_mod", label_fill(c, w));
  }
  const Value datums = jsx::get(a, "datums");
  if (!datums.is_array() || datums.as_array().size() > 3) {
    return w.t("model.gdt.max_datums");
  }
  const std::string_view rule = c.c != nullptr ? c.c->datums : std::string_view{};
  if (rule == "none" && !datums.as_array().empty()) {
    return tr(w, "model.gdt.form_no_datums", label_fill(c, w));
  }
  if (rule == "required" && datums.as_array().empty()) {
    return tr(w, "model.gdt.needs_datum", label_fill(c, w));
  }
  std::set<std::string> seen;
  for (const Value& d : datums.as_array()) {
    if (!d.truthy() || !datum_letter(jsx::get(d, "letter"))) {
      return w.t("model.gdt.datum_ref_letter");
    }
    const std::string letter = jsx::get(d, "letter").as_string();
    if (seen.contains(letter)) {
      return w.t("model.gdt.datum_twice", {{"letter", letter}});
    }
    seen.insert(letter);
    const Value dm = jsx::get(d, "mod");
    if (!mod_value(dm)) {
      return w.t("model.gdt.datum_mod_values");
    }
    if (dm.truthy() && !cmod) {
      return tr(w, "model.gdt.no_datum_mod", label_fill(c, w));
    }
  }
  return std::nullopt;
}

base::Result<Value> read_notes(const Value& raw, const i18n::Catalog& w) {
  if (raw.is_null() || raw.is_undefined()) {
    return Value(base::Array{});
  }
  if (!raw.is_array()) {
    return base::fail(base::Errc::invalid, w.t("model.gdt.notes_not_list"));
  }
  if (raw.as_array().size() > 500) {
    return base::fail(base::Errc::invalid, w.t("model.gdt.notes_too_many"));
  }
  std::set<std::string> ids;
  const auto pair = [](const Value& v, double lim) {
    if (!v.is_array() || v.as_array().size() != 2) {
      return false;
    }
    return std::ranges::all_of(v.as_array(),
                               [lim](const Value& x) { return fin(x) && std::abs(x.as_number()) <= lim; });
  };
  const auto id_ok = [](const Value& v) {
    if (!v.is_string()) {
      return false;
    }
    const std::string& s = v.as_string();
    if (s.size() < 2 || s.size() > 10 || s[0] != 'g') {
      return false;
    }
    return std::ranges::all_of(s.substr(1), [](char c) { return c >= '0' && c <= '9'; });
  };
  base::Array out;
  std::size_t i = 0;
  for (const Value& n : raw.as_array()) {
    const std::string idx = std::to_string(i + 1);
    const auto bad = [&](std::string_view key) { return base::fail(base::Errc::invalid, w.t(key, {{"n", idx}})); };
    if (!n.truthy() || (!n.is_object() && !n.is_array())) {
      return bad("model.gdt.note.not_note");
    }
    const Value id = jsx::get(n, "id");
    if (!id_ok(id) || ids.contains(id.as_string())) {
      return bad("model.gdt.note.bad_id");
    }
    ids.insert(id.as_string());
    const Value kind = jsx::get(n, "kind");
    const std::string k = kind.is_string() ? kind.as_string() : std::string();
    if (!kind.is_string() || (k != "datum" && k != "fcf" && k != "finish")) {
      return bad("model.gdt.note.unknown_kind");
    }
    if (!view_name(jsx::get(n, "view"))) {
      return bad("model.gdt.note.unknown_view");
    }
    if (!pair(jsx::get(n, "at"), 1e6)) {
      return bad("model.gdt.note.bad_at");
    }
    if (!pair(jsx::get(n, "off"), 2000)) {
      return bad("model.gdt.note.bad_off");
    }
    Value a = base::make_object({{"id", id},
                                 {"kind", kind},
                                 {"view", jsx::get(n, "view")},
                                 {"at", jsx::get(n, "at")},
                                 {"off", jsx::get(n, "off")}});
    if (k == "datum") {
      a.append("letter", clean(jsx::get(n, "letter"), 2));
    } else if (k == "finish") {
      a.append("ra", jsx::get(n, "ra"));
      a.append("process", jsx::get(n, "process"));
      a.append("note", clean(jsx::get(n, "note"), 40));
    } else {
      const Value ds = jsx::get(n, "datums");
      if (!ds.is_array() || ds.as_array().size() > 3) {
        return bad("model.gdt.note.bad_datums");
      }
      a.append("char", jsx::get(n, "char"));
      a.append("dia", jsx::get(n, "dia") == Value(true));
      a.append("tol", jsx::get(n, "tol"));
      a.append("mod", nullish(jsx::get(n, "mod"), Value("")));
      base::Array datums;
      for (const Value& d : ds.as_array()) {
        datums.push_back(base::make_object(
            {{"letter", clean(and_get(d, "letter"), 2)}, {"mod", nullish(and_get(d, "mod"), Value(""))}}));
      }
      a.append("datums", std::move(datums));
    }
    if (auto why = validate(a, w)) {
      std::string s = *why;
      if (!s.empty() && s.back() == '.') {
        s.pop_back();
      }
      if (!s.empty() && s[0] != '\n' && s[0] != '\r') {  // /^./ : the first character, lower case
        std::size_t len = 1;
        const auto c0 = static_cast<unsigned char>(s[0]);
        if (c0 >= 0xF0) {
          len = 0;  // an astral first character is a surrogate pair: its lead unit has no case
        } else if (c0 >= 0xE0) {
          len = 3;
        } else if (c0 >= 0xC0) {
          len = 2;
        }
        if (len > 0) {
          s = base::js_to_lower(s.substr(0, len)) + s.substr(len);
        }
      }
      return base::fail(base::Errc::invalid, w.t("model.gdt.note.invalid", {{"n", idx}, {"why", s}}));
    }
    out.push_back(std::move(a));
    ++i;
  }
  return Value(std::move(out));
}

base::Result<std::string> describe(const Value& a, const i18n::Catalog& w) {
  const std::string kind = base::js_string(jsx::get(a, "kind"));
  if (kind == "datum") {
    return w.t("model.gdt.describe.datum", {{"letter", base::js_string(jsx::get(a, "letter"))}});
  }
  if (kind == "finish") {
    const std::string ra = num4(jsx::get(a, "ra").as_number());
    const auto proc = process_of(jsx::get(a, "process"), w);
    const Value note = jsx::get(a, "note");
    std::vector<std::pair<std::string_view, std::string>> f{{"ra", ra}};
    if (proc) {
      f.emplace_back("process", *proc);
    }
    if (note.truthy()) {
      f.emplace_back("note", base::js_string(note));
      return tr(w, "model.gdt.describe.finish_note", f);
    }
    return tr(w, "model.gdt.describe.finish", f);
  }
  const CharRef c = char_of(jsx::get(a, "char"));
  if (c.c == nullptr) {
    return base::fail(base::Errc::invalid, c.proto ? "Cannot read properties of undefined (reading 'replace')"
                                                   : "Cannot read properties of undefined (reading 'label')");
  }
  const auto m = [](const Value& x) { return x.truthy() ? " (" + base::js_string(x) + ")" : std::string(); };
  std::string label = w.t(c.c->label_key);
  // label.replace(/ \(.*\)$/, ''): the first " (" whose rest runs to the end on one line and ends with ")"
  for (std::size_t at = label.find(" ("); at != std::string::npos; at = label.find(" (", at + 1)) {
    const std::string_view rest = std::string_view(label).substr(at);
    if (rest.ends_with(')') && !rest.contains('\n') && !rest.contains('\r') && !rest.contains("\xE2\x80\xA8") &&
        !rest.contains("\xE2\x80\xA9")) {
      label.resize(at);
      break;
    }
  }
  const std::string tol = num4(jsx::get(a, "tol").as_number());
  std::string out = (jsx::get(a, "dia").truthy() ? w.t("model.gdt.describe.tol_dia", {{"label", label}, {"tol", tol}})
                                                 : w.t("model.gdt.describe.tol", {{"label", label}, {"tol", tol}})) +
                    m(jsx::get(a, "mod"));
  for (const Value& d : jsx::get(a, "datums").as_array()) {
    out += " | " + base::js_string(jsx::get(d, "letter")) + m(jsx::get(d, "mod"));
  }
  return out;
}

double text_width(std::string_view s, double size) {
  return font::advance(s) / 1000 * size;
}

namespace {

using P2 = std::array<double, 2>;
using Seg = std::array<double, 4>;

std::vector<P2> circle_pts(P2 c, double r, double n = 32, double a0 = 0, double sweep = 2 * PI) {
  std::vector<P2> pts;
  for (std::int64_t k = 0; static_cast<double>(k) <= n; ++k) {
    const double t = a0 + sweep * static_cast<double>(k) / n;
    pts.push_back({c[0] + r * base::js_cos(t), c[1] + r * base::js_sin(t)});
  }
  return pts;
}
void poly(const std::vector<P2>& pts, std::vector<Seg>& segs) {
  for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
    segs.push_back({pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1]});
  }
}

struct Text {
  double x, y, size;
  std::string text;
  std::string_view anchor;
};

void arrow_head(std::vector<std::vector<P2>>& fills, P2 tip, P2 from) {
  const double dx = tip[0] - from[0];
  const double dy = tip[1] - from[1];
  double l = base::js_hypot(dx, dy);
  if (l == 0 || std::isnan(l)) {
    l = 1;
  }
  const double ux = dx / l;
  const double uy = dy / l;
  const double L = kArrowL;
  const double w = kArrowW;
  fills.push_back({{tip[0], tip[1]},
                   {tip[0] - ux * L - uy * w, tip[1] - uy * L + ux * w},
                   {tip[0] - ux * L + uy * w, tip[1] - uy * L - ux * w}});
}

base::Result<void> char_symbol(std::string_view key, P2 c, std::vector<Seg>& segs,
                               std::vector<std::vector<P2>>& fills) {
  const double r = 0.75 * kH;
  const auto [cx, cy] = c;
  const auto L = [&](double x1, double y1, double x2, double y2) {
    segs.push_back({cx + x1 * r, cy + y1 * r, cx + x2 * r, cy + y2 * r});
  };
  const auto C = [&](double x, double y, double rr, double a0 = 0, double sw = 2 * PI, double n = 32) {
    poly(circle_pts({cx + x * r, cy + y * r}, rr * r, n, a0, sw), segs);
  };
  const double s60 = base::js_sin(PI / 3);
  const double c60 = base::js_cos(PI / 3);
  if (key == "straightness") {
    L(-1, 0, 1, 0);
  } else if (key == "flatness") {
    L(-1, -0.4, 0.55, -0.4);
    L(0.55, -0.4, 1, 0.4);
    L(1, 0.4, -0.55, 0.4);
    L(-0.55, 0.4, -1, -0.4);
  } else if (key == "circularity") {
    C(0, 0, 0.85);
  } else if (key == "cylindricity") {
    C(0, 0, 0.55);
    const double nx = -s60;
    const double ny = c60;
    for (const double s : {1.0, -1.0}) {
      L(nx * 0.55 * s - c60 * 1.1, ny * 0.55 * s - s60 * 1.1, nx * 0.55 * s + c60 * 1.1, ny * 0.55 * s + s60 * 1.1);
    }
  } else if (key == "profileLine") {
    C(0, -0.45, 0.95, 0, PI, 24);
  } else if (key == "profileSurface") {
    C(0, -0.45, 0.95, 0, PI, 24);
    L(-0.95, -0.45, 0.95, -0.45);
  } else if (key == "perpendicularity") {
    L(-0.9, -0.85, 0.9, -0.85);
    L(0, -0.85, 0, 0.95);
  } else if (key == "angularity") {
    const double len = 2.1;
    const double a = PI / 6;
    L(-0.95, -0.75, 0.95, -0.75);
    L(-0.95, -0.75, -0.95 + len * base::js_cos(a), -0.75 + len * base::js_sin(a));
  } else if (key == "parallelism") {
    for (const double dx : {-0.35, 0.35}) {
      L(dx - c60 * 1.05, -s60 * 1.05, dx + c60 * 1.05, s60 * 1.05);
    }
  } else if (key == "position") {
    C(0, 0, 0.6);
    L(-1, 0, 1, 0);
    L(0, -1, 0, 1);
  } else if (key == "concentricity") {
    C(0, 0, 0.4);
    C(0, 0, 0.9);
  } else if (key == "symmetry") {
    L(-0.6, 0.5, 0.6, 0.5);
    L(-1, 0, 1, 0);
    L(-0.6, -0.5, 0.6, -0.5);
  } else if (key == "circularRunout" || key == "totalRunout") {
    const bool total = key == "totalRunout";
    const std::vector<double> xs = total ? std::vector<double>{-0.75, 0.15} : std::vector<double>{-0.4};
    for (const double x0 : xs) {
      const P2 a{cx + x0 * r, cy - 0.9 * r};
      const P2 b{cx + (x0 + 1.1) * r, cy + 0.6 * r};
      segs.push_back({a[0], a[1], b[0], b[1]});
      const double ux = (b[0] - a[0]);
      const double uy = (b[1] - a[1]);
      const double l = base::js_hypot(ux, uy);
      const P2 tip{b[0] + ux / l * 0.45 * r, b[1] + uy / l * 0.45 * r};
      const double k = 0.5 * r;
      const double w = 0.22 * r;
      fills.push_back({tip,
                       {tip[0] - ux / l * k - uy / l * w, tip[1] - uy / l * k + ux / l * w},
                       {tip[0] - ux / l * k + uy / l * w, tip[1] - uy / l * k - ux / l * w}});
    }
    if (total) {
      L(-0.75, -0.9, 0.15, -0.9);
    }
  } else {
    return base::fail(base::Errc::invalid, "Unknown characteristic " + std::string(key));
  }
  return {};
}

void dia_symbol(P2 c, std::vector<Seg>& segs) {
  const double r = 0.36 * kH;
  poly(circle_pts(c, r, 32), segs);
  const double l = r * 1.35;
  const double a = PI / 3;
  segs.push_back(
      {c[0] - l * base::js_cos(a), c[1] - l * base::js_sin(a), c[0] + l * base::js_cos(a), c[1] + l * base::js_sin(a)});
}

void mod_symbol(std::string_view letter, P2 c, std::vector<Seg>& segs) {
  const double r = 0.48 * kH;
  poly(circle_pts(c, r, 32), segs);
  const double u = r * 0.5;
  const auto [x, y] = c;
  if (letter == "M") {
    poly({{x - u, y - u}, {x - u, y + u}, {x, y - 0.1 * u}, {x + u, y + u}, {x + u, y - u}}, segs);
  } else {
    poly({{x - 0.6 * u, y + u}, {x - 0.6 * u, y - u}, {x + 0.75 * u, y - u}}, segs);
  }
}

double jmin(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return a < b ? a : b;
}

std::array<double, 4> box_of(const std::vector<Seg>& frame, const std::vector<Seg>& leader,
                             const std::vector<std::vector<P2>>& fills, const std::vector<Text>& texts) {
  constexpr double inf = std::numeric_limits<double>::infinity();
  double x0 = inf;
  double y0 = inf;
  double x1 = -inf;
  double y1 = -inf;
  const auto add = [&](double x, double y) {
    x0 = jmin(x0, x);
    x1 = jsx::max(x1, x);
    y0 = jmin(y0, y);
    y1 = jsx::max(y1, y);
  };
  for (const auto* s : {&frame, &leader}) {
    for (const Seg& g : *s) {
      add(g[0], g[1]);
      add(g[2], g[3]);
    }
  }
  for (const auto& f : fills) {
    for (const P2& p : f) {
      add(p[0], p[1]);
    }
  }
  for (const Text& t : texts) {
    const double w = text_width(t.text, t.size);
    double dx = 0;
    if (t.anchor == "middle") {
      dx = -w / 2;
    } else if (t.anchor == "end") {
      dx = -w;
    }
    add(t.x + dx, t.y - 0.25 * t.size);
    add(t.x + dx + w, t.y + 0.75 * t.size);
  }
  return {x0, y0, x1, y1};
}

P2 nearest_on_box(const std::array<double, 4>& b, P2 p) {
  return {jmin(b[2], jsx::max(b[0], p[0])), jmin(b[3], jsx::max(b[1], p[1]))};
}
bool inside(const std::array<double, 4>& b, P2 p) {
  return p[0] > b[0] && p[0] < b[2] && p[1] > b[1] && p[1] < b[3];
}

Value pts_value(const std::vector<P2>& pts) {
  base::Array a;
  for (const P2& p : pts) {
    a.emplace_back(base::Array{p[0], p[1]});
  }
  return a;
}
Value segs_value(const std::vector<Seg>& segs) {
  base::Array a;
  for (const Seg& s : segs) {
    a.emplace_back(base::Array{s[0], s[1], s[2], s[3]});
  }
  return a;
}
Value box_value(const std::array<double, 4>& b) {
  return base::Array{b[0], b[1], b[2], b[3]};
}

double js_sign(double x) {
  if (std::isnan(x) || x == 0) {
    return x;
  }
  return x > 0 ? 1 : -1;
}

}  // namespace

base::Result<Value> annotation_geometry(const Value& a, const std::array<double, 2>& anchor, const i18n::Catalog& w) {
  const double h = kH;
  std::vector<Seg> frame;
  std::vector<Seg> leader;
  std::vector<std::vector<P2>> fills;
  std::vector<Text> texts;
  const Value off_v = jsx::get(a, "off");
  const P2 off{base::js_to_number(off_v.as_array().at(0)), base::js_to_number(off_v.as_array().at(1))};
  const P2 P{anchor[0] + off[0], anchor[1] + off[1]};
  Value out = Value::object();
  const auto rect = [](double x0, double y0, double x1, double y1, std::vector<Seg>& segs) {
    segs.push_back({x0, y0, x1, y0});
    segs.push_back({x1, y0, x1, y1});
    segs.push_back({x1, y1, x0, y1});
    segs.push_back({x0, y1, x0, y0});
  };
  const auto leader_to = [&](P2 from) {
    if (base::js_hypot(anchor[0] - from[0], anchor[1] - from[1]) > kArrowL * 1.2) {
      leader.push_back({from[0], from[1], anchor[0], anchor[1]});
      arrow_head(fills, anchor, from);
    }
  };
  const std::string kind = base::js_string(jsx::get(a, "kind"));
  Value extra = Value::object();
  if (kind == "fcf") {
    const double H = 2 * h;
    const double pad = kPad;
    const double y0 = P[1] - H / 2;
    const double y1 = P[1] + H / 2;
    const double ty = P[1] - 0.35 * h;
    base::Array parts;
    std::vector<double> part_x;
    double x = P[0];
    parts.push_back(base::make_object({{"kind", "char"}, {"box", base::Array{x, y0, x + H, y1}}}));
    part_x.push_back(x);
    if (auto r = char_symbol(base::js_string(jsx::get(a, "char")), {x + H / 2, P[1]}, frame, fills); !r) {
      return std::unexpected(r.error());
    }
    x += H;
    const std::string tol_text = num4(jsx::get(a, "tol").as_number());
    const bool dia = jsx::get(a, "dia").truthy();
    const Value mod = jsx::get(a, "mod");
    double wd = pad + (dia ? kDiaW : 0) + text_width(tol_text, h) + (mod.truthy() ? 0.6 + kModW : 0) + pad;
    wd = jsx::max(wd, H);
    double cx = x + pad;
    if (dia) {
      dia_symbol({cx + kDiaW / 2, P[1]}, frame);
      cx += kDiaW;
    }
    texts.push_back({cx, ty, h, tol_text, "start"});
    cx += text_width(tol_text, h);
    if (mod.truthy()) {
      mod_symbol(base::js_string(mod), {cx + 0.6 + kModW / 2, P[1]}, frame);
    }
    parts.push_back(base::make_object({{"kind", "tol"}, {"box", base::Array{x, y0, x + wd, y1}}}));
    part_x.push_back(x);
    x += wd;
    for (const Value& d : jsx::get(a, "datums").as_array()) {
      const std::string letter = base::js_string(jsx::get(d, "letter"));
      const Value dmod = jsx::get(d, "mod");
      const double lw = text_width(letter, h);
      const double dw = jsx::max(H, pad + lw + (dmod.truthy() ? 0.6 + kModW : 0) + pad);
      const double inner = lw + (dmod.truthy() ? 0.6 + kModW : 0);
      const double sx = x + (dw - inner) / 2;
      texts.push_back({sx, ty, h, letter, "start"});
      if (dmod.truthy()) {
        mod_symbol(base::js_string(dmod), {sx + lw + 0.6 + kModW / 2, P[1]}, frame);
      }
      parts.push_back(
          base::make_object({{"kind", "datum"}, {"letter", letter}, {"box", base::Array{x, y0, x + dw, y1}}}));
      part_x.push_back(x);
      x += dw;
    }
    rect(P[0], y0, x, y1, frame);
    for (std::size_t i = 1; i < part_x.size(); ++i) {
      frame.push_back({part_x[i], y0, part_x[i], y1});
    }
    const std::array<double, 4> fb{P[0], y0, x, y1};
    if (!inside(fb, anchor)) {
      leader_to(nearest_on_box(fb, anchor));
    }
    extra.append("parts", std::move(parts));
    extra.append("frameBox", box_value(fb));
  } else if (kind == "datum") {
    const double S = 2 * h;
    const std::array<double, 4> fb{P[0] - S / 2, P[1] - S / 2, P[0] + S / 2, P[1] + S / 2};
    rect(fb[0], fb[1], fb[2], fb[3], frame);
    texts.push_back({P[0], P[1] - 0.35 * h, h, base::js_string(jsx::get(a, "letter")), "middle"});
    const double dx = P[0] - anchor[0];
    const double dy = P[1] - anchor[1];
    const bool vertical = std::abs(dx) <= 2 * std::abs(dy);
    const double ux = vertical ? 0 : js_sign(dx);
    double uy = 0;
    if (vertical) {
      uy = dy < 0 ? -1 : 1;
    }
    const double side = 3;
    const double th = side * std::numbers::sqrt3 / 2;
    const P2 apex{anchor[0] + ux * th, anchor[1] + uy * th};
    fills.push_back({{anchor[0] - uy * side / 2, anchor[1] + ux * side / 2},
                     {anchor[0] + uy * side / 2, anchor[1] - ux * side / 2},
                     apex});
    const P2 start = nearest_on_box(fb, apex);
    if (!inside(fb, apex) && base::js_hypot(start[0] - apex[0], start[1] - apex[1]) > 1e-9) {
      leader.push_back({start[0], start[1], apex[0], apex[1]});
    }
    extra.append("frameBox", box_value(fb));
    extra.append("triangle", pts_value(fills[0]));
  } else if (kind == "finish") {
    const double H1 = 1.4 * h;
    const double H2 = 3 * h;
    const double k = 1 / std::tan(PI / 3);
    const P2 tip = P;
    const P2 s_top{tip[0] - H1 * k, tip[1] + H1};
    const P2 l_top{tip[0] + H2 * k, tip[1] + H2};
    frame.push_back({tip[0], tip[1], s_top[0], s_top[1]});
    frame.push_back({tip[0], tip[1], l_top[0], l_top[1]});
    const std::string process = base::js_string(jsx::get(a, "process"));
    if (process == "removal") {
      frame.push_back({s_top[0], s_top[1], tip[0] + H1 * k, tip[1] + H1});
    }
    if (process == "noRemoval") {
      const double rc = H1 / 3;
      poly(circle_pts({tip[0], tip[1] + 2 * rc}, rc, 32), frame);
      extra.append("circle", base::make_object({{"c", base::Array{tip[0], tip[1] + 2 * rc}}, {"r", rc}}));
    }
    const std::string ra_text = "Ra " + num4(jsx::get(a, "ra").as_number());
    const Value note = jsx::get(a, "note");
    const std::string note_s = note.truthy() ? base::js_string(note) : std::string();
    const double ext = jsx::max(text_width(ra_text, h), note.truthy() ? text_width(note_s, 2.5) : 0) + 2;
    frame.push_back({l_top[0], l_top[1], l_top[0] + ext, l_top[1]});
    texts.push_back({l_top[0] + 1, l_top[1] - 1 - 0.75 * h, h, ra_text, "start"});
    if (note.truthy()) {
      texts.push_back({l_top[0] + 1, l_top[1] + 1, 2.5, note_s, "start"});
    }
    if (base::js_hypot(off[0], off[1]) > 1e-9) {
      leader_to(tip);
    }
  } else {
    return base::fail(base::Errc::invalid, w.t("model.gdt.unknown_kind"));
  }
  base::Array strokes;
  strokes.emplace_back(base::make_object({{"width", kFrame}, {"segs", segs_value(frame)}}));
  strokes.emplace_back(base::make_object({{"width", kLeader}, {"segs", segs_value(leader)}}));
  out.append("strokes", std::move(strokes));
  base::Array fv;
  for (const auto& f : fills) {
    fv.emplace_back(base::make_object({{"pts", pts_value(f)}}));
  }
  out.append("fills", std::move(fv));
  base::Array tv;
  for (const Text& t : texts) {
    tv.emplace_back(base::make_object(
        {{"x", t.x}, {"y", t.y}, {"size", t.size}, {"text", t.text}, {"anchor", std::string(t.anchor)}}));
  }
  out.append("texts", std::move(tv));
  for (const auto& m : extra.as_object()) {
    out.append(m.key, m.value);
  }
  out.append("box", box_value(box_of(frame, leader, fills, texts)));
  return out;
}

}  // namespace cc::partcraft::gdt
