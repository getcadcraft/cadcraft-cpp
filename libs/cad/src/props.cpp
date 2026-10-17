// SPDX-License-Identifier: Apache-2.0
#include "props.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/json.hpp"

namespace cc::cad::detail {

namespace {

/// [st key, variable, DXF group code, kind, in R12] — dwg-props.js DIMVARS, in its order.
struct DimVar {
  std::string_view key, name;
  int code;
  std::string_view kind;
  bool r12;
};
constexpr std::array<DimVar, 42> kDimVars = {{
    {"scale", "DIMSCALE", 40, "real", true},     {"asz", "DIMASZ", 41, "real", true},
    {"exo", "DIMEXO", 42, "real", true},         {"dli", "DIMDLI", 43, "real", true},
    {"exe", "DIMEXE", 44, "real", true},         {"rnd", "DIMRND", 45, "real", true},
    {"tp", "DIMTP", 47, "real", true},           {"tm", "DIMTM", 48, "real", true},
    {"txt", "DIMTXT", 140, "real", true},        {"tsz", "DIMTSZ", 142, "real", true},
    {"altf", "DIMALTF", 143, "real", true},      {"lfac", "DIMLFAC", 144, "real", true},
    {"tfac", "DIMTFAC", 146, "real", true},      {"gap", "DIMGAP", 147, "real", true},
    {"altrnd", "DIMALTRND", 148, "real", false}, {"se1", "DIMSE1", 75, "bool", true},
    {"se2", "DIMSE2", 76, "bool", true},         {"tad", "DIMTAD", 77, "int", true},
    {"zin", "DIMZIN", 78, "int", true},          {"azin", "DIMAZIN", 79, "int", false},
    {"alt", "DIMALT", 170, "bool", true},        {"altd", "DIMALTD", 171, "int", true},
    {"tofl", "DIMTOFL", 172, "bool", true},      {"tix", "DIMTIX", 174, "bool", true},
    {"clrd", "DIMCLRD", 176, "color", true},     {"clre", "DIMCLRE", 177, "color", true},
    {"clrt", "DIMCLRT", 178, "color", true},     {"adec", "DIMADEC", 179, "int", false},
    {"dec", "DIMDEC", 271, "int", false},        {"tdec", "DIMTDEC", 272, "int", false},
    {"altu", "DIMALTU", 273, "int", false},      {"aunit", "DIMAUNIT", 275, "int", false},
    {"lunit", "DIMLUNIT", 277, "int", false},    {"dsep", "DIMDSEP", 278, "sep", false},
    {"tmove", "DIMTMOVE", 279, "int", false},    {"just", "DIMJUST", 280, "int", false},
    {"sd1", "DIMSD1", 281, "bool", false},       {"sd2", "DIMSD2", 282, "bool", false},
    {"tolj", "DIMTOLJ", 283, "int", false},      {"tzin", "DIMTZIN", 284, "int", false},
    {"altz", "DIMALTZ", 285, "int", false},      {"atfit", "DIMATFIT", 289, "int", false},
}};
// the three DIMVARS rows past 42 (lineweights and the text fill) — kept in a second table to keep the first readable
constexpr std::array<DimVar, 4> kDimVars2 = {{
    {"lwd", "DIMLWD", 371, "lw", false},
    {"lwe", "DIMLWE", 372, "lw", false},
    {"tfill", "DIMTFILL", 69, "int", false},
    {"tfillclr", "DIMTFILLCLR", 70, "color", false},
}};
template <class F>
void each_dimvar(const F& f) {
  for (const DimVar& d : kDimVars) {
    f(d);
  }
  for (const DimVar& d : kDimVars2) {
    f(d);
  }
}

// codes handled by hand: DIMPOST 3, DIMAPOST 4, DIMTOL 71, DIMLIM 72, DIMTIH 73, DIMTOH 74, DIMSAH 173, DIMUNIT 270
double special_code(std::string_view name) {
  if (name == "DIMPOST") {
    return 3;
  }
  if (name == "DIMAPOST") {
    return 4;
  }
  if (name == "DIMTOL") {
    return 71;
  }
  if (name == "DIMLIM") {
    return 72;
  }
  if (name == "DIMTIH") {
    return 73;
  }
  if (name == "DIMTOH") {
    return 74;
  }
  if (name == "DIMSAH") {
    return 173;
  }
  if (name == "DIMUNIT") {
    return 270;
  }
  return kNaN;
}

bool is_hex_color(const Value& v) {  // /^#[0-9a-f]{6}$/i
  if (!v.is_string() || v.as_string().size() != 7 || v.as_string()[0] != '#') {
    return false;
  }
  for (std::size_t i = 1; i < 7; ++i) {
    const char c = v.as_string()[i];
    if ((c < '0' || c > '9') && (c < 'a' || c > 'f') && (c < 'A' || c > 'F')) {
      return false;
    }
  }
  return true;
}

/// parseFloat(n) for any value (parseFloat converts with String first)
double parse_float(const Value& n) {
  return n.is_number() ? n.as_number() : base::js_parse_float(str(n));
}

/// in1: a variable's value → the st value (checked), or undefined.
Value in1(std::string_view kind, const Value& raw) {
  Value n = raw;
  if (raw.is_object()) {  // typeof object (arrays too) and not null
    const Value rgb = g(raw, "rgb");
    n = rgb.is_undefined() ? g(raw, "index") : Value(hex_color(num(rgb)));
  } else if (raw.is_array()) {
    n = Value();  // an array has no .rgb / .index: raw.index → undefined
  }
  if (kind == "real") {
    const double x = parse_float(n);
    return std::isfinite(x) ? Value(x) : Value();
  }
  if (kind == "int") {
    const double x = base::js_round(parse_float(n));
    return std::isfinite(x) ? Value(x) : Value();
  }
  if (kind == "bool") {
    const double x = parse_float(n);
    if (x != 0 && !std::isnan(x)) {
      return true;
    }
    return n.is_bool() && n.as_bool();
  }
  if (kind == "color") {
    if (is_hex_color(n)) {
      return base::js_to_lower(n.as_string());
    }
    const double x = base::js_round(parse_float(n));
    if (x == 256) {
      return {nullptr};
    }
    if (x == 0) {
      return 0;
    }
    if (x >= 1 && x <= 255) {
      return x;
    }
    return {};
  }
  if (kind == "lw") {
    const double x = base::js_round(parse_float(n));
    if (x == -1 || x == -2 || x == -3) {
      return x;
    }
    if (x >= 0 && x <= 211) {
      return x / 100;
    }
    return {};
  }
  if (kind == "sep") {
    return base::js_round(parse_float(n)) == 44 ? "," : ".";
  }
  return {};
}

/// DIMPOST "pre<>suf" → [pre, suf]; a text with no "<>" is a suffix.
std::pair<std::string, std::string> post(const Value& v) {
  const std::string s = nullish(v) ? std::string() : str(v);
  const std::size_t k = s.find("<>");
  if (k == std::string::npos) {
    return {"", s};
  }
  return {s.substr(0, k), s.substr(k + 2)};
}

bool undefined_key(const Value& st, std::string_view k) {
  const Value* v = st.find(k);
  return v == nullptr || v->is_undefined();
}

/// a JS `x < 0` on a member that may be absent
bool lt0(const Value& st, std::string_view k) {
  const Value* v = st.find(k);
  return v != nullptr && num(*v) < 0;
}

void specials(const std::function<Value(std::string_view)>& get, Value& st) {
  const Value tih = get("DIMTIH");
  const Value toh = get("DIMTOH");
  if (!tih.is_undefined() || !toh.is_undefined()) {
    // st.tih = !!+(tih ?? 0) && !!+(toh ?? tih ?? 0)
    const auto truthy_num = [](const Value& v) {
      const double d = num(v);
      return d != 0 && !std::isnan(d);
    };
    const bool a = truthy_num(nn(tih, 0));
    st.set("tih", a && truthy_num(nn(toh, nn(tih, 0))));
  }
  const auto set_or_drop = [&](std::string_view k, const std::string& v) {
    if (!v.empty()) {
      st.set(k, v);
    } else {
      st.erase(k);
    }
  };
  if (const Value p = get("DIMPOST"); !p.is_undefined()) {
    const auto [pre, suf] = post(p);
    set_or_drop("pre", pre);
    set_or_drop("suf", suf);
  }
  if (const Value ap = get("DIMAPOST"); !ap.is_undefined()) {
    const auto [pre, suf] = post(ap);
    set_or_drop("apre", pre);
    set_or_drop("asuf", suf);
  }
  const Value tol = get("DIMTOL");
  const Value lim = get("DIMLIM");
  if (!tol.is_undefined() || !lim.is_undefined() || lt0(st, "gap")) {
    const auto truthy_num = [](const Value& v) {
      const double d = num(v);
      return d != 0 && !std::isnan(d);
    };
    if (truthy_num(lim)) {
      st.set("tol", "lim");
    } else if (truthy_num(tol)) {
      const Value tp = st.get("tp");
      const Value tm = st.get("tm");
      const double d = (tp.truthy() ? num(tp) : 0) - (tm.truthy() ? num(tm) : 0);
      st.set("tol", std::abs(d) < 1e-12 ? "sym" : "dev");
    } else if (lt0(st, "gap")) {
      st.set("tol", "basic");
    } else if (!tol.is_undefined() || !lim.is_undefined()) {
      st.erase("tol");
    }
  }
  if (lt0(st, "gap")) {
    st.set("gap", -num(st.get("gap")));
  }
  if (const Value unit = get("DIMUNIT"); !unit.is_undefined() && undefined_key(st, "lunit")) {
    const double u = base::js_round(num(unit));
    double lunit = u;  // DIMUNIT 6, 7 (R12's unstacked architectural / fractional) → 4, 5
    if (u == 6) {
      lunit = 4;
    } else if (u == 7) {
      lunit = 5;
    }
    st.set("lunit", lunit);
  }
}

/// joined(strs, prefix): the strings that start with prefix, without it, joined.
std::string joined(const std::vector<std::string>& strs, std::string_view prefix) {
  std::string out;
  for (const std::string& s : strs) {
    if (s.starts_with(prefix)) {
      out += s.substr(prefix.size());
    }
  }
  return out;
}

/// colorFromWord: CMC colour word (0xC2rrggbb true colour, 0xC3 + ACI index) → '#rrggbb', an index, or undefined.
Value color_from_word(const Value& v) {
  const std::uint32_t w = base::js_to_uint32(num(v));
  const std::uint32_t m = w >> 24U;
  if (m == 0xC2) {
    return hex_color(static_cast<double>(w & 0xFFFFFFU));
  }
  if (m == 0xC3 && (w & 0xFFU) >= 1) {
    return static_cast<double>(w & 0xFFU);
  }
  return {};
}

/// `[c, v]` destructuring of an xdata item (undefined parts when it is not an array)
const Value& code_of(const Value& it) {
  return item(it, 0);
}
const Value& val_of(const Value& it) {
  return item(it, 1);
}
bool code_is(const Value& it, double c) {
  const Value& x = code_of(it);
  return x.is_number() && x.as_number() == c;
}

/// app(n) of propsFromXdata: xd[k] || [] for the first key equal without case, else null (nullptr)
const Value* app(const Value& xd, std::string_view upper) {
  static const Value kEmpty = Value::array();
  for (const base::Member* m : base::js_members(xd.as_object())) {
    if (base::js_to_upper(m->key) == upper) {
      return m->value.truthy() ? &m->value : &kEmpty;
    }
  }
  return nullptr;
}

bool known_app(std::string_view up) {
  return up == "ACAD" || up == "CADCRAFT" || up == "PE_URL" || up == "ACADANNOTATIVE" || up == "HATCHBACKGROUNDCOLOR";
}

bool annotative_data(const Value& v) {  // /^AnnotativeData$/i
  if (!v.is_string()) {
    return false;
  }
  return base::js_to_lower(v.as_string()) == "annotativedata";
}

}  // namespace

double js_max(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return kNaN;
  }
  return a > b ? a : b;
}
double js_min(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return kNaN;
  }
  return a < b ? a : b;
}

const Value& dim_defaults() {
  static const Value kV = base::make_object({{"txt", 2.5},
                                             {"asz", 2.5},
                                             {"exe", 1.25},
                                             {"exo", 0.625},
                                             {"gap", 0.625},
                                             {"dli", 3.75},
                                             {"dec", 2},
                                             {"adec", 0},
                                             {"scale", 1},
                                             {"tad", 1},
                                             {"zin", 8}});
  return kV;
}

const Value& dim_extra() {
  static const Value kV = base::make_object({
      {"blk1", ""},    {"blk2", ""},   {"tsz", 0},      {"clrd", 0},    {"clre", 0},    {"lwd", -2},
      {"lwe", -2},     {"sd1", false}, {"sd2", false},  {"se1", false}, {"se2", false}, {"txsty", "Standard"},
      {"clrt", 0},     {"tfill", 0},   {"tfillclr", 0}, {"just", 0},    {"tih", false}, {"atfit", 3},
      {"tmove", 0},    {"tix", false}, {"tofl", false}, {"rnd", 0},     {"pre", ""},    {"suf", ""},
      {"lfac", 1},     {"dsep", "."},  {"aunit", 0},    {"azin", 0},    {"alt", false}, {"altu", 2},
      {"altd", 2},     {"altf", 25.4}, {"altrnd", 0},   {"apre", ""},   {"asuf", ""},   {"altz", 0},
      {"tol", "none"}, {"tp", 0},      {"tm", 0},       {"tdec", 2},    {"tfac", 1},    {"tolj", 1},
      {"tzin", 0},
  });
  return kV;
}

Value tr_from(const Value& v) {
  if (v.is_undefined() || v.is_null() || (v.is_string() && v.as_string().empty())) {
    return {};
  }
  const auto pct = [](double alpha) -> Value {
    const double p = base::js_round((1 - alpha / 255) * 100);
    return p > 0 ? Value(js_min(90, p)) : Value();
  };
  if (v.is_object() || v.is_array()) {
    if (v.is_object() && g(v, "byBlock").truthy()) {
      return "ByBlock";
    }
    const Value alpha = v.is_object() ? g(v, "alpha") : Value();
    if (finite(alpha)) {
      return pct(alpha.as_number());
    }
    return {};
  }
  const std::uint32_t w = base::js_to_uint32(num(v));
  const std::uint32_t kind = w >> 24U;
  if (kind == 1) {
    return "ByBlock";
  }
  if (kind == 2 || kind == 3) {
    return pct(static_cast<double>(w & 0xFFU));
  }
  return {};
}

Value props_from_xdata(const Value& xd) {
  Value out = Value::object();
  if (!xd.is_object() || xd.is_point()) {
    return out;
  }
  std::vector<std::string> cc;
  if (const Value* a = app(xd, "CADCRAFT")) {
    if (a->is_array()) {
      for (const Value& it : a->as_array()) {
        if (code_is(it, 1000)) {
          cc.push_back(str(val_of(it)));
        }
      }
    }
  }
  if (const std::string mat = joined(cc, "CCMAT:"); !mat.empty()) {
    out.set("mat", mat);
  }
  for (const std::string& s : cc) {
    if (!s.starts_with("CCHORG:")) {
      continue;
    }
    const std::string rest = s.substr(7);
    const std::size_t comma = rest.find(',');
    const double x = base::js_number(rest.substr(0, comma));
    const double y = comma == std::string::npos
                         ? kNaN
                         : base::js_number(rest.substr(comma + 1, rest.find(',', comma + 1) - comma - 1));
    if (std::isfinite(x) && std::isfinite(y)) {
      if (x != 0) {
        out.set("ox", x);
      }
      if (y != 0) {
        out.set("oy", y);
      }
    }
    break;
  }
  for (const std::string& s : cc) {
    if (s == "CCHASSOC") {
      out.set("assoc", true);
      break;
    }
  }
  for (const std::string& s : cc) {
    if (!s.starts_with("CCTR:")) {
      continue;
    }
    const std::string v = s.substr(5);
    if (v == "ByBlock") {
      out.set("tr", v);
    } else {
      const double t = base::js_number(v);
      if (std::isfinite(t) && t >= 0 && t <= 90) {
        out.set("tr", t);
      }
    }
    break;
  }
  if (const Value* a = app(xd, "PE_URL"); a != nullptr && a->is_array()) {
    for (const Value& it : a->as_array()) {
      if (code_is(it, 1000)) {
        const std::string u = str(val_of(it));
        if (!base::js_trim(u).empty()) {
          out.set("url", js_slice(u, 2048));
        }
        break;
      }
    }
  }
  if (const Value* a = app(xd, "ACADANNOTATIVE"); a != nullptr && a->is_array()) {
    bool has = false;
    for (const Value& it : a->as_array()) {
      if (code_is(it, 1000) && annotative_data(Value(str(val_of(it))))) {
        has = true;
        break;
      }
    }
    if (has) {
      std::vector<double> ints;
      for (const Value& it : a->as_array()) {
        if (code_is(it, 1070)) {
          ints.push_back(num(val_of(it)));
        }
      }
      if ((ints.size() > 1 && ints[1] == 1) || (ints.size() == 1 && ints[0] == 1)) {
        out.set("ann", true);
      }
    }
  }
  if (const Value* a = app(xd, "HATCHBACKGROUNDCOLOR"); a != nullptr && a->is_array()) {
    for (const Value& it : a->as_array()) {
      if (code_is(it, 1071)) {
        const Value c = color_from_word(val_of(it));
        if (!c.is_undefined()) {
          out.set("bg", c);
        }
        break;
      }
    }
  }
  // every other application's xdata is kept as e.xd, without a group that holds a 1005 handle
  Value keep = Value::object();
  for (const base::Member* mp : base::js_members(xd.as_object())) {
    const base::Member& m = *mp;
    const std::string up = base::js_to_upper(m.key);
    if (known_app(up) || !m.value.is_array() || m.value.as_array().empty()) {
      continue;
    }
    bool handle = false;
    for (const Value& it : m.value.as_array()) {
      if (num(code_of(it)) == 1005) {
        handle = true;
        break;
      }
    }
    if (handle) {
      continue;
    }
    Array items;
    for (const Value& it : m.value.as_array()) {
      const double n = num(code_of(it));
      items.emplace_back(Array{n, n >= 1010 && n <= 1071 ? Value(num(val_of(it))) : Value(str(val_of(it)))});
    }
    keep.set(up, std::move(items));
  }
  if (!keep.as_object().empty()) {
    out.set("xd", std::move(keep));
  }
  return out;
}

void st_from_vars(const Value& vars, Value& st) {
  const auto var = [&](std::string_view name) -> Value {
    return vars.is_object() && !vars.is_point() ? vars[name] : Value();
  };
  each_dimvar([&](const DimVar& d) {
    const Value raw = var(d.name);
    if (!raw.is_undefined()) {
      const Value x = in1(d.kind, raw);
      if (!x.is_undefined()) {
        st.set(d.key, x);
      }
    }
  });
  specials(var, st);
}

void st_from_codes(const std::function<Value(double)>& get, Value& st) {
  each_dimvar([&](const DimVar& d) {
    const Value raw = get(d.code);
    if (!raw.is_undefined()) {
      const Value x = in1(d.kind, raw);
      if (!x.is_undefined()) {
        st.set(d.key, x);
      }
    }
  });
  specials([&](std::string_view name) { return get(special_code(name)); }, st);
}

void cc_dim_from(const std::vector<std::string>& strs, Value& st) {
  const std::string j = joined(strs, "CCDIMST:");
  if (j.empty()) {
    return;
  }
  auto parsed = base::parse_json(j);
  if (!parsed || !parsed->is_object()) {
    return;
  }
  const Value& extra = dim_extra();
  const Value& defs = dim_defaults();
  static constexpr std::array<std::string_view, 12> kArrows = {"",        "CLOSEDBLANK", "CLOSED",   "OPEN",
                                                               "OPEN30",  "DOT",         "DOTSMALL", "ARCHTICK",
                                                               "OBLIQUE", "ORIGIN",      "OPEN90",   "NONE"};
  static constexpr std::array<std::string_view, 5> kTolModes = {"none", "sym", "dev", "lim", "basic"};
  for (const base::Member* mp : base::js_members(parsed->as_object())) {
    const base::Member& m = *mp;
    const std::string& k = m.key;
    const Value& v = m.value;
    const Value* d = extra.find(k);
    if (d == nullptr) {
      d = defs.find(k);  // DEF = { ...DIM_DEFAULTS, ...DIM_EXTRA }: the extra value wins where both have the key
    }
    if (d == nullptr) {
      continue;
    }
    if (k == "blk1" || k == "blk2") {
      if (v.is_string() && std::ranges::find(kArrows, std::string_view(v.as_string())) != kArrows.end()) {
        st.set(k, v);
      }
      continue;
    }
    if (k == "tol") {
      if (v.is_string() && std::ranges::find(kTolModes, std::string_view(v.as_string())) != kTolModes.end()) {
        st.set(k, v);
      }
      continue;
    }
    if (k.starts_with("clr") || k == "tfillclr") {
      const bool ok = v.is_null() ||
                      (v.is_number() && (v.as_number() == 0 || (v.as_number() >= 1 && v.as_number() <= 255 &&
                                                                std::trunc(v.as_number()) == v.as_number()))) ||
                      is_hex_color(v);
      if (ok) {
        st.set(k, v);
      }
      continue;
    }
    if ((d->is_number() && finite(v)) || (d->is_bool() && v.is_bool())) {
      st.set(k, v);
    } else if (d->is_string() && v.is_string()) {
      st.set(k, js_slice(v.as_string(), 255));
    }
  }
}

std::vector<std::pair<double, Value>> dstyle_map(const Value& items) {
  std::vector<std::pair<double, Value>> m;
  if (!items.is_array()) {
    return m;
  }
  const Array& list = items.as_array();
  std::size_t k = list.size();
  for (std::size_t i = 0; i < list.size(); ++i) {
    if (code_is(list[i], 1000) && base::js_trim(str(val_of(list[i]))) == "DSTYLE") {
      k = i;
      break;
    }
  }
  if (k == list.size()) {
    return m;
  }
  for (std::size_t j = k + 1; j + 1 < list.size(); ++j) {
    const Value& c = code_of(list[j]);
    const Value& v = val_of(list[j]);
    if (num(c) == 1002 && c.is_number() && base::js_trim(str(v)) == "}") {
      break;
    }
    if (c.is_number() && c.as_number() == 1002) {
      continue;
    }
    if (c.is_number() && c.as_number() == 1070) {
      const double key = base::js_round(num(v));
      const Value& value = val_of(list[j + 1]);
      bool found = false;
      for (auto& [kk, vv] : m) {
        if (kk == key || (std::isnan(kk) && std::isnan(key))) {
          vv = value;
          found = true;
          break;
        }
      }
      if (!found) {
        m.emplace_back(key, value);
      }
      ++j;
    }
  }
  return m;
}

const Value& xdata_of(const Value& xdata, std::string_view app_upper) {
  static const Value kEmpty = Value::array();
  if (!xdata.is_object() || xdata.is_point()) {
    return kEmpty;
  }
  for (const base::Member* m : base::js_members(xdata.as_object())) {
    if (base::js_to_upper(m->key) == app_upper) {
      return m->value.is_array() ? m->value : kEmpty;
    }
  }
  return kEmpty;
}

std::vector<std::string> cadcraft_strings(const Value& xdata) {
  std::vector<std::string> out;
  const Value& list = xdata_of(xdata, "CADCRAFT");
  for (const Value& it : list.as_array()) {
    out.push_back(str(val_of(it)));
  }
  return out;
}

}  // namespace cc::cad::detail
