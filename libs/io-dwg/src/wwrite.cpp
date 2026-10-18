// SPDX-License-Identifier: Apache-2.0
// write.js (cc-dwg writeDwg) in C++: the version-neutral model → DWG records → the file. The other writer modules are
// in wcopy.cpp, wtable.cpp and wsolid.cpp; the encoders in wobjects.cpp; the file layouts in wcontainer.cpp.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <random>
#include <regex>
#include <stdexcept>

#include "cc/base/file.hpp"
#include "cc/base/js.hpp"
#include "cc/dwg/write.hpp"
#include "messages.hpp"
#include "wcontainer.hpp"
#include "wwriter.hpp"

namespace cc::dwg {

namespace w {

using base::make_object;

// ---------------------------------------------------------------- small helpers

Key hex_key(const Value& h) {
  if (nullish(h)) {
    return std::nullopt;
  }
  std::string s;
  if (h.is_number()) {
    const double d = h.as_number();
    if (std::isfinite(d) && d >= 0 && std::trunc(d) == d) {
      s = hex_upper(d);
    } else {
      // Number#toString(16) of a fraction / negative: rare (hand-made models); the decimal form keeps keys distinct
      s = base::js_string(h);
    }
  } else {
    s = base::js_to_upper(h.is_string() ? h.as_string() : base::js_string(h));
  }
  std::size_t i = 0;
  while (i + 1 < s.size() && s[i] == '0') {
    ++i;
  }
  return s.substr(i);
}

std::string hex_upper(double v) {
  return hex_digits(static_cast<std::uint64_t>(v), true);
}

std::string upper(const Value& v) {
  return base::js_to_upper(v.is_string() ? v.as_string() : base::js_string(v));
}

bool js_less(const std::string& a, const std::string& b) {
  return base::to_utf16(a) < base::to_utf16(b);
}

bool js_space(std::uint32_t c) noexcept {
  return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 ||
         c == 0xFEFF;
}

namespace {

constexpr std::array<double, 24> kLwTable = {0,  5,  9,  13, 15, 18,  20,  25,  30,  35,  40,  50,
                                             53, 60, 70, 80, 90, 100, 106, 120, 140, 158, 200, 211};

/// UTF-8 → code points
std::vector<std::uint32_t> code_points(std::string_view s) {
  std::vector<std::uint32_t> out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    const auto c0 = static_cast<unsigned char>(s[i]);
    std::uint32_t c = c0;
    std::size_t n = 1;
    if (c0 >= 0xF0 && i + 3 < s.size()) {
      c = ((c0 & 7U) << 18U) | ((static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 12U) |
          ((static_cast<unsigned char>(s[i + 2]) & 0x3FU) << 6U) | (static_cast<unsigned char>(s[i + 3]) & 0x3FU);
      n = 4;
    } else if (c0 >= 0xE0 && i + 2 < s.size()) {
      c = ((c0 & 0xFU) << 12U) | ((static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 6U) |
          (static_cast<unsigned char>(s[i + 2]) & 0x3FU);
      n = 3;
    } else if (c0 >= 0xC0 && i + 1 < s.size()) {
      c = ((c0 & 0x1FU) << 6U) | (static_cast<unsigned char>(s[i + 1]) & 0x3FU);
      n = 2;
    }
    out.push_back(c);
    i += n;
  }
  return out;
}
void put_utf8(std::string& o, std::uint32_t c) {
  if (c < 0x80) {
    o.push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    o.push_back(static_cast<char>(0xC0U | (c >> 6U)));
    o.push_back(static_cast<char>(0x80U | (c & 0x3FU)));
  } else if (c < 0x10000) {
    o.push_back(static_cast<char>(0xE0U | (c >> 12U)));
    o.push_back(static_cast<char>(0x80U | ((c >> 6U) & 0x3FU)));
    o.push_back(static_cast<char>(0x80U | (c & 0x3FU)));
  } else {
    o.push_back(static_cast<char>(0xF0U | (c >> 18U)));
    o.push_back(static_cast<char>(0x80U | ((c >> 12U) & 0x3FU)));
    o.push_back(static_cast<char>(0x80U | ((c >> 6U) & 0x3FU)));
    o.push_back(static_cast<char>(0x80U | (c & 0x3FU)));
  }
}
/// String#slice(0, n) in UTF-16 units
std::string slice16(const std::string& s, std::size_t n) {
  std::string o;
  std::size_t units = 0;
  for (const std::uint32_t c : code_points(s)) {
    const std::size_t u = c > 0xFFFF ? 2 : 1;
    if (units + u > n) {
      break;
    }
    units += u;
    put_utf8(o, c);
  }
  return o;
}
/// BAD_NAME = /[<>/\\":;?*|,=`\x00-\x1f]/
bool bad_name_char(std::uint32_t c, bool bar) {
  if (c < 0x20) {
    return true;
  }
  switch (c) {
    case '<':
    case '>':
    case '/':
    case '\\':
    case '"':
    case ':':
    case ';':
    case '?':
    case '*':
    case ',':
    case '=':
    case '`': return true;
    case '|': return bar;
    default: return false;
  }
}
std::string js_trim(const std::string& s) {
  const auto cps = code_points(s);
  std::size_t a = 0;
  std::size_t b = cps.size();
  while (a < b && js_space(cps[a])) {
    ++a;
  }
  while (b > a && js_space(cps[b - 1])) {
    --b;
  }
  std::string o;
  for (std::size_t i = a; i < b; ++i) {
    put_utf8(o, cps[i]);
  }
  return o;
}
std::string str_of(const Value& v) {
  if (nullish(v)) {
    return {};
  }
  return v.is_string() ? v.as_string() : base::js_string(v);
}
bool ieq(const Value& v, std::string_view lower) {  // /^lower$/i.test(v)
  if (nullish(v) && lower != "undefined" && lower != "null") {
    return false;
  }
  return base::js_to_lower(v.is_string() ? v.as_string() : base::js_string(v)) == lower;
}
/// fin(p.x, p.y): both finite numbers
bool fin2(const Value& p) {
  const Value x = get(p, "x");
  const Value y = get(p, "y");
  return x.is_number() && std::isfinite(x.as_number()) && y.is_number() && std::isfinite(y.as_number());
}
/// v || ''
Value orv_value(const Value& v) {
  return v.truthy() ? v : Value("");
}
bool is_str(const Value& v, std::string_view s) {
  return v.is_string() && v.as_string() == s;
}
std::string null_key(const Key& k) {
  return k ? *k : std::string("\x01null");
}
/// okP(p): p && Number.isFinite(p.x, p.y)
bool okP(const Value& p) {
  return p.truthy() && p.is_object() && fin2(p);
}
/// write.js P3: p ? { num(x), num(y), num(z) } : zeros
Value P3w(const Value& p) {
  if (!p.truthy()) {
    return base::pt3(0, 0, 0);
  }
  return base::pt3(numk(p, "x"), numk(p, "y"), numk(p, "z"));
}
Value str_value(const Value& v) {  // String(v ?? '')
  return {str_of(v)};
}
RecPtr make_rec(Value v) {
  auto r = std::make_shared<Rec>();
  r->v = std::move(v);
  return r;
}
/// tagOf
std::string tag_of(const Value& t) {
  const std::string up = upper(t);
  std::string o;
  bool in_space = false;
  for (const std::uint32_t c : code_points(up)) {
    if (js_space(c)) {
      if (!in_space) {
        o.push_back('_');
      }
      in_space = true;
      continue;
    }
    in_space = false;
    if (c < 0x20) {
      continue;
    }
    put_utf8(o, c);
  }
  return o.empty() ? std::string("TAG") : o;
}
/// a.b.c (optional chaining) as a value
Value path(const Value& o, std::initializer_list<std::string_view> ks) {
  Value cur = o;
  for (const auto k : ks) {
    cur = get(cur, k);
  }
  return cur;
}

}  // namespace

double aci_from_rgb(double rgb) {
  static const std::array<std::array<double, 3>, 256> t = [] {
    std::array<std::array<double, 3>, 256> a{};
    const std::array<std::array<double, 3>, 9> first = {{{255, 0, 0},
                                                         {255, 255, 0},
                                                         {0, 255, 0},
                                                         {0, 255, 255},
                                                         {0, 0, 255},
                                                         {255, 0, 255},
                                                         {255, 255, 255},
                                                         {128, 128, 128},
                                                         {192, 192, 192}}};
    for (std::size_t i = 0; i < 9; ++i) {
      a[i + 1] = first[i];
    }
    const std::array<double, 5> V = {255, 204, 153, 127, 76};
    for (int i = 10; i < 250; ++i) {
      const double hue = std::floor((i - 10) / 10.0) * 15;
      const int k = (i - 10) % 10;
      const double v = V[static_cast<std::size_t>(k >> 1)];
      const bool pastel = (k & 1) != 0;
      const double h6 = hue / 60;
      const double f = h6 - std::floor(h6);
      const double lo = pastel ? v / 2 : 0;
      const double q = v - (v - lo) * f;
      const double u = lo + (v - lo) * f;
      const std::array<std::array<double, 3>, 6> sextant = {
          {{v, u, lo}, {q, v, lo}, {lo, v, u}, {lo, q, v}, {u, lo, v}, {v, lo, q}}};
      const auto& c = sextant[static_cast<std::size_t>(static_cast<int>(std::floor(h6)) % 6)];
      a[static_cast<std::size_t>(i)] = {base::js_round(c[0]), base::js_round(c[1]), base::js_round(c[2])};
    }
    const std::array<double, 6> g = {51, 91, 132, 173, 214, 255};
    for (std::size_t i = 0; i < 6; ++i) {
      a[250 + i] = {g[i], g[i], g[i]};
    }
    return a;
  }();
  const int x = base::js_to_int32(rgb);
  const double r = (x >> 16) & 255;
  const double gg = (x >> 8) & 255;
  const double b = x & 255;
  double best = 7;
  double bd = std::numeric_limits<double>::infinity();
  for (std::size_t i = 1; i < 256; ++i) {
    const auto& c = t[i];
    const double d = (c[0] - r) * (c[0] - r) + (c[1] - gg) * (c[1] - gg) + (c[2] - b) * (c[2] - b);
    if (d < bd) {
      bd = d;
      best = static_cast<double>(i);
    }
  }
  return best;
}

double lw_index(const Value& lwv) {
  if (nullish(lwv) || is(lwv, -1)) {
    return 29;
  }
  if (is(lwv, -2)) {
    return 30;
  }
  const double lw = base::js_to_number(lwv);
  if (lw == -3 || lw < 0) {
    return 31;
  }
  std::size_t best = 0;
  for (std::size_t i = 1; i < kLwTable.size(); ++i) {
    if (std::abs(kLwTable[i] - lw) < std::abs(kLwTable[best] - lw)) {
      best = i;
    }
  }
  return static_cast<double>(best);
}

// ---------------------------------------------------------------- Writer

namespace {
/// CLASSED as the JS modules leave it once loaded: objects-write.js, then mleader-write.js, nongraphic-write.js,
/// table-write.js, solid-write.js and surface-write.js add theirs (a name already there keeps its entry)
std::map<std::string, ClassDef, std::less<>> all_classed() {
  auto c = base_classed();
  const auto add = [&](const char* k, const ClassDef& d) { c.try_emplace(k, d); };
  add("MULTILEADER", {"MULTILEADER", "AcDbMLeader", "ACDB_MLEADER_CLASS", true, 1025});
  add("MLEADERSTYLE", {"MLEADERSTYLE", "AcDbMLeaderStyle", "ACDB_MLEADERSTYLE_CLASS", false, 4095});
  add("DICTIONARYVAR", {"DICTIONARYVAR", "AcDbDictionaryVar", "ObjectDBX Classes", false, 0});
  add("SCALE", {"SCALE", "AcDbScale", "ObjectDBX Classes", false, 1153});
  add("SORTENTSTABLE", {"SORTENTSTABLE", "AcDbSortentsTable", "ObjectDBX Classes", false, 0});
  add("IDBUFFER", {"IDBUFFER", "AcDbIdBuffer", "ObjectDBX Classes", false, 0});
  add("LAYER_INDEX", {"LAYER_INDEX", "AcDbLayerIndex", "ObjectDBX Classes", false, 0});
  add("SPATIAL_FILTER", {"SPATIAL_FILTER", "AcDbSpatialFilter", "ObjectDBX Classes", false, 0});
  add("FIELDLIST", {"FIELDLIST", "AcDbFieldList", "ObjectDBX Classes", false, 1152});
  add("PLOTSETTINGS", {"PLOTSETTINGS", "AcDbPlotSettings", "ObjectDBX Classes", false, 0});
  add("ACAD_TABLE", {"ACAD_TABLE", "AcDbTable", "ObjectDBX Classes", true, 1025});
  add("TABLESTYLE", {"TABLESTYLE", "AcDbTableStyle", "ObjectDBX Classes", false, 4095});
  add("FIELD", {"FIELD", "AcDbField", "ObjectDBX Classes", false, 1152});
  add("LIGHT", {"LIGHT", "AcDbLight", "SCENEOE", true, 1025});
  add("SURFACE", {"SURFACE", "AcDbSurface", "ObjectDBX Classes", true, 4095});
  add("PLANESURFACE", {"PLANESURFACE", "AcDbPlaneSurface", "ObjectDBX Classes", true, 4095});
  add("EXTRUDEDSURFACE", {"EXTRUDEDSURFACE", "AcDbExtrudedSurface", "ObjectDBX Classes", true, 4093});
  add("LOFTEDSURFACE", {"LOFTEDSURFACE", "AcDbLoftedSurface", "ObjectDBX Classes", true, 4093});
  add("REVOLVEDSURFACE", {"REVOLVEDSURFACE", "AcDbRevolvedSurface", "ObjectDBX Classes", true, 4093});
  add("SWEPTSURFACE", {"SWEPTSURFACE", "AcDbSweptSurface", "ObjectDBX Classes", true, 4093});
  add("NURBSURFACE", {"NURBSURFACE", "AcDbNurbSurface", "ObjectDBX Classes", true, 0});
  return c;
}
}  // namespace

Writer::Writer(const Value& m) : model(m), classed(all_classed()) {}

void Writer::map_src(const Value& src, double h) {
  if (const Key k = hex_key(src); k && !k->empty()) {
    map_src_key(*k, h);
  }
}
void Writer::map_src_key(const std::string& k, double h) {
  src_map[k] = h;
  if (const auto it = src_index.find(k); it != src_index.end()) {
    src_order[it->second].second = h;
  } else {
    src_index.emplace(k, src_order.size());
    src_order.emplace_back(k, h);
  }
  report.find("handles")->set(k, hex_upper(h));
}
double Writer::src(const Value& hex) const {
  const Key k = hex_key(hex);
  if (!k) {
    return 0;
  }
  const auto it = src_map.find(*k);
  return it == src_map.end() ? 0 : it->second;
}

std::string Writer::clean_name(const std::string& table, const Value& name, const std::string& fallback) {
  auto& used = used_names[table];
  std::string s;
  for (const std::uint32_t c : code_points(str_of(name))) {
    if (c == '|') {
      s += "$0$";
    } else if (bad_name_char(c, true)) {
      s.push_back('_');
    } else {
      put_utf8(s, c);
    }
  }
  std::string clean = slice16(js_trim(s), 255);
  if (clean.empty()) {
    clean = fallback;
  }
  std::string n = clean;
  int k = 2;
  while (used.contains(base::js_to_upper(n))) {
    n = clean + "_" + std::to_string(k++);
  }
  used.insert(base::js_to_upper(n));
  return n;
}
void Writer::reserve(const std::string& table, const std::string& name) {
  used_names[table].insert(base::js_to_upper(name));
}

Value Writer::ltype_ref(const Value& name) const {
  if (nullish(name) || ieq(name, "bylayer")) {
    return make_object({{"flag", 0}});
  }
  if (ieq(name, "byblock")) {
    return make_object({{"flag", 1}});
  }
  double h = 0;
  if (const auto it = lt_by_name.find(upper(name)); it != lt_by_name.end()) {
    h = it->second;
  }
  if (h == 0) {
    h = src(name);
  }
  return h != 0 ? make_object({{"flag", 3}, {"h", h}}) : make_object({{"flag", 0}});
}

double Writer::style_ref(const Value& name) const {
  const std::string k = nullish(name) ? std::string("STANDARD") : upper(name);
  if (const auto it = style_by_name.find(k); it != style_by_name.end() && it->second != 0) {
    return it->second;
  }
  if (const double h = src(name); h != 0) {
    return h;
  }
  const auto it = style_by_name.find("STANDARD");
  return it == style_by_name.end() ? 0 : it->second;
}

double Writer::dim_ref(const Value& name) const {
  const std::string k = nullish(name) ? std::string("STANDARD") : upper(name);
  if (const auto it = dim_by_name.find(k); it != dim_by_name.end() && it->second != 0) {
    return it->second;
  }
  if (const double h = src(name); h != 0) {
    return h;
  }
  const auto it = dim_by_name.find("STANDARD");
  return it == dim_by_name.end() ? 0 : it->second;
}

RecPtr Writer::add_layer(const Value& l) {
  const Value& nm = ref(l, "name");
  const std::string orig = nullish(nm) ? std::string("0") : str_of(nm);
  if (const auto it = layer_by_name.find(orig); it != layer_by_name.end()) {
    return it->second;
  }
  static const std::regex system_layers(R"(^\*(LayerNameForDynamicConstraint|ADSK_CONSTRAINTS|ADSK_SYSTEM_LIGHTS)$)",
                                        std::regex::icase);
  const bool keep = orig == "0" || std::regex_match(orig, system_layers);
  const std::size_t bar = orig.find('|');
  const std::string xref_of = bar != std::string::npos && bar > 0 ? orig.substr(0, bar) : std::string();
  bool bad_nobar = false;
  for (const std::uint32_t c : code_points(orig)) {
    bad_nobar = bad_nobar || bad_name_char(c, false);
  }
  const bool dep = !xref_of.empty() && xref_block_names.contains(base::js_to_upper(xref_of)) && !bad_nobar;
  std::string name;
  if (keep || dep) {
    name = orig;
    reserve("layer", orig);
  } else {
    name = clean_name("layer", orig.empty() ? Value("Unnamed") : Value(orig), "Unnamed");
  }
  if (name != orig) {
    notes.renamed_layers += 1;
    report.find("renamed")->find("layers")->set(orig, name);
  }
  Value color = ref(l, "color");
  if (color.is_number()) {
    color = make_object({{"index", color}});
  }
  const auto idx_ok = [](const Value& c) {
    const double i = std::abs(base::js_to_number(get(c, "index")));
    return i >= 1 && i <= 255;
  };
  if (!color.truthy() || (get(color, "rgb").is_undefined() && !idx_ok(color))) {
    color = make_object({{"index", 7}});
  }
  const Value cidx = get(color, "index");
  const bool off = (ref(l, "off").is_bool() && ref(l, "off").as_bool()) || is_false(ref(l, "on")) ||
                   (!cidx.is_undefined() && base::js_to_number(cidx) < 0);
  if (base::js_to_number(cidx) < 0) {
    Value c2 = color;
    c2.set("index", -base::js_to_number(cidx));
    color = c2;
  }
  if (!get(color, "rgb").is_undefined() && !r2004) {
    lost_rgb += 1;
    color = make_object({{"index", aci_from_rgb(base::js_to_number(get(color, "rgb")))}});
  }
  const double h = orig == "0" ? H["layer0"] : next();
  map_src(ref(l, "handle"), h);
  const Value& lt = ref(l, "linetype");
  double lth = num(get(ltype_ref(lt), "h"));
  if (lth == 0) {
    lth = src(lt);
  }
  if (lth == 0) {
    lth = H["ltContinuous"];
  }
  Value rec = make_object({{"t", "LAYER"},
                           {"h", h},
                           {"owner", H["layerCtl"]},
                           {"name", name},
                           {"color", color},
                           {"off", off},
                           {"frozen", tr(l, "frozen")},
                           {"locked", tr(l, "locked")},
                           {"plot", !is_false(ref(l, "plot"))},
                           {"frozenNew", tr(l, "frozenInNewViewports")}});
  if (dep) {
    rec.set("xdep", true);
    rec.set("xrefOf", xref_of);
  }
  rec.set("lwIndex", ref(l, "lineweight").is_undefined() ? 31 : lw_index(ref(l, "lineweight")));
  rec.set("ltype", lth);
  rec.set("plotstyle", H["plotStyleNormal"]);
  RecPtr r = make_rec(std::move(rec));
  layers.push_back(r);
  layer_by_name[orig] = r;
  layer_by_new[name] = r;
  report.find("layers")->push(
      make_object({{"name", name}, {"color", get(color, "rgb").is_undefined() ? get(color, "index") : color}}));
  return r;
}

double Writer::layer_ref(const Value& name) {
  const std::string k = nullish(name) ? std::string("0") : str_of(name);
  if (const auto it = layer_by_name.find(k); it != layer_by_name.end()) {
    return ref(it->second->v, "h").as_number();
  }
  return ref(add_layer(make_object({{"name", k}}))->v, "h").as_number();
}

double Writer::add_app(const Value& name, const Value& src_h) {
  const std::string up = upper(name);
  if (const auto it = app_by_name.find(up); it != app_by_name.end()) {
    map_src(src_h, it->second);
    return it->second;
  }
  const std::string clean = clean_name("appid", name, "APP");
  const double h = next();
  map_src(src_h, h);
  app_by_name[up] = h;
  appids.push_back(make_rec(make_object({{"t", "APPID"}, {"h", h}, {"owner", H["appidCtl"]}, {"name", clean}})));
  return h;
}

Value Writer::eed_of(const Value& x) {
  if (!x.truthy() || !x.is_object() || x.is_point()) {
    return {};
  }
  Array out;
  for (const base::Member* m : base::js_members(x.as_object())) {
    if (m->value.is_array() && !m->value.as_array().empty()) {
      out.push_back(make_object({{"app", add_app(Value(m->key))}, {"items", m->value}}));
    }
  }
  return out.empty() ? Value() : Value(std::move(out));
}

Value Writer::color(const Value& c0) {
  Value c = c0;
  if (nullish(c)) {
    c = make_object({{"index", 256}});
  } else if (c.is_number()) {
    c = make_object({{"index", c}});
  }
  if (!get(c, "rgb").is_undefined() && !r2004) {
    lost_rgb += 1;
    return make_object({{"index", aci_from_rgb(base::js_to_number(get(c, "rgb")))}});
  }
  return c;
}

namespace {
Value transparency(const Value& t) {
  if (nullish(t)) {
    return {};
  }
  const std::uint32_t v = base::js_to_uint32(base::js_to_number(t));
  if ((v & 0xFF000000U) == 0x02000000U) {
    return static_cast<double>(0x03000000U | (v & 0xFFU));
  }
  if (v == 0x01000000U || v == 0) {
    return static_cast<double>(0x01000000U);
  }
  return {};
}
}  // namespace

Value Writer::common(const Value& e, std::string_view t, double owner) {
  Value r = base::object_with_capacity(16);
  r.append("t", Value(t));
  r.append("h", next());
  r.append("owner", owner);
  r.append("layer", layer_ref(ref(e, "layer")));
  r.append("ltype", ltype_ref(ref(e, "linetype")));
  r.append("color", color(ref(e, "color")));
  r.append("transparency", transparency(ref(e, "transparency")));
  r.append("ltscale", numk(e, "ltscale", 1));
  r.append("lw", lw_index(ref(e, "lineweight")));
  r.append("invisible", tr(e, "invisible"));
  r.append("eed", eed_of(ref(e, "xdata")));
  return r;
}

BlockW* Writer::add_block(const Value* src_b, Value own, const std::string& kind, const std::string& forced) {
  BlockW& B = blocks.emplace_back();
  B.src = src_b;
  B.own = std::move(own);
  const Value& s = B.s();
  const Value& sn = ref(s, "name");
  if (!forced.empty()) {
    B.name = forced;
    reserve("block", B.name);
  } else if (str_of(sn).starts_with("*")) {
    const std::string orig = str_of(sn);
    B.name = "*" + clean_name("anonymous", Value(orig.substr(1)), "U");
    if (B.name != orig) {
      report.find("renamed")->find("blocks")->set(orig, B.name);
    }
  } else {
    B.name = clean_name("block", sn, "Block");
    if (!(sn.is_string() && B.name == sn.as_string())) {
      notes.renamed_blocks += 1;
      report.find("renamed")->find("blocks")->set(base::js_string(sn), B.name);
    }
  }
  B.kind = kind;
  block_by_name[upper(sn)] = &B;
  block_by_name[base::js_to_upper(B.name)] = &B;
  return &B;
}

BlockW* Writer::find_block(const Value& v) {
  if (const auto it = block_by_name.find(upper(nullish(v) ? Value("") : v)); it != block_by_name.end()) {
    return it->second;
  }
  const Key k = hex_key(v);
  for (BlockW& b : blocks) {
    if (hex_key(ref(b.s(), "record")) == k) {
      return &b;
    }
  }
  return nullptr;
}

std::size_t Writer::block_index(const BlockW* b) const {
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    if (&blocks[i] == b) {
      return i;
    }
  }
  return blocks.size();
}

void Writer::random_bytes(std::span<std::uint8_t> b) const {
  if (random) {
    random(b);
    return;
  }
  thread_local std::random_device rd;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
  for (std::uint8_t& x : b) {
    x = static_cast<std::uint8_t>(rd() & 0xFFU);
  }
}

std::string Writer::guid() const {
  std::array<std::uint8_t, 16> r{};
  random_bytes(r);
  std::string h;
  for (const std::uint8_t b : r) {
    h += hex_digits(b, true, 2);
  }
  return "{" + h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" +
         h.substr(20) + "}";
}

// ---------------------------------------------------------------- the conversion of one entity

namespace {

using Out = std::variant<std::vector<RecPtr>, std::string, std::monostate>;  // records | skip reason | invalid

const char* dim_type(int t) {
  switch (t & 7) {
    case 0: return "DIMENSION_LINEAR";
    case 1: return "DIMENSION_ALIGNED";
    case 2: return "DIMENSION_ANG2LN";
    case 3: return "DIMENSION_DIAMETER";
    case 4: return "DIMENSION_RADIUS";
    case 5: return "DIMENSION_ANG3PT";
    case 6: return "DIMENSION_ORDINATE";
    default: return "DIMENSION_LINEAR";
  }
}

struct Conv {
  Writer& W;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): the conversion of one write
  std::unordered_map<std::string, const Value*> image_defs;  // source hex → IMAGEDEF object
  std::unordered_map<std::string, RecPtr> written_defs;
  std::vector<RecPtr> written_defs_order;
  std::set<std::string> def_names;
  bool need_wipeout_vars = false;

  [[nodiscard]] Value text_fields(const Value& e) const {
    Value o = Value::object();
    o.set("insert", P3w(ref(e, "insert")));
    o.set("align", ref(e, "align").truthy() ? P3w(ref(e, "align")) : Value(nullptr));
    o.set("height", numk(e, "height", 0.2));
    const Value& v = ref(e, "value");
    o.set("value", str_value(nullish(v) ? ref(e, "text") : v));
    o.set("rotation", numk(e, "rotation"));
    o.set("widthFactor", numk(e, "widthFactor", 1));
    o.set("oblique", numk(e, "oblique"));
    o.set("style", W.style_ref(ref(e, "style")));
    o.set("hAlign", ivk(e, "hAlign"));
    o.set("vAlign", ivk(e, "vAlign"));
    o.set("generation", ivk(e, "generation"));
    o.set("thickness", numk(e, "thickness"));
    o.set("extrusion", ref(e, "extrusion"));
    return o;
  }
  static void spread(Value& into, const Value& from) {
    for (const base::Member& m : from.as_object()) {
      into.set(m.key, m.value);
    }
  }
  RecPtr image_def(const Value& srcv) {
    const std::string kk = null_key(hex_key(ref(srcv, "handle")));
    if (const auto it = written_defs.find(kk); it != written_defs.end()) {
      return it->second;
    }
    std::string fname = ref(srcv, "fileName").truthy() ? str_of(ref(srcv, "fileName")) : std::string("image");
    if (const auto p = fname.find_last_of("\\/"); p != std::string::npos) {
      fname = fname.substr(p + 1);
    }
    if (const auto d = fname.rfind('.'); d != std::string::npos) {
      fname = fname.substr(0, d);
    }
    std::string base;
    for (const std::uint32_t c : code_points(fname)) {
      if (bad_name_char(c, true)) {
        base.push_back('_');
      } else {
        put_utf8(base, c);
      }
    }
    if (base.empty()) {
      base = "image";
    }
    std::string n = base;
    int i = 2;
    while (def_names.contains(base::js_to_upper(n))) {
      n = base + "_" + std::to_string(i++);
    }
    def_names.insert(base::js_to_upper(n));
    const Value& fn = ref(srcv, "fileName");
    RecPtr rec = make_rec(make_object({{"t", "IMAGEDEF"},
                                       {"h", W.next()},
                                       {"name", n},
                                       {"classVersion", ivk(srcv, "classVersion")},
                                       {"size", ref(srcv, "size")},
                                       {"fileName", fn.truthy() ? fn : Value("")},
                                       {"loaded", !is_false(ref(srcv, "loaded"))},
                                       {"resUnits", ivk(srcv, "resUnits")},
                                       {"pixelSize", ref(srcv, "pixelSize")},
                                       {"reactors", Array{}},
                                       {"ownerCode", 3}}));
    written_defs[kk] = rec;
    written_defs_order.push_back(rec);
    W.map_src(ref(srcv, "handle"), ref(rec->v, "h").as_number());
    return rec;
  }

  Out convert(const Value& e, double owner) {
    std::string type = upper(orv_or_empty(ref(e, "type")));
    if (type == "DIMENSION") {
      type = dim_type(base::js_to_int32(nnum(ref(e, "dimType"), 0)));
    }
    if (type == "POLYLINE") {
      const int f = base::js_to_int32(base::js_to_number(ref(e, "flags")));
      if ((f & 64) != 0) {
        type = "POLYLINE_PFACE";
      } else if ((f & 16) != 0) {
        type = "POLYLINE_MESH";
      } else if ((f & 8) != 0) {
        type = "POLYLINE_3D";
      } else {
        type = "POLYLINE_2D";
      }
    }
    const auto c = [&](std::string_view t) { return W.common(e, t, owner); };
    if (auto xr = W.X->convert(e, type, owner)) {
      if (std::holds_alternative<std::string>(*xr)) {
        return std::get<std::string>(*xr);
      }
      return std::get<std::vector<RecPtr>>(*xr);
    }
    if (type == "ACAD_TABLE") {
      auto r = W.TBL->convert(e, owner);
      if (std::holds_alternative<std::string>(r)) {
        return std::get<std::string>(r);
      }
      return std::get<std::vector<RecPtr>>(r);
    }
    if (auto su = W.SOL->convert_surface(e, type, owner)) {
      return std::move(*su);
    }
    if (auto sr = W.SOL->convert_solid(e, type, owner, [&](const std::string& t) { return make_rec(c(t)); })) {
      return std::move(*sr);
    }
    const auto one = [](Value v) { return std::vector<RecPtr>{make_rec(std::move(v))}; };
    if (type == "LINE") {
      if (!okP(ref(e, "start")) || !okP(ref(e, "end"))) {
        return std::monostate{};
      }
      Value r = c("LINE");
      r.set("start", P3w(ref(e, "start")));
      r.set("end", P3w(ref(e, "end")));
      r.set("thickness", numk(e, "thickness"));
      r.set("extrusion", ref(e, "extrusion"));
      return one(std::move(r));
    }
    if (type == "POINT") {
      if (!okP(ref(e, "position"))) {
        return std::monostate{};
      }
      Value r = c("POINT");
      r.set("position", P3w(ref(e, "position")));
      r.set("thickness", numk(e, "thickness"));
      r.set("extrusion", ref(e, "extrusion"));
      r.set("xAxisAngle", numk(e, "xAxisAngle"));
      return one(std::move(r));
    }
    if (type == "CIRCLE") {
      if (!okP(ref(e, "center")) || !(base::js_to_number(ref(e, "radius")) > 0)) {
        return std::monostate{};
      }
      Value r = c("CIRCLE");
      r.set("center", P3w(ref(e, "center")));
      r.set("radius", ref(e, "radius"));
      r.set("thickness", numk(e, "thickness"));
      r.set("extrusion", ref(e, "extrusion"));
      return one(std::move(r));
    }
    if (type == "ARC") {
      if (!okP(ref(e, "center")) || !(base::js_to_number(ref(e, "radius")) > 0) || !ref(e, "startAngle").is_number() ||
          !std::isfinite(ref(e, "startAngle").as_number()) || !ref(e, "endAngle").is_number() ||
          !std::isfinite(ref(e, "endAngle").as_number())) {
        return std::monostate{};
      }
      Value r = c("ARC");
      r.set("center", P3w(ref(e, "center")));
      r.set("radius", ref(e, "radius"));
      r.set("startAngle", ref(e, "startAngle"));
      r.set("endAngle", ref(e, "endAngle"));
      r.set("thickness", numk(e, "thickness"));
      r.set("extrusion", ref(e, "extrusion"));
      return one(std::move(r));
    }
    if (type == "ELLIPSE") {
      if (!okP(ref(e, "center")) || !okP(ref(e, "majorAxis")) || !(base::js_to_number(ref(e, "ratio")) > 0)) {
        return std::monostate{};
      }
      Value maj = ref(e, "majorAxis");
      double ratio = base::js_to_number(ref(e, "ratio"));
      const Value& sp = ref(e, "startParam");
      const Value& ep = ref(e, "endParam");
      double s = sp.is_undefined() ? 0 : base::js_to_number(sp);
      double en = ep.is_undefined() ? 2 * std::numbers::pi : base::js_to_number(ep);
      if (ratio > 1) {
        const double mx = base::js_to_number(get(maj, "x"));
        const double my = base::js_to_number(get(maj, "y"));
        maj = make_object({{"x", -my * ratio}, {"y", mx * ratio}, {"z", numk(maj, "z") * ratio}});
        ratio = 1 / ratio;
        s -= std::numbers::pi / 2;
        en -= std::numbers::pi / 2;
      }
      Value r = c("ELLIPSE");
      r.set("center", P3w(ref(e, "center")));
      r.set("majorAxis", P3w(maj));
      r.set("ratio", ratio);
      r.set("startParam", s);
      r.set("endParam", en);
      r.set("extrusion", ref(e, "extrusion"));
      return one(std::move(r));
    }
    if (type == "RAY" || type == "XLINE") {
      if (!okP(ref(e, "start")) || !okP(ref(e, "direction"))) {
        return std::monostate{};
      }
      const Value d = P3w(ref(e, "direction"));
      const double dx = d.get("x").as_number();
      const double dy = d.get("y").as_number();
      const double dz = d.get("z").as_number();
      const double n = std::hypot(dx, dy, dz);
      if (!(n > 0)) {
        return std::monostate{};
      }
      Value r = c(type);
      r.set("start", P3w(ref(e, "start")));
      r.set("direction", base::pt3(dx / n, dy / n, dz / n));
      return one(std::move(r));
    }
    if (type == "SOLID" || type == "TRACE" || type == "3DFACE") {
      Array pts;
      for (const Value& p : arr(e, "points")) {
        if (okP(p)) {
          pts.push_back(P3w(p));
        }
      }
      if (pts.size() < 3) {
        return std::monostate{};
      }
      while (pts.size() < 4) {
        pts.push_back(pts.back());
      }
      Value r = c(type);
      const double z0 = pts[0].get("z").as_number();
      r.set("points", std::move(pts));
      r.set("thickness", numk(e, "thickness"));
      r.set("extrusion", ref(e, "extrusion"));
      r.set("invisibleEdges", ivk(e, "invisibleEdges"));
      r.set("elevation", numk(e, "elevation", z0));
      return one(std::move(r));
    }
    if (type == "LWPOLYLINE") {
      Array v;
      for (const Value& p : arr(e, "vertices")) {
        if (okP(p)) {
          v.push_back(p);
        }
      }
      if (v.size() < 2) {
        return std::monostate{};
      }
      Value r = c("LWPOLYLINE");
      r.set("vertices", std::move(v));
      const Value& cl = ref(e, "closed");
      r.set("closed",
            cl.is_undefined() ? (base::js_to_int32(base::js_to_number(ref(e, "flags"))) & 512) != 0 : cl.truthy());
      r.set("constantWidth", numk(e, "constantWidth"));
      r.set("elevation", numk(e, "elevation"));
      r.set("thickness", numk(e, "thickness"));
      r.set("extrusion", ref(e, "extrusion"));
      r.set("plinegen", tr(e, "plinegen"));
      return one(std::move(r));
    }
    if (type == "POLYLINE_2D" || type == "POLYLINE_3D") {
      Array v;
      for (const Value& p : arr(e, "vertices")) {
        if (okP(p)) {
          v.push_back(p);
        }
      }
      if (v.size() < 2) {
        return std::monostate{};
      }
      const bool is3d = type == "POLYLINE_3D";
      Value head = c(type);
      const Value& cl = ref(e, "closed");
      const int ef = base::js_to_int32(base::js_to_number(ref(e, "flags")));
      const bool closed = cl.is_undefined() ? (ef & 1) != 0 : cl.truthy();
      head.set("closed", closed);
      const double hh = head.get("h").as_number();
      std::vector<RecPtr> out;
      out.push_back(nullptr);
      Array vh;
      for (const Value& p : v) {
        Value x = W.common(e, is3d ? "VERTEX_3D" : "VERTEX_2D", hh);
        x.set("mode", 0);
        x.set("point", P3w(p));
        x.set("flags", is3d ? 32 : ivk(p, "flags"));
        x.set("bulge", numk(p, "bulge"));
        x.set("startWidth", numk(p, "startWidth"));
        x.set("endWidth", numk(p, "endWidth"));
        x.set("id", ivk(p, "id"));
        x.set("tangent", numk(p, "tangent"));
        vh.push_back(x.get("h"));
        out.push_back(make_rec(std::move(x)));
      }
      Value seq = W.common(e, "SEQEND", hh);
      seq.set("mode", 0);
      if (is3d) {
        head.set("splineFlags", (ef & 4) != 0 ? 1 : 0);
      } else {
        head.set("flags", static_cast<double>(base::js_to_int32(iv(ref(e, "flags"))) | (closed ? 1 : 0)));
        head.set("curveType", ivk(e, "curveType"));
        head.set("startWidth", numk(e, "startWidth"));
        head.set("endWidth", numk(e, "endWidth"));
        head.set("thickness", numk(e, "thickness"));
        head.set("elevation", numk(e, "elevation"));
        head.set("extrusion", ref(e, "extrusion"));
      }
      head.set("vertices", std::move(vh));
      head.set("seqend", seq.get("h"));
      out[0] = make_rec(std::move(head));
      out.push_back(make_rec(std::move(seq)));
      return out;
    }
    if (type == "POLYLINE_PFACE") {
      Array v;
      for (const Value& p : arr(e, "vertices")) {
        if (okP(p)) {
          v.push_back(p);
        }
      }
      const Array& faces = arr(e, "faces");
      if (v.size() < 3 || faces.empty()) {
        return std::monostate{};
      }
      Value head = c(type);
      head.set("numVerts", static_cast<double>(v.size()));
      head.set("numFaces", static_cast<double>(faces.size()));
      const double hh = head.get("h").as_number();
      std::vector<RecPtr> out;
      out.push_back(nullptr);
      Array vh;
      for (const Value& p : v) {
        Value x = W.common(e, "VERTEX_PFACE", hh);
        x.set("mode", 0);
        x.set("point", P3w(p));
        x.set("flags", 192);
        vh.push_back(x.get("h"));
        out.push_back(make_rec(std::move(x)));
      }
      for (const Value& f : faces) {
        Value x = W.common(e, "VERTEX_PFACE_FACE", hh);
        x.set("mode", 0);
        const Value& src_i = ref(f, "indices").truthy() ? ref(f, "indices") : f;
        Array ix;
        if (src_i.is_array()) {
          for (std::size_t i = 0; i < 4 && i < src_i.as_array().size(); ++i) {
            ix.push_back(src_i.as_array()[i]);
          }
        }
        while (ix.size() < 4) {
          ix.emplace_back(0);
        }
        x.set("indices", std::move(ix));
        vh.push_back(x.get("h"));
        out.push_back(make_rec(std::move(x)));
      }
      Value seq = W.common(e, "SEQEND", hh);
      seq.set("mode", 0);
      head.set("vertices", std::move(vh));
      head.set("seqend", seq.get("h"));
      out[0] = make_rec(std::move(head));
      out.push_back(make_rec(std::move(seq)));
      return out;
    }
    if (type == "POLYLINE_MESH") {
      Array v;
      for (const Value& p : arr(e, "vertices")) {
        if (okP(p)) {
          v.push_back(p);
        }
      }
      const double m = ivk(e, "m");
      const double n = ivk(e, "n");
      if (m == 0 || n == 0 || static_cast<double>(v.size()) != m * n) {
        return std::monostate{};
      }
      Value head = c(type);
      head.set("flags", static_cast<double>(base::js_to_int32(ivk(e, "flags")) | 16));
      head.set("curveType", ivk(e, "curveType"));
      head.set("m", m);
      head.set("n", n);
      head.set("mDensity", ivk(e, "mDensity"));
      head.set("nDensity", ivk(e, "nDensity"));
      const double hh = head.get("h").as_number();
      std::vector<RecPtr> out;
      out.push_back(nullptr);
      Array vh;
      for (const Value& p : v) {
        Value x = W.common(e, "VERTEX_MESH", hh);
        x.set("mode", 0);
        x.set("point", P3w(p));
        x.set("flags", 64);
        vh.push_back(x.get("h"));
        out.push_back(make_rec(std::move(x)));
      }
      Value seq = W.common(e, "SEQEND", hh);
      seq.set("mode", 0);
      head.set("vertices", std::move(vh));
      head.set("seqend", seq.get("h"));
      out[0] = make_rec(std::move(head));
      out.push_back(make_rec(std::move(seq)));
      return out;
    }
    if (type == "TEXT") {
      if (!okP(ref(e, "insert")) || !(base::js_to_number(ref(e, "height")) > 0)) {
        return std::monostate{};
      }
      Value r = c("TEXT");
      spread(r, text_fields(e));
      return one(std::move(r));
    }
    if (type == "ATTDEF") {
      if (!okP(ref(e, "insert")) || !(base::js_to_number(ref(e, "height")) > 0) || !tr(e, "tag")) {
        return std::monostate{};
      }
      Value r = c("ATTDEF");
      spread(r, text_fields(e));
      r.set("tag", tag_of(ref(e, "tag")));
      r.set("prompt", str_value(ref(e, "prompt")));
      r.set("flags", ivk(e, "flags"));
      r.set("lockPosition", tr(e, "lockPosition"));
      return one(std::move(r));
    }
    if (type == "MTEXT") {
      if (!okP(ref(e, "insert"))) {
        return std::monostate{};
      }
      const auto ior = [&](std::string_view k, double d) {
        const double x = ivk(e, k);
        return x != 0 ? x : d;
      };
      Value r = c("MTEXT");
      r.set("insert", P3w(ref(e, "insert")));
      r.set("xAxis", ref(e, "xAxis").truthy() ? P3w(ref(e, "xAxis")) : Value(nullptr));
      r.set("rotation", numk(e, "rotation"));
      r.set("width", numk(e, "width"));
      r.set("height", numk(e, "height", 0.2));
      r.set("attach", ior("attach", 1));
      r.set("direction", ior("direction", 1));
      const Value& v = ref(e, "value");
      r.set("value", str_value(nullish(v) ? ref(e, "text") : v));
      r.set("style", W.style_ref(ref(e, "style")));
      r.set("lineSpacing", numk(e, "lineSpacing", 1));
      r.set("lineSpacingStyle", ior("lineSpacingStyle", 1));
      r.set("extWidth", numk(e, "extWidth"));
      r.set("extHeight", numk(e, "extHeight"));
      r.set("rectHeight", numk(e, "rectHeight"));
      r.set("extrusion", ref(e, "extrusion"));
      r.set("backgroundFlags", ivk(e, "backgroundFlags"));
      r.set("backgroundScale", numk(e, "backgroundScale", 1.5));
      r.set("backgroundColor", ref(e, "backgroundColor"));
      r.set("backgroundTransparency", ivk(e, "backgroundTransparency"));
      return one(std::move(r));
    }
    if (type == "INSERT" || type == "MINSERT") {
      BlockW* B = W.find_block(ref(e, "block"));
      if (B == nullptr || B->kind != "block") {
        return std::string("INSERT of a block the drawing does not define");
      }
      if (!okP(ref(e, "insert"))) {
        return std::monostate{};
      }
      const auto ior = [&](std::string_view k, double d) {
        const double x = ivk(e, k);
        return x != 0 ? x : d;
      };
      Value head = c(type);
      head.set("insert", P3w(ref(e, "insert")));
      const Value& sc = ref(e, "scale");
      head.set("scale", base::pt3(numk(sc, "x", 1), numk(sc, "y", 1), numk(sc, "z", 1)));
      head.set("rotation", numk(e, "rotation"));
      head.set("extrusion", ref(e, "extrusion"));
      head.set("blockRef", static_cast<double>(W.block_index(B)));
      head.set("cols", ior("cols", 1));
      head.set("rows", ior("rows", 1));
      head.set("colSpacing", numk(e, "colSpacing"));
      head.set("rowSpacing", numk(e, "rowSpacing"));
      const double hh = head.get("h").as_number();
      std::vector<RecPtr> out;
      out.push_back(nullptr);
      Array ah;
      for (const Value& a : arr(e, "attribs")) {
        if (!okP(ref(a, "insert")) || base::js_to_number(ref(a, "height")) <= 0 || !tr(a, "tag")) {
          continue;
        }
        Value merged = e;
        spread(merged, a);
        merged.set("layer", nullish(ref(a, "layer")) ? ref(e, "layer") : ref(a, "layer"));
        merged.set("color", nullish(ref(a, "color")) ? ref(e, "color") : ref(a, "color"));
        Value x = W.common(merged, "ATTRIB", hh);
        x.set("mode", 0);
        spread(x, text_fields(a));
        x.set("tag", tag_of(ref(a, "tag")));
        x.set("flags", ivk(a, "flags"));
        x.set("lockPosition", tr(a, "lockPosition"));
        ah.push_back(x.get("h"));
        out.push_back(make_rec(std::move(x)));
      }
      RecPtr seq;
      if (!ah.empty()) {
        Value s = W.common(e, "SEQEND", hh);
        s.set("mode", 0);
        seq = make_rec(std::move(s));
      }
      const std::size_t na = ah.size();
      head.set("attribs", std::move(ah));
      head.set("seqend", seq ? ref(seq->v, "h") : Value(0));
      B->inserts.push_back(hh);
      if (na != 0) {
        Value* counts = W.report.find("counts");
        counts->set("ATTRIB", num(ref(*counts, "ATTRIB")) + static_cast<double>(na));
      }
      out[0] = make_rec(std::move(head));
      if (seq) {
        out.push_back(seq);
      }
      return out;
    }
    if (type.starts_with("DIMENSION_") || type == "ARC_DIMENSION" || type == "LARGE_RADIAL_DIMENSION") {
      BlockW const* B = W.find_block(ref(e, "block"));
      if ((B == nullptr || B->kind != "block") && tr(e, "block")) {
        return std::string("DIMENSION without its block");
      }
      const Value& fl = ref(e, "flags");
      const int f70 = base::js_to_int32(base::js_to_number(nullish(fl) ? nn_or(ref(e, "dimType"), 0) : fl));
      Value chord = ref(e, "chordPoint");
      if (nullish(chord)) {
        chord = type == "DIMENSION_RADIUS" || type == "DIMENSION_DIAMETER" ? ref(e, "center") : ref(e, "xline1");
      }
      const auto ior = [&](std::string_view k, double d) {
        const double x = ivk(e, k);
        return x != 0 ? x : d;
      };
      Value r = c(type);
      r.set("blockRef", B != nullptr ? static_cast<double>(W.block_index(B)) : -1.0);
      r.set("dimstyle", W.dim_ref(ref(e, "style")));
      r.set("defPoint", P3w(ref(e, "defPoint")));
      r.set("textMid", P3w(ref(e, "textMid")));
      r.set("insert", ref(e, "insert").truthy() ? P3w(ref(e, "insert")) : Value(nullptr));
      r.set("xline1", P3w(ref(e, "xline1")));
      r.set("xline2", P3w(ref(e, "xline2")));
      r.set("center", P3w(nullish(ref(e, "center")) ? ref(e, "xline1End") : ref(e, "center")));
      r.set("arcPoint", P3w(ref(e, "arcPoint")));
      r.set("chordPoint", P3w(chord));
      r.set("angle", numk(e, "angle"));
      r.set("obliqueAngle", numk(e, "obliqueAngle"));
      r.set("leaderLen", numk(e, "leaderLen"));
      r.set("measurement", numk(e, "measurement"));
      r.set("text", str_value(ref(e, "text")));
      r.set("attach", ior("attach", 5));
      r.set("textRotation", num(nullish(ref(e, "textRotation")) ? ref(e, "rotation") : ref(e, "textRotation")));
      r.set("horizDir", numk(e, "horizDir"));
      r.set("extrusion", ref(e, "extrusion"));
      r.set("insScale", ref(e, "insScale"));
      r.set("insRotation", numk(e, "insRotation"));
      r.set("lineSpacing", numk(e, "lineSpacing", 1));
      r.set("lineSpacingStyle", ior("lineSpacingStyle", 1));
      r.set("flipArrow1", tr(e, "flipArrow1"));
      r.set("flipArrow2", tr(e, "flipArrow2"));
      r.set("ordinateX", tr(e, "ordinateX") || (f70 & 64) != 0);
      r.set("flags1", static_cast<double>(((f70 & 128) != 0 ? 0 : 1) | ((f70 & 32) != 0 ? 2 : 0)));
      r.set("isPartial", tr(e, "isPartial"));
      r.set("arcStart", numk(e, "arcStart"));
      r.set("arcEnd", numk(e, "arcEnd"));
      r.set("hasLeader", tr(e, "hasLeader"));
      r.set("leader1", P3w(ref(e, "leader1")));
      r.set("leader2", P3w(ref(e, "leader2")));
      r.set("overrideCenter", P3w(ref(e, "overrideCenter")));
      r.set("jogPoint", P3w(ref(e, "jogPoint")));
      return one(std::move(r));
    }
    if (type == "LEADER") {
      Array v;
      for (const Value& p : arr(e, "vertices")) {
        if (okP(p)) {
          v.push_back(P3w(p));
        }
      }
      if (v.size() < 2) {
        return std::monostate{};
      }
      Value r = c("LEADER");
      const Value v0 = v[0];
      r.set("vertices", std::move(v));
      r.set("annotType", nn_or(ref(e, "annotType"), 3));
      r.set("pathType", ivk(e, "pathType"));
      r.set("origin", ref(e, "origin").truthy() ? P3w(ref(e, "origin")) : v0);
      r.set("extrusion", ref(e, "extrusion"));
      r.set("xDirection", ref(e, "xDirection"));
      r.set("blockOffset", ref(e, "blockOffset"));
      r.set("endPointProj", ref(e, "endPointProj"));
      r.set("boxHeight", numk(e, "boxHeight"));
      r.set("boxWidth", numk(e, "boxWidth"));
      r.set("hookLineOnX", tr(e, "hookLineOnX"));
      r.set("arrowhead", !is_false(ref(e, "arrowhead")));
      r.set("annotationSrc", ref(e, "annotation"));
      r.set("dimstyle", W.dim_ref(ref(e, "style")));
      return one(std::move(r));
    }
    if (type == "IMAGE" || type == "WIPEOUT") {
      if (!okP(ref(e, "insert")) || !okP(ref(e, "u")) || !okP(ref(e, "v"))) {
        return std::monostate{};
      }
      Value head = c(type);
      head.set("classVersion", ivk(e, "classVersion"));
      head.set("insert", P3w(ref(e, "insert")));
      head.set("u", P3w(ref(e, "u")));
      head.set("v", P3w(ref(e, "v")));
      head.set("size", ref(e, "size"));
      head.set("displayFlags", nn_or(ref(e, "displayFlags"), 7));
      head.set("clipping", tr(e, "clipping"));
      head.set("brightness", nn_or(ref(e, "brightness"), 50));
      head.set("contrast", nn_or(ref(e, "contrast"), 50));
      head.set("fade", ivk(e, "fade"));
      head.set("clipMode", tr(e, "clipMode"));
      head.set("clipType", ref(e, "clipType"));
      head.set("clip", ref(e, "clip").truthy() ? ref(e, "clip") : Value(Array{}));
      double def = 0;
      RecPtr reactor;
      if (type == "IMAGE") {
        const auto it = image_defs.find(null_key(hex_key(ref(e, "def"))));
        if (it == image_defs.end()) {
          return std::string("IMAGE whose image definition is not in the model");
        }
        RecPtr d = image_def(*it->second);
        def = ref(d->v, "h").as_number();
        reactor = make_rec(
            make_object({{"t", "IMAGEDEF_REACTOR"}, {"h", W.next()}, {"owner", head.get("h")}, {"classVersion", 2}}));
        d->v.find("reactors")->push(ref(reactor->v, "h"));
      } else {
        need_wipeout_vars = true;
      }
      head.set("def", def);
      head.set("defReactor", reactor ? ref(reactor->v, "h") : Value(0));
      std::vector<RecPtr> out{make_rec(std::move(head))};
      if (reactor) {
        out.push_back(reactor);
      }
      return out;
    }
    if (type == "TOLERANCE") {
      if (!okP(ref(e, "insert"))) {
        return std::monostate{};
      }
      Value r = c("TOLERANCE");
      r.set("insert", P3w(ref(e, "insert")));
      r.set("xDirection", ref(e, "xDirection"));
      r.set("extrusion", ref(e, "extrusion"));
      r.set("text", str_value(ref(e, "text")));
      r.set("dimstyle", W.dim_ref(ref(e, "style")));
      return one(std::move(r));
    }
    if (type == "MLINE") {
      Array v;
      for (const Value& x : arr(e, "vertices")) {
        if (okP(ref(x, "point"))) {
          v.push_back(x);
        }
      }
      if (v.size() < 2) {
        return std::monostate{};
      }
      const auto nlines = [](const Value& x) {
        const Value& l = ref(x, "lines");
        return l.is_array() ? static_cast<double>(l.as_array().size()) : 2.0;
      };
      if (nlines(v[0]) != 2 || std::ranges::any_of(v, [&](const Value& x) { return nlines(x) != 2; })) {
        return std::string("MLINE (multiline style other than Standard)");
      }
      Value r = c("MLINE");
      r.set("scale", numk(e, "scale", 1));
      r.set("justification", ivk(e, "justification"));
      r.set("base", P3w(nullish(ref(e, "base")) ? ref(v[0], "point") : ref(e, "base")));
      r.set("extrusion", ref(e, "extrusion"));
      r.set("closed", tr(e, "closed"));
      r.set("lines", 2);
      Array vs;
      for (const Value& x : v) {
        vs.push_back(make_object({{"point", P3w(ref(x, "point"))},
                                  {"direction", P3w(ref(x, "direction"))},
                                  {"miter", P3w(ref(x, "miter"))},
                                  {"lines", ref(x, "lines")}}));
      }
      r.set("vertices", std::move(vs));
      r.set("style", W.H["mlstyle"]);
      return one(std::move(r));
    }
    if (type == "SHAPE") {
      if (!okP(ref(e, "insert"))) {
        return std::monostate{};
      }
      const Value& sn = ref(e, "style");
      double st = 0;
      if (const auto it = W.style_by_name.find(upper(nullish(sn) ? Value("") : sn)); it != W.style_by_name.end()) {
        st = it->second;
      }
      if (st == 0) {
        st = W.src(sn);
      }
      if (st == 0) {
        return std::string("SHAPE (its shape file is not in the styles table)");
      }
      Value r = c("SHAPE");
      r.set("insert", P3w(ref(e, "insert")));
      r.set("scale", numk(e, "scale", 1));
      r.set("rotation", numk(e, "rotation"));
      r.set("widthFactor", numk(e, "widthFactor", 1));
      r.set("oblique", numk(e, "oblique"));
      r.set("thickness", numk(e, "thickness"));
      r.set("shapeNumber", ivk(e, "shapeNumber"));
      r.set("extrusion", ref(e, "extrusion"));
      r.set("style", st);
      return one(std::move(r));
    }
    if (type == "HATCH") {
      Array loops;
      for (const Value& l : arr(e, "loops")) {
        if (tr(l, "polyline") || tr(l, "vertices")) {
          Value pl = ref(l, "polyline").truthy()
                         ? ref(l, "polyline")
                         : make_object({{"vertices", ref(l, "vertices")}, {"closed", ref(l, "closed")}});
          Array v;
          for (const Value& p : arr(pl, "vertices")) {
            if (okP(p)) {
              v.push_back(p);
            }
          }
          if (v.size() >= 2) {
            loops.push_back(make_object(
                {{"flags", ivk(l, "flags")},
                 {"polyline", make_object({{"vertices", std::move(v)}, {"closed", !is_false(ref(pl, "closed"))}})}}));
          }
          continue;
        }
        Array edges;
        for (const Value& x : arr(l, "edges")) {
          if (x.truthy() && tr(x, "type")) {
            edges.push_back(x);
          }
        }
        if (!edges.empty()) {
          loops.push_back(make_object({{"flags", ivk(l, "flags")}, {"edges", std::move(edges)}}));
        }
      }
      if (loops.empty()) {
        return std::monostate{};
      }
      const bool solid = tr(e, "solid");
      Value r = c("HATCH");
      r.set("pattern", ref(e, "pattern").truthy() ? ref(e, "pattern") : Value(solid ? "SOLID" : "ANSI31"));
      r.set("solid", solid);
      r.set("style", ivk(e, "style"));
      r.set("patternType", nn_or(ref(e, "patternType"), 1));
      r.set("angle", numk(e, "angle"));
      r.set("scale", numk(e, "scale", 1));
      r.set("double", tr(e, "double"));
      Value lines = ref(e, "lines").truthy() ? ref(e, "lines") : ref(e, "patternLines");
      r.set("lines", lines.truthy() ? lines : Value(Array{}));
      r.set("loops", std::move(loops));
      Array seeds;
      for (const Value& s : arr(e, "seeds")) {
        if (okP(s)) {
          seeds.push_back(s);
        }
      }
      r.set("seeds", std::move(seeds));
      r.set("gradient", ref(e, "gradient"));
      r.set("elevation", numk(e, "elevation"));
      r.set("extrusion", ref(e, "extrusion"));
      return one(std::move(r));
    }
    if (type == "SPLINE") {
      Array fit;
      for (const Value& p : arr(e, "fit")) {
        if (okP(p)) {
          fit.push_back(p);
        }
      }
      Array ctrl;
      if (!tr(e, "controlComputed") || fit.size() < 2) {
        for (const Value& p : arr(e, "control")) {
          if (okP(p)) {
            ctrl.push_back(p);
          }
        }
      }
      if (ctrl.size() < 2 && fit.size() < 2) {
        return std::monostate{};
      }
      const Value& weights = ref(e, "weights");
      Value r = c("SPLINE");
      const double deg = ivk(e, "degree");
      r.set("degree", deg != 0 ? deg : 3);
      r.set("flags", ivk(e, "flags"));
      r.set("knotParam", ref(e, "knotParam"));
      r.set("knots", !ctrl.empty() ? or_value(ref(e, "knots"), Value(Array{})) : Value(Array{}));
      r.set("weights", weights);
      Array cp;
      for (std::size_t i = 0; i < ctrl.size(); ++i) {
        Value q = make_object({{"x", numk(ctrl[i], "x")}, {"y", numk(ctrl[i], "y")}, {"z", numk(ctrl[i], "z")}});
        Value wv = get(ctrl[i], "w");
        if (nullish(wv) && weights.is_array() && i < weights.as_array().size()) {
          wv = weights.as_array()[i];
        }
        if (!wv.is_undefined()) {
          q.set("w", wv);
        }
        cp.push_back(std::move(q));
      }
      r.set("control", std::move(cp));
      Array fp;
      for (const Value& p : fit) {
        fp.push_back(P3w(p));
      }
      r.set("fit", std::move(fp));
      r.set("startTangent", ref(e, "startTangent"));
      r.set("endTangent", ref(e, "endTangent"));
      const Value& tol = ref(e, "tolerances");
      const auto tolv = [&](std::string_view k, std::string_view alt) {
        const Value v = get(tol, k);
        return nullish(v) ? ref(e, alt) : v;
      };
      r.set("fitTolerance", num(tolv("fit", "fitTolerance")));
      r.set("knotTolerance", num(tolv("knot", "knotTolerance"), 1e-10));
      r.set("controlTolerance", num(tolv("control", "controlTolerance"), 1e-10));
      return one(std::move(r));
    }
    if (type == "VIEWPORT") {
      if (!okP(ref(e, "center"))) {
        return std::monostate{};
      }
      Value r = c("VIEWPORT");
      r.set("center", P3w(ref(e, "center")));
      r.set("width", numk(e, "width", 1));
      r.set("height", numk(e, "height", 1));
      r.set("viewCenter", P3w(ref(e, "viewCenter")));
      r.set("viewTarget", P3w(ref(e, "viewTarget")));
      r.set("viewHeight", numk(e, "viewHeight", numk(e, "height", 1)));
      r.set("twist", numk(e, "twist"));
      r.set("status", nn_or(ref(e, "status"), 0x8060));
      r.set("id", ivk(e, "id"));
      Array fz;
      for (const Value& x : arr(e, "frozenLayers")) {
        double h = W.src(x);
        if (h == 0 && x.is_string()) {
          if (const auto it = W.layer_by_name.find(x.as_string()); it != W.layer_by_name.end()) {
            h = ref(it->second->v, "h").as_number();
          }
        }
        if (h != 0) {
          fz.emplace_back(h);
        }
      }
      r.set("frozenLayers", std::move(fz));
      return one(std::move(r));
    }
    return type.empty() ? std::string("(no type)") : type;
  }
  static Value orv_or_empty(const Value& v) { return v.truthy() ? v : Value(""); }
  static Value nn_or(const Value& v, double d) { return nullish(v) ? Value(d) : v; }
};

/// entmode: 2 model space, 1 the active paper space, 0 a block
double space_mode(const std::string& kind) {
  if (kind == "model") {
    return 2;
  }
  return kind == "paper" ? 1 : 0;
}

}  // namespace

}  // namespace w

// ---------------------------------------------------------------- writeDwg

std::string dwg_version_code(std::string_view v) {
  static const std::array<std::pair<std::string_view, std::string_view>, 18> a = {{
      {"AC1015", "AC1015"},
      {"AC1018", "AC1018"},
      {"AC1021", "AC1021"},
      {"AC1024", "AC1024"},
      {"AC1027", "AC1027"},
      {"AC1032", "AC1032"},
      {"R2000", "AC1015"},
      {"R2004", "AC1018"},
      {"R2007", "AC1021"},
      {"R2010", "AC1024"},
      {"R2013", "AC1027"},
      {"R2018", "AC1032"},
      {"2000", "AC1015"},
      {"2004", "AC1018"},
      {"2007", "AC1021"},
      {"2010", "AC1024"},
      {"2013", "AC1027"},
      {"2018", "AC1032"},
  }};
  for (const auto& [k, c] : a) {
    if (k == v) {
      return std::string(c);
    }
  }
  return {};
}

// NOLINTNEXTLINE(readability-function-size): writeDwg in one piece, as the JS reads (each stage marked)
WriteResult write_dwg(const base::Value& model, const WriteOptions& opts) {
  using namespace w;  // NOLINT(google-build-using-namespace): the port reads as the JS
  using base::make_object;
  std::string version = dwg_version_code(opts.version);
  if (version.empty()) {
    if (!opts.version.empty()) {
      version = opts.version;
    } else {
      version = ref(model, "version").is_string() ? ref(model, "version").as_string() : "AC1015";
    }
    if (version.empty()) {
      version = "AC1015";
    }
  }
  if (dwg_version_code(version) != version || version.size() != 6) {
    throw std::invalid_argument("writeDwg: version " + version +
                                " is not supported (use one of AC1015, AC1018, AC1021, AC1024, AC1027, AC1032)");
  }
  Writer W(model);
  W.version = version;
  W.r2004 = at_least(version, "AC1018");
  W.r2007 = at_least(version, "AC1021");
  W.random = opts.random;
  W.report = make_object(
      {{"version", version},
       {"notes", Array{}},
       {"counts", Value::object()},
       {"skipped", Value::object()},
       {"layers", Array{}},
       {"handles", Value::object()},
       {"renamed",
        make_object({{"layers", Value::object()}, {"blocks", Value::object()}, {"layouts", Value::object()}})},
       {"objects", 0},
       {"entities", 0}});
  text_loss().lost = 0;
  const double created = opts.created_ms ? *opts.created_ms
                                         : static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                   std::chrono::system_clock::now().time_since_epoch())
                                                                   .count());
  const Value hdr_in = ref(model, "header").is_object() ? ref(model, "header") : Value::object();

  // ---------------------------------------------------------------- handles
  for (const char* k :
       {"blockCtl",      "layerCtl",        "styleCtl",  "ltypeCtl",  "viewCtl",      "ucsCtl",  "vportCtl",
        "appidCtl",      "dimstyleCtl",     "vpentCtl",  "nod",       "groupDict",    "mlsDict", "layoutDict",
        "plotStyleDict", "plotStyleNormal", "ltByBlock", "ltByLayer", "ltContinuous", "layer0",  "mlstyle",
        "vport"}) {
    W.H[k] = W.next();
  }

  // ---------------------------------------------------------------- linetypes
  const Value& T = ref(model, "tables");
  W.reserve("ltype", "ByBlock");
  W.reserve("ltype", "ByLayer");
  W.reserve("ltype", "Continuous");
  W.lt_by_name["BYBLOCK"] = W.H["ltByBlock"];
  W.lt_by_name["BYLAYER"] = W.H["ltByLayer"];
  W.lt_by_name["CONTINUOUS"] = W.H["ltContinuous"];
  for (const Value& lt : arr(T, "linetypes")) {
    const std::string up = base::js_to_upper(str_of(ref(lt, "name")));
    if (const auto it = W.lt_by_name.find(up); it != W.lt_by_name.end()) {
      W.map_src(ref(lt, "handle"), it->second);
      continue;
    }
    const std::string name = W.clean_name("ltype", ref(lt, "name"), "Linetype");
    const double h = W.next();
    W.map_src(ref(lt, "handle"), h);
    W.lt_by_name[up] = h;
    W.lt_by_name[base::js_to_upper(name)] = h;
    Array els;
    for (const Value& e : arr(lt, "elements")) {
      els.push_back(make_object({{"length", numk(e, "length")},
                                 {"shape", ivk(e, "shape")},
                                 {"offset", ref(e, "offset")},
                                 {"scale", ref(e, "scale")},
                                 {"rotation", ref(e, "rotation")},
                                 {"flags", 0}}));
    }
    const Value& d = ref(lt, "description");
    W.ltypes.push_back(make_rec(make_object({{"t", "LTYPE"},
                                             {"h", h},
                                             {"owner", W.H["ltypeCtl"]},
                                             {"name", name},
                                             {"description", d.truthy() ? d : Value("")},
                                             {"patternLength", ref(lt, "patternLength")},
                                             {"elements", std::move(els)}})));
  }

  // ---------------------------------------------------------------- text styles
  const auto add_style = [&](const Value& s) {
    const Value& sn = ref(s, "name");
    const std::string up = base::js_to_upper(sn.truthy() ? str_of(sn) : std::string("Standard"));
    if (const auto it = W.style_by_name.find(up); it != W.style_by_name.end()) {
      W.map_src(ref(s, "handle"), it->second);
      return;
    }
    std::string name;
    if (up == "STANDARD") {
      name = "Standard";
      W.reserve("style", "Standard");
    } else {
      name = W.clean_name("style", sn, "Style");
    }
    const double h = W.next();
    W.map_src(ref(s, "handle"), h);
    W.style_by_name[up] = h;
    W.style_by_name[base::js_to_upper(name)] = h;
    const int flags = base::js_to_int32(base::js_to_number(ref(s, "flags")));
    const Value& font = ref(s, "font");
    const Value& big = ref(s, "bigFont");
    W.styles.push_back(make_rec(make_object({{"t", "STYLE"},
                                             {"h", h},
                                             {"owner", W.H["styleCtl"]},
                                             {"name", name},
                                             {"height", numk(s, "height")},
                                             {"widthFactor", numk(s, "widthFactor", 1)},
                                             {"oblique", numk(s, "oblique")},
                                             {"generation", ivk(s, "generation")},
                                             {"lastHeight", numk(s, "lastHeight", 0.2)},
                                             {"font", nullish(font) ? Value("txt") : font},
                                             {"bigFont", nullish(big) ? Value("") : big},
                                             {"vertical", (flags & 4) != 0},
                                             {"shapeFile", (flags & 1) != 0}})));
  };
  {
    const Value* st = nullptr;
    for (const Value& s : arr(T, "styles")) {
      if (ieq(ref(s, "name"), "standard")) {
        st = &s;
        break;
      }
    }
    add_style(st != nullptr ? *st : make_object({{"name", "Standard"}}));
    for (const Value& s : arr(T, "styles")) {
      if (tr(s, "name")) {
        add_style(s);
      }
    }
  }

  // ---------------------------------------------------------------- layers
  for (const Value& b : arr(model, "blocks")) {
    if ((base::js_to_int32(base::js_to_number(ref(b, "flags"))) & 4) != 0 && tr(b, "name")) {
      W.xref_block_names.insert(upper(ref(b, "name")));
    }
  }
  {
    const Value* l0 = nullptr;
    for (const Value& l : arr(T, "layers")) {
      if (ref(l, "name").is_string() && ref(l, "name").as_string() == "0") {
        l0 = &l;
        break;
      }
    }
    (void)W.add_layer(l0 != nullptr ? *l0 : make_object({{"name", "0"}, {"color", make_object({{"index", 7}})}}));
    for (const Value& l : arr(T, "layers")) {
      (void)W.add_layer(l);
    }
  }

  // ---------------------------------------------------------------- registered applications
  (void)W.add_app(Value("ACAD"));
  for (const Value& a : arr(T, "appids")) {
    if (tr(a, "name")) {
      (void)W.add_app(ref(a, "name"), ref(a, "handle"));
    }
  }

  // ---------------------------------------------------------------- dimension styles
  const auto add_dim = [&](const Value& d) {
    const Value& dn = ref(d, "name");
    const std::string up = base::js_to_upper(dn.truthy() ? str_of(dn) : std::string("Standard"));
    if (const auto it = W.dim_by_name.find(up); it != W.dim_by_name.end()) {
      W.map_src(ref(d, "handle"), it->second);
      return;
    }
    const std::string name = up == "STANDARD" ? std::string("Standard") : W.clean_name("dimstyle", dn, "Dimstyle");
    const double h = W.next();
    W.map_src(ref(d, "handle"), h);
    W.dim_by_name[up] = h;
    W.dim_by_name[base::js_to_upper(name)] = h;
    Value vars = ref(d, "vars").is_object() ? ref(d, "vars") : Value::object();
    const double txsty = W.style_ref(ref(vars, "DIMTXSTY"));
    W.dimstyles.push_back(make_rec(make_object({{"t", "DIMSTYLE"},
                                                {"h", h},
                                                {"owner", W.H["dimstyleCtl"]},
                                                {"name", name},
                                                {"vars", std::move(vars)},
                                                {"txsty", txsty}})));
  };
  {
    const Value* st = nullptr;
    for (const Value& d : arr(T, "dimstyles")) {
      if (ieq(ref(d, "name"), "standard")) {
        st = &d;
        break;
      }
    }
    add_dim(st != nullptr ? *st : make_object({{"name", "Standard"}}));
    for (const Value& d : arr(T, "dimstyles")) {
      if (tr(d, "name")) {
        add_dim(d);
      }
    }
  }

  // ---------------------------------------------------------------- named views and UCSs
  for (const Value& v : arr(T, "views")) {
    if (!v.truthy() || !tr(v, "name")) {
      continue;
    }
    const double h = W.next();
    W.map_src(ref(v, "handle"), h);
    Value rec = v;
    rec.set("t", "VIEW");
    rec.set("h", h);
    rec.set("owner", W.H["viewCtl"]);
    rec.set("name", W.clean_name("view", ref(v, "name"), "View"));
    rec.set("reactors", Value());
    rec.set("xdic", Value());
    rec.set("eed", Value());
    W.views.push_back(make_rec(std::move(rec)));
  }
  for (const Value& u : arr(T, "ucs")) {
    const auto p3 = [](const Value& p) -> Value {
      return p.truthy() && fin2(p) ? base::pt3(numk(p, "x"), numk(p, "y"), numk(p, "z")) : Value(nullptr);
    };
    const Value o = p3(ref(u, "origin"));
    const Value xa = p3(ref(u, "xAxis"));
    const Value ya = p3(ref(u, "yAxis"));
    if (!u.truthy() || !tr(u, "name") || o.is_null() || xa.is_null() || ya.is_null()) {
      continue;
    }
    const double h = W.next();
    W.map_src(ref(u, "handle"), h);
    W.ucs_list.push_back(make_rec(make_object({{"t", "UCS"},
                                               {"h", h},
                                               {"owner", W.H["ucsCtl"]},
                                               {"name", W.clean_name("ucs", ref(u, "name"), "UCS")},
                                               {"origin", o},
                                               {"xAxis", xa},
                                               {"yAxis", ya},
                                               {"elevation", numk(u, "elevation")}})));
  }
  // the *Active viewports
  struct Active {
    const Value* v;
    double h;
  };
  std::vector<Active> actives;
  {
    const auto corner = [](const Value& p) {
      if (!p.truthy() || !fin2(p)) {
        return false;
      }
      const double x = numk(p, "x");
      const double y = numk(p, "y");
      return x >= 0 && x <= 1 && y >= 0 && y <= 1;
    };
    std::vector<const Value*> act_in;
    for (const Value& v : arr(T, "vports")) {
      if (v.truthy() && ieq(orv_value(ref(v, "name")), "*active") && corner(ref(v, "lowerLeft")) &&
          corner(ref(v, "upperRight")) && numk(ref(v, "upperRight"), "x") > numk(ref(v, "lowerLeft"), "x") &&
          numk(ref(v, "upperRight"), "y") > numk(ref(v, "lowerLeft"), "y")) {
        act_in.push_back(&v);
      }
    }
    if (act_in.size() >= 2 && act_in.size() <= 4) {
      for (std::size_t i = 0; i < act_in.size(); ++i) {
        actives.push_back({act_in[i], i != 0 ? W.next() : W.H["vport"]});
      }
    } else if (act_in.size() == 1 && numk(*act_in[0], "twist") != 0) {
      actives.push_back({act_in[0], W.H["vport"]});
    } else {
      actives.push_back({nullptr, W.H["vport"]});
    }
  }

  // ---------------------------------------------------------------- blocks
  std::vector<const Value*> blocks_in;
  std::deque<Value> synth;  // blocks the writer adds
  for (const Value& b : arr(model, "blocks")) {
    blocks_in.push_back(&b);
  }
  // /^\*model_space$/i, /^\*paper_space\d*$/i, /^\*paper_space$/i (ASCII letters: the i flag folds them only)
  const auto ascii_lower = [](std::string s) {
    for (char& c : s) {
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c - 'A' + 'a');
      }
    }
    return s;
  };
  const auto ms_name = [&](const std::string& n) { return ascii_lower(n) == "*model_space"; };
  const auto ps_name = [&](const std::string& n) {
    const std::string l = ascii_lower(n);
    if (!l.starts_with("*paper_space")) {
      return false;
    }
    return std::ranges::all_of(l.substr(12), [](char c) { return c >= '0' && c <= '9'; });
  };
  const auto ps0_name = [&](const std::string& n) { return ascii_lower(n) == "*paper_space"; };
  const auto name_of = [](const Value* b) { return str_of(orv_value(ref(*b, "name"))); };
  const auto is_ms = [&](const Value* b) { return ms_name(name_of(b)); };
  const auto is_ps = [&](const Value* b) { return ps_name(name_of(b)); };
  if (std::ranges::none_of(blocks_in, is_ms)) {
    blocks_in.insert(blocks_in.begin(),
                     &synth.emplace_back(make_object({{"name", "*Model_Space"}, {"entities", Array{}}})));
  }
  if (std::ranges::none_of(blocks_in, is_ps)) {
    blocks_in.insert(blocks_in.begin() + 1,
                     &synth.emplace_back(make_object({{"name", "*Paper_Space"}, {"entities", Array{}}})));
  }
  const Value* ms_b = *std::ranges::find_if(blocks_in, is_ms);
  std::vector<const Value*> ps_bs;
  std::vector<const Value*> others;
  for (const Value* b : blocks_in) {
    if (is_ps(b)) {
      ps_bs.push_back(b);
    } else if (!is_ms(b)) {
      others.push_back(b);
    }
  }
  std::ranges::stable_sort(ps_bs, [&](const Value* a, const Value* b) {
    const bool pa = ps0_name(name_of(a));
    const bool pb = ps0_name(name_of(b));
    return pa && !pb;
  });
  std::unordered_map<std::string, const Value*> block_recs_by_record;
  for (const Value& r : arr(T, "blockRecords")) {
    const Key k = hex_key(ref(r, "handle"));
    block_recs_by_record[k ? *k : std::string("\x01null")] = &r;
  }
  W.MS = W.add_block(ms_b, Value(), "model", "*Model_Space");
  for (std::size_t i = 0; i < ps_bs.size(); ++i) {
    W.PSs.push_back(W.add_block(ps_bs[i], Value(), i != 0 ? "paper2" : "paper",
                                i != 0 ? "*Paper_Space" + std::to_string(i - 1) : "*Paper_Space"));
  }
  for (const Value* b : others) {
    if (!W.block_by_name.contains(upper(ref(*b, "name")))) {
      (void)W.add_block(b, Value(), "block", "");
    }
  }
  for (BlockW& B : W.blocks) {
    const Key k = hex_key(ref(B.s(), "record"));
    if (const auto it = block_recs_by_record.find(k ? *k : std::string("\x01null")); it != block_recs_by_record.end()) {
      B.rec_src = it->second;
    }
  }

  // layouts
  struct LayoutW {
    const Value* src;
    Value own;
    std::string name;
    double tab;
    BlockW* B;
    double h = 0;
    [[nodiscard]] const Value& s() const { return src != nullptr ? *src : own; }
  };
  std::vector<LayoutW> layouts;
  {
    const Array& layouts_in = arr(model, "layouts");
    const Value* model_l = nullptr;
    for (const Value& l : layouts_in) {
      if (ieq(ref(l, "name"), "model")) {
        model_l = &l;
        break;
      }
    }
    if (model_l != nullptr) {
      layouts.push_back({model_l, Value(), "Model", 0, W.MS});
    } else {
      layouts.push_back({nullptr, make_object({{"name", "Model"}}), "Model", 0, W.MS});
    }
    std::vector<const Value*> paper_ls;
    for (const Value& l : layouts_in) {
      if (!ieq(ref(l, "name"), "model")) {
        paper_ls.push_back(&l);
      }
    }
    std::ranges::stable_sort(
        paper_ls, [](const Value* a, const Value* b) { return numk(*a, "tabOrder", 1) - numk(*b, "tabOrder", 1) < 0; });
    std::set<BlockW*> used_b;
    for (std::size_t i = 0; i < paper_ls.size(); ++i) {
      const Value& l = *paper_ls[i];
      BlockW* B = nullptr;
      if (const Key k = hex_key(ref(l, "block")); k) {
        for (BlockW* x : W.PSs) {
          if (hex_key(ref(x->s(), "record")) == k) {
            B = x;
            break;
          }
        }
      }
      if (B == nullptr || used_b.contains(B)) {
        B = nullptr;
        for (BlockW* x : W.PSs) {
          if (!used_b.contains(x)) {
            B = x;
            break;
          }
        }
      }
      if (B == nullptr) {
        const std::string nm = "*Paper_Space" + std::to_string(W.PSs.size() - 1);
        B = W.add_block(nullptr, make_object({{"name", nm}, {"entities", Array{}}}), "paper2", nm);
        W.PSs.push_back(B);
      }
      used_b.insert(B);
      const std::string lname = W.clean_name("layout", ref(l, "name"), "Layout" + std::to_string(i + 1));
      if (!(ref(l, "name").is_string() && lname == ref(l, "name").as_string())) {
        W.report.find("renamed")->find("layouts")->set(str_of(ref(l, "name")), lname);
      }
      layouts.push_back({&l, Value(), lname, static_cast<double>(i + 1), B});
    }
    if (layouts.size() == 1) {
      layouts.push_back({nullptr, make_object({{"name", "Layout1"}}), "Layout1", 1, W.PSs[0]});
    }
    BlockW const* first = layouts[1].B;
    if (first != W.PSs[0]) {
      const auto it = std::ranges::find(W.PSs, first);
      std::iter_swap(W.PSs.begin(), it);
    }
    for (std::size_t i = 0; i < W.PSs.size(); ++i) {
      W.PSs[i]->kind = i != 0 ? "paper2" : "paper";
      W.PSs[i]->name = i != 0 ? "*Paper_Space" + std::to_string(i - 1) : "*Paper_Space";
    }
    for (LayoutW& L : layouts) {
      L.h = W.next();
    }
    for (BlockW* B : W.PSs) {
      if (std::ranges::none_of(layouts, [&](const LayoutW& L) { return L.B == B; })) {
        const std::string nm = "Layout" + std::to_string(layouts.size());
        LayoutW L{nullptr, make_object({{"name", nm}}), W.clean_name("layout", Value(nm), "Layout"),
                  static_cast<double>(layouts.size()), B};
        L.h = W.next();
        layouts.push_back(std::move(L));
      }
    }
  }

  // ---------------------------------------------------------------- entities → records
  Value skipped = Value::object();
  Conv conv{W, {}, {}, {}, {}, false};
  for (const Value& o : arr(model, "objects")) {
    if (is_str(ref(o, "type"), "IMAGEDEF")) {
      conv.image_defs[null_key(hex_key(ref(o, "handle")))] = &o;
    }
  }
  W.X = std::make_unique<CopyWriter>(W);
  W.TBL = std::make_unique<TableWriter>(W);
  W.SOL = std::make_unique<SolidWriter>(W);

  double invalid = 0;
  for (std::size_t bi = 0; bi < W.blocks.size(); ++bi) {
    BlockW& B = W.blocks[bi];
    B.hdr = W.next();
    B.block = W.next();
    W.map_src(ref(B.s(), "record"), B.hdr);
    W.map_src(ref(B.s(), "handle"), B.block);
    const double mode = space_mode(B.kind);
    for (const Value& e : arr(B.s(), "entities")) {
      w::Out out = conv.convert(e, B.hdr);
      if (std::holds_alternative<std::monostate>(out)) {
        invalid += 1;
        continue;
      }
      if (std::holds_alternative<std::string>(out)) {
        const std::string& t = std::get<std::string>(out);
        skipped.set(t, num(ref(skipped, t)) + 1);
        continue;
      }
      auto& list = std::get<std::vector<RecPtr>>(out);
      list[0]->v.set("mode", mode);
      if (mode == 0) {
        list[0]->v.set("owner", B.hdr);
      }
      W.map_src(ref(e, "handle"), ref(list[0]->v, "h").as_number());
      const std::string t = ref(list[0]->v, "t").as_string();
      Value* counts = W.report.find("counts");
      counts->set(t, num(ref(*counts, t)) + 1);
      B.ents.push_back(std::move(list));
    }
    B.endblk = W.next();
    W.map_src(ref(B.s(), "endHandle"), B.endblk);
  }
  const auto note = [&](std::string s) { W.report.find("notes")->push(Value(std::move(s))); };
  const auto nfmt = [](double n) { return base::js_string(Value(n)); };
  if (invalid != 0) {
    note(nfmt(invalid) + " item" + (invalid == 1 ? " was" : "s were") + " left out (empty or not drawable).");
  }
  for (const base::Member* m : base::js_members(skipped.as_object())) {
    const double n = m->value.as_number();
    if (m->key.contains(' ')) {
      note(nfmt(n) + " × " + m->key + ": not written.");
    } else {
      note(nfmt(n) + " " + m->key + " " + (n == 1 ? "entity was" : "entities were") +
           " not written (not supported by the writer yet).");
    }
  }
  W.report.set("skipped", skipped);
  if (W.notes.renamed_layers != 0) {
    const double n = W.notes.renamed_layers;
    note(nfmt(n) + " layer name" + (n == 1 ? " was" : "s were") +
         " changed to fit DWG naming rules (a linked drawing's \"Xref|Layer\" becomes \"Xref$0$Layer\", as when "
         "AutoCAD binds it).");
  }
  if (W.notes.renamed_blocks != 0) {
    const double n = W.notes.renamed_blocks;
    note(nfmt(n) + " block name" + (n == 1 ? " was" : "s were") + " changed to fit DWG naming rules.");
  }

  // ---------------------------------------------------------------- extents (model space)
  Value ext;
  {
    double min_x = std::numeric_limits<double>::infinity();
    double min_y = std::numeric_limits<double>::infinity();
    double min_z = std::numeric_limits<double>::infinity();
    double max_x = -std::numeric_limits<double>::infinity();
    double max_y = -std::numeric_limits<double>::infinity();
    double max_z = -std::numeric_limits<double>::infinity();
    const auto g = [&](const Value& p, double r = 0) {
      if (!p.truthy() || !fin2(p)) {
        return;
      }
      const double x = numk(p, "x");
      const double y = numk(p, "y");
      const double z = numk(p, "z");
      min_x = std::fmin(min_x, x - r);
      max_x = std::fmax(max_x, x + r);
      min_y = std::fmin(min_y, y - r);
      max_y = std::fmax(max_y, y + r);
      min_z = std::fmin(min_z, z);
      max_z = std::fmax(max_z, z);
    };
    const Value& he = ref(hdr_in, "EXTMIN");
    const Value& hx = ref(hdr_in, "EXTMAX");
    if (he.truthy() && hx.truthy() && fin2(he) && fin2(hx) && numk(he, "x") <= numk(hx, "x") &&
        numk(he, "y") <= numk(hx, "y") && std::abs(numk(he, "x")) < 1e19) {
      g(he);
      g(hx);
    } else {
      for (const auto& list : W.MS->ents) {
        const Value& r = list[0]->v;
        const std::string& t = ref(r, "t").as_string();
        if (t == "LINE") {
          g(ref(r, "start"));
          g(ref(r, "end"));
        } else if (t == "CIRCLE" || t == "ARC") {
          g(ref(r, "center"), base::js_to_number(ref(r, "radius")));
        } else if (t == "ELLIPSE") {
          const Value& m = ref(r, "majorAxis");
          g(ref(r, "center"), std::hypot(numk(m, "x"), numk(m, "y")));
        } else if (t == "LWPOLYLINE") {
          for (const Value& p : arr(r, "vertices")) {
            g(make_object({{"x", get(p, "x")}, {"y", get(p, "y")}, {"z", ref(r, "elevation")}}));
          }
        } else if (t == "POLYLINE_2D" || t == "POLYLINE_3D" || t == "POLYLINE_PFACE" || t == "POLYLINE_MESH") {
          for (std::size_t i = 1; i < list.size(); ++i) {
            if (ref(list[i]->v, "point").truthy()) {
              g(ref(list[i]->v, "point"));
            }
          }
        } else if (t == "POINT") {
          g(ref(r, "position"));
        } else if (t == "TEXT" || t == "MTEXT" || t == "INSERT" || t == "MINSERT") {
          g(ref(r, "insert"));
        } else if (t == "SOLID" || t == "TRACE" || t == "3DFACE") {
          for (const Value& p : arr(r, "points")) {
            g(p);
          }
        } else if (t == "HATCH") {
          for (const Value& l : arr(r, "loops")) {
            if (tr(l, "polyline")) {
              for (const Value& p : arr(ref(l, "polyline"), "vertices")) {
                g(p);
              }
            }
          }
        } else if (t == "SPLINE") {
          for (const Value& p : !arr(r, "control").empty() ? arr(r, "control") : arr(r, "fit")) {
            g(p);
          }
        } else if (ref(r, "defPoint").truthy()) {
          g(ref(r, "defPoint"));
        }
      }
    }
    if (std::isfinite(min_x)) {
      ext = make_object({{"min", make_object({{"x", min_x}, {"y", min_y}, {"z", min_z}})},
                         {"max", make_object({{"x", max_x}, {"y", max_y}, {"z", max_z}})}});
    }
  }

  // ---------------------------------------------------------------- records for the fixed objects
  std::vector<RecPtr> recs;
  const auto push = [&](RecPtr r) { recs.push_back(std::move(r)); };
  const auto pushv = [&](Value v) { recs.push_back(make_rec(std::move(v))); };
  for (BlockW& B : W.blocks) {
    W.block_by_name[base::js_to_upper(B.name)] = &B;
  }
  const auto hs = [](const std::vector<RecPtr>& v) {
    Array a;
    for (const RecPtr& r : v) {
      a.push_back(ref(r->v, "h"));
    }
    return a;
  };
  {
    Array ub;
    for (BlockW& B : W.blocks) {
      if (B.kind == "block" || B.kind == "paper2") {
        ub.emplace_back(B.hdr);
      }
    }
    pushv(make_object({{"t", "BLOCK_CONTROL"},
                       {"h", W.H["blockCtl"]},
                       {"owner", 0},
                       {"entries", std::move(ub)},
                       {"hardEntries", Array{W.MS->hdr, W.PSs[0]->hdr}}}));
  }
  pushv(make_object({{"t", "LAYER_CONTROL"}, {"h", W.H["layerCtl"]}, {"owner", 0}, {"entries", hs(W.layers)}}));
  pushv(make_object({{"t", "STYLE_CONTROL"}, {"h", W.H["styleCtl"]}, {"owner", 0}, {"entries", hs(W.styles)}}));
  {
    Array e{W.H["ltContinuous"]};
    for (const RecPtr& l : W.ltypes) {
      e.push_back(ref(l->v, "h"));
    }
    pushv(make_object({{"t", "LTYPE_CONTROL"},
                       {"h", W.H["ltypeCtl"]},
                       {"owner", 0},
                       {"count", 1 + static_cast<double>(W.ltypes.size())},
                       {"entries", std::move(e)},
                       {"hardEntries", Array{W.H["ltByLayer"], W.H["ltByBlock"]}}}));
  }
  pushv(make_object({{"t", "VIEW_CONTROL"}, {"h", W.H["viewCtl"]}, {"owner", 0}, {"entries", hs(W.views)}}));
  pushv(make_object({{"t", "UCS_CONTROL"}, {"h", W.H["ucsCtl"]}, {"owner", 0}, {"entries", hs(W.ucs_list)}}));
  {
    Array e;
    for (const Active& a : actives) {
      e.emplace_back(a.h);
    }
    pushv(make_object({{"t", "VPORT_CONTROL"}, {"h", W.H["vportCtl"]}, {"owner", 0}, {"entries", std::move(e)}}));
  }
  pushv(make_object({{"t", "APPID_CONTROL"}, {"h", W.H["appidCtl"]}, {"owner", 0}, {"entries", hs(W.appids)}}));
  pushv(
      make_object({{"t", "DIMSTYLE_CONTROL"}, {"h", W.H["dimstyleCtl"]}, {"owner", 0}, {"entries", hs(W.dimstyles)}}));
  if (!W.r2004) {
    pushv(make_object({{"t", "VPENT_CONTROL"}, {"h", W.H["vpentCtl"]}, {"owner", 0}, {"entries", Array{}}}));
  }
  const auto by_name = [](Array& items) {
    std::ranges::stable_sort(items, [](const Value& a, const Value& b) {
      return js_less(a.as_array()[0].as_string(), b.as_array()[0].as_string());
    });
  };
  Array layout_items;
  for (const LayoutW& L : layouts) {
    layout_items.emplace_back(Array{L.name, L.h});
  }
  by_name(layout_items);
  Array nod_items{Array{"ACAD_GROUP", W.H["groupDict"]}, Array{"ACAD_LAYOUT", W.H["layoutDict"]},
                  Array{"ACAD_MLINESTYLE", W.H["mlsDict"]}, Array{"ACAD_PLOTSTYLENAME", W.H["plotStyleDict"]}};
  std::vector<RecPtr> extra_objects;
  if (!conv.written_defs_order.empty()) {
    const double dict_h = W.next();
    const double vars_h = W.next();
    nod_items.emplace_back(Array{"ACAD_IMAGE_DICT", dict_h});
    nod_items.emplace_back(Array{"ACAD_IMAGE_VARS", vars_h});
    Array items;
    for (const RecPtr& d : conv.written_defs_order) {
      items.emplace_back(Array{ref(d->v, "name"), ref(d->v, "h")});
    }
    extra_objects.push_back(make_rec(make_object({{"t", "DICTIONARY"},
                                                  {"h", dict_h},
                                                  {"owner", W.H["nod"]},
                                                  {"reactors", Array{W.H["nod"]}},
                                                  {"items", std::move(items)},
                                                  {"hardOwner", true}})));
    extra_objects.push_back(make_rec(make_object(
        {{"t", "RASTERVARIABLES"}, {"h", vars_h}, {"owner", W.H["nod"]}, {"reactors", Array{W.H["nod"]}}})));
    for (const RecPtr& d : conv.written_defs_order) {
      d->v.set("owner", dict_h);
      d->v.find("reactors")->push(dict_h);
      extra_objects.push_back(d);
    }
  }
  if (conv.need_wipeout_vars) {
    const double h = W.next();
    nod_items.emplace_back(Array{"ACAD_WIPEOUT_VARS", h});
    extra_objects.push_back(make_rec(
        make_object({{"t", "WIPEOUTVARIABLES"}, {"h", h}, {"owner", W.H["nod"]}, {"reactors", Array{W.H["nod"]}}})));
  }
  by_name(nod_items);
  pushv(make_object({{"t", "DICTIONARY"}, {"h", W.H["nod"]}, {"owner", 0}, {"items", std::move(nod_items)}}));
  for (RecPtr const& o : extra_objects) {
    push(o);
  }
  const Array nod_reactor{W.H["nod"]};
  pushv(make_object({{"t", "DICTIONARY"},
                     {"h", W.H["groupDict"]},
                     {"owner", W.H["nod"]},
                     {"reactors", nod_reactor},
                     {"items", Array{}}}));
  pushv(make_object({{"t", "DICTIONARY"},
                     {"h", W.H["layoutDict"]},
                     {"owner", W.H["nod"]},
                     {"reactors", nod_reactor},
                     {"items", std::move(layout_items)}}));
  pushv(make_object({{"t", "DICTIONARY"},
                     {"h", W.H["mlsDict"]},
                     {"owner", W.H["nod"]},
                     {"reactors", nod_reactor},
                     {"items", base::array_of(Array{"Standard", W.H["mlstyle"]})}}));
  pushv(make_object({{"t", "DICTIONARYWDFLT"},
                     {"h", W.H["plotStyleDict"]},
                     {"owner", W.H["nod"]},
                     {"reactors", nod_reactor},
                     {"items", base::array_of(Array{"Normal", W.H["plotStyleNormal"]})},
                     {"defaultEntry", W.H["plotStyleNormal"]}}));
  pushv(make_object({{"t", "PLACEHOLDER"},
                     {"h", W.H["plotStyleNormal"]},
                     {"owner", W.H["plotStyleDict"]},
                     {"reactors", Array{W.H["plotStyleDict"]}}}));
  pushv(make_object({{"t", "MLINESTYLE"},
                     {"h", W.H["mlstyle"]},
                     {"owner", W.H["mlsDict"]},
                     {"reactors", Array{W.H["mlsDict"]}},
                     {"name", "Standard"},
                     {"ltByLayer", W.H["ltByLayer"]}}));
  pushv(make_object({{"t", "LTYPE"},
                     {"h", W.H["ltByBlock"]},
                     {"owner", W.H["ltypeCtl"]},
                     {"name", "ByBlock"},
                     {"elements", Array{}}}));
  pushv(make_object({{"t", "LTYPE"},
                     {"h", W.H["ltByLayer"]},
                     {"owner", W.H["ltypeCtl"]},
                     {"name", "ByLayer"},
                     {"elements", Array{}}}));
  pushv(make_object({{"t", "LTYPE"},
                     {"h", W.H["ltContinuous"]},
                     {"owner", W.H["ltypeCtl"]},
                     {"name", "Continuous"},
                     {"description", "Solid line"},
                     {"elements", Array{}}}));
  for (const RecPtr& l : W.ltypes) {
    push(l);
  }
  for (const RecPtr& L : W.layers) {
    if (tr(L->v, "xdep")) {
      const std::string k = upper(ref(L->v, "xrefOf"));
      const auto it = W.block_by_name.find(k);
      L->v.set("xrefBlock", it != W.block_by_name.end() ? it->second->hdr : 0.0);
    }
    push(L);
  }
  for (const RecPtr& s : W.styles) {
    push(s);
  }
  for (const RecPtr& a : std::vector<RecPtr>(W.appids)) {
    push(a);
  }
  for (const RecPtr& d : W.dimstyles) {
    push(d);
  }
  for (const RecPtr& v : W.views) {
    push(v);
  }
  for (const RecPtr& u : W.ucs_list) {
    push(u);
  }
  // the *Active viewport
  {
    const double emin_x = ext.truthy() ? path(ext, {"min", "x"}).as_number() : 0;
    const double emin_y = ext.truthy() ? path(ext, {"min", "y"}).as_number() : 0;
    const double emax_x = ext.truthy() ? path(ext, {"max", "x"}).as_number() : 12;
    const double emax_y = ext.truthy() ? path(ext, {"max", "y"}).as_number() : 9;
    constexpr double kAspect = 1.5;
    double vh = std::fmax(std::fmax(emax_y - emin_y, (emax_x - emin_x) / kAspect), 1e-6) * 1.05;
    if (vh == 0 || vh != vh) {
      vh = 1;
    }
    const Value c0 = make_object({{"x", (emin_x + emax_x) / 2}, {"y", (emin_y + emax_y) / 2}});
    for (const Active& a : actives) {
      if (a.v == nullptr) {
        pushv(make_object({{"t", "VPORT"},
                           {"h", a.h},
                           {"owner", W.H["vportCtl"]},
                           {"name", "*Active"},
                           {"viewHeight", vh},
                           {"aspect", kAspect},
                           {"center", c0}}));
        continue;
      }
      const Value& v = *a.v;
      const Value c = ref(v, "center").truthy() ? ref(v, "center") : ref(v, "viewCenter");
      const Value& hgt = ref(v, "viewHeight");
      const Value& asp = nullish(ref(v, "aspect")) ? ref(v, "aspectRatio") : ref(v, "aspect");
      Value center = c.truthy() && fin2(c) ? make_object({{"x", get(c, "x")}, {"y", get(c, "y")}}) : c0;
      pushv(make_object({{"t", "VPORT"},
                         {"h", a.h},
                         {"owner", W.H["vportCtl"]},
                         {"name", "*Active"},
                         {"viewHeight", num(hgt) > 0 ? hgt : Value(vh)},
                         {"aspect", num(asp) > 0 ? asp : Value(kAspect)},
                         {"center", std::move(center)},
                         {"lowerLeft", ref(v, "lowerLeft")},
                         {"upperRight", ref(v, "upperRight")},
                         {"twist", numk(v, "twist")},
                         {"eed", W.eed_of(ref(v, "xdata"))}}));
    }
  }
  // layouts
  for (const LayoutW& L : layouts) {
    const Value& s = L.s();
    pushv(make_object({{"t", "LAYOUT"},
                       {"h", L.h},
                       {"owner", W.H["layoutDict"]},
                       {"reactors", Array{W.H["layoutDict"]}},
                       {"name", L.name},
                       {"tab", L.tab},
                       {"block", L.B->hdr},
                       {"paper", ref(s, "paper")},
                       {"plotFlags", ref(s, "plotFlags")},
                       {"limits", ref(s, "limits")},
                       {"insbase", ref(s, "insbase")},
                       {"extents", L.tab == 0 ? or_value(ext, Value(nullptr)) : Value(nullptr)},
                       {"viewports", Array{}},
                       {"lastViewport", L.tab == 0 ? W.H["vport"] : 0}}));
    L.B->layout = L.h;
  }
  // blocks and their entities
  for (BlockW& B : W.blocks) {
    const std::string& kind = B.kind;
    const Value& src = B.rec_src != nullptr ? *B.rec_src : undef();
    const bool has_attrs =
        std::ranges::any_of(B.ents, [](const auto& l) { return is_str(ref(l[0]->v, "t"), "ATTDEF"); });
    const int bflags = base::js_to_int32(base::js_to_number(ref(B.s(), "flags")));
    const bool xref = kind == "block" && (bflags & 4) != 0;
    Array owned;
    for (const auto& l : B.ents) {
      owned.push_back(ref(l[0]->v, "h"));
    }
    Array ins;
    if (kind == "block") {
      for (const double h : B.inserts) {
        ins.emplace_back(h);
      }
    }
    const Value& d = ref(B.s(), "description");
    pushv(make_object({{"t", "BLOCK_HEADER"},
                       {"h", B.hdr},
                       {"owner", W.H["blockCtl"]},
                       {"name", B.name},
                       {"anonymous", B.name.starts_with("*") && kind == "block"},
                       {"hasAttrs", has_attrs},
                       {"isXref", xref},
                       {"overlaid", xref && (bflags & 8) != 0},
                       {"base", ref(B.s(), "base")},
                       {"xrefPath", xref ? Value(str_of(orv_value(ref(B.s(), "xrefPath")))) : Value("")},
                       {"description", d.truthy() ? d : Value("")},
                       {"owned", std::move(owned)},
                       {"inserts", std::move(ins)},
                       {"block", B.block},
                       {"endblk", B.endblk},
                       {"layout", B.layout},
                       {"insertUnits", ivk(src, "insertUnits")},
                       {"explodable", ref(src, "explodable")},
                       {"scalable", ref(src, "scalable")},
                       {"eed", W.eed_of(ref(src, "xdata"))}}));
    const double block_mode = space_mode(kind);
    Value blk = make_object({{"t", "BLOCK"},
                             {"h", B.block},
                             {"mode", block_mode},
                             {"owner", block_mode != 0 ? 0 : B.hdr},
                             {"layer", W.H["layer0"]},
                             {"ltype", make_object({{"flag", 0}})},
                             {"color", make_object({{"index", 256}})},
                             {"lw", 29},
                             {"name", B.name}});
    if (!W.r2004) {
      blk.set("prev", 0);
      blk.set("next", 0);
    }
    pushv(std::move(blk));
    for (std::size_t i = 0; i < B.ents.size(); ++i) {
      if (!W.r2004) {
        Value& r = B.ents[i][0]->v;
        r.set("prev", i != 0 ? ref(B.ents[i - 1][0]->v, "h") : Value(0));
        r.set("next", i + 1 < B.ents.size() ? ref(B.ents[i + 1][0]->v, "h") : Value(0));
      }
    }
    for (auto& list : B.ents) {
      Value& head = list[0]->v;
      if (const Value* br = head.find("blockRef"); br != nullptr) {
        const double bi = br->as_number();
        head.set("block", bi >= 0 && bi < static_cast<double>(W.blocks.size())
                              ? W.blocks[static_cast<std::size_t>(bi)].hdr
                              : 0.0);
      }
      push(list[0]);
      std::vector<RecPtr> chain;
      for (std::size_t i = 1; i < list.size(); ++i) {
        if (!is_str(ref(list[i]->v, "t"), "SEQEND")) {
          chain.push_back(list[i]);
        }
      }
      for (std::size_t i = 0; i < chain.size(); ++i) {
        if (!W.r2004) {
          chain[i]->v.set("prev", i != 0 ? ref(chain[i - 1]->v, "h") : Value(0));
          chain[i]->v.set("next", i + 1 < chain.size() ? ref(chain[i + 1]->v, "h") : Value(0));
        }
        push(chain[i]);
      }
      for (std::size_t i = 1; i < list.size(); ++i) {
        if (is_str(ref(list[i]->v, "t"), "SEQEND")) {
          if (!W.r2004) {
            list[i]->v.set("prev", 0);
            list[i]->v.set("next", 0);
          }
          push(list[i]);
        }
      }
    }
    Value end = make_object({{"t", "ENDBLK"},
                             {"h", B.endblk},
                             {"mode", block_mode},
                             {"owner", block_mode != 0 ? 0 : B.hdr},
                             {"layer", W.H["layer0"]},
                             {"ltype", make_object({{"flag", 0}})},
                             {"color", make_object({{"index", 256}})},
                             {"lw", 29}});
    if (!W.r2004) {
      end.set("prev", 0);
      end.set("next", 0);
    }
    pushv(std::move(end));
  }

  W.TBL->finish(recs);
  {
    std::vector<std::pair<Value, double>> ls;
    ls.reserve(layouts.size());
    for (const LayoutW& L : layouts) {
      ls.emplace_back(ref(L.s(), "handle"), L.h);
    }
    W.X->finish(recs, ls);
  }
  W.SOL->finish();
  W.SOL->finish_surfaces(recs);
  const double handseed = W.seed;

  // ---------------------------------------------------------------- encode
  const VersionInfo info = version_info(version);
  Ctx ctx;
  ctx.version = version;
  {
    const Value& mm = ref(hdr_in, "MEASUREMENT");
    const int iu = base::js_to_int32(base::js_to_number(ref(hdr_in, "INSUNITS")));
    ctx.metric =
        !mm.is_undefined() ? mm.truthy() : iu != 1 && iu != 2 && iu != 3 && iu != 8 && iu != 9 && iu != 10 && iu != 0;
  }
  ctx.created_ms = created;
  ctx.handseed = handseed;
  ctx.extents = ext;
  ctx.guid = [&W] { return W.guid(); };
  ctx.dwg_version = info.code;
  ctx.maint = info.maint;
  ctx.classed = std::move(W.classed);
  for (RecPtr const& r : recs) {
    if (is_str(ref(r->v, "t"), "LEADER") && ref(r->v, "annotationSrc").truthy()) {
      r->v.set("annotation", W.src(ref(r->v, "annotationSrc")));
    }
  }
  double dropped_eed = 0;
  for (RecPtr const& r : recs) {
    Value* eed = r->v.find("eed");
    if (eed == nullptr || !eed->truthy()) {
      continue;
    }
    Array kept;
    for (const Value& blk : eed->as_array()) {
      Array items;
      bool drop = false;
      for (const Value& it : arr(blk, "items")) {
        const Array& pair = it.as_array();
        const double code = base::js_to_number(pair[0]);
        const Value& v = pair.size() > 1 ? pair[1] : undef();
        if (code == 1005 || code == 1003) {
          if (hex_key(v) == Key("0")) {
            items.emplace_back(Array{pair[0], "0"});
            continue;
          }
          const double n = W.src(v);
          if (n == 0) {
            dropped_eed += 1;
            drop = true;
            break;
          }
          items.emplace_back(Array{pair[0], hex_upper(n)});
        } else {
          items.push_back(it);
        }
      }
      if (drop) {
        continue;
      }
      Value b2 = blk;
      b2.set("items", std::move(items));
      kept.push_back(std::move(b2));
    }
    if (kept.empty()) {
      r->v.erase("eed");
    } else {
      *eed = std::move(kept);
    }
  }
  if (dropped_eed != 0) {
    note(msg("ccdwg.write.eed_dropped", {{"n", dropped_eed}}));
  }
  Parts parts;
  double entities = 0;
  // /CONTROL|^LAYER$|^STYLE$|^LTYPE$|^APPID$|^DIMSTYLE$|^VPORT$|^VIEW$|DICTIONARY|PLACEHOLDER|MLINESTYLE|LAYOUT|BLOCK_HEADER/
  const auto not_entity = [](const std::string& t) {
    for (const std::string_view k : {"CONTROL", "DICTIONARY", "PLACEHOLDER", "MLINESTYLE", "LAYOUT", "BLOCK_HEADER"}) {
      if (t.contains(k)) {
        return true;
      }
    }
    return t == "LAYER" || t == "STYLE" || t == "LTYPE" || t == "APPID" || t == "DIMSTYLE" || t == "VPORT" ||
           t == "VIEW";
  };
  for (RecPtr const& r : recs) {
    std::optional<ByteBuf> data = encode_object(*r, ctx);
    const std::string t = ref(r->v, "t").is_string() ? ref(r->v, "t").as_string() : std::string();
    if (!data) {
      note("internal: no encoder for " + t);
      continue;
    }
    parts.objects.push_back({ref(r->v, "h").as_number(), std::move(*data)});
    if (!not_entity(t)) {
      entities += 1;
    }
  }
  std::ranges::stable_sort(parts.objects,
                           [](const Parts::Object& a, const Parts::Object& b) { return a.handle < b.handle; });
  Value hv = hdr_in;
  if (ref(hv, "INSUNITS").is_undefined()) {
    hv.set("INSUNITS", 0);
  }
  if (ref(hv, "LUPREC").is_undefined()) {
    hv.set("LUPREC", ctx.metric ? 3 : 2);
  }
  std::map<std::string, double, std::less<>> Hh = W.H;
  for (const auto& [k, v] : W.X->header_handles) {
    Hh[k] = v;
  }
  Hh["msBlockHdr"] = W.MS->hdr;
  Hh["psBlockHdr"] = W.PSs[0]->hdr;
  {
    const Value& cl = ref(hdr_in, "CLAYER");
    double h = 0;
    if (const auto it = W.layer_by_name.find(nullish(cl) ? std::string("0") : str_of(cl));
        it != W.layer_by_name.end()) {
      h = ref(it->second->v, "h").as_number();
    }
    if (h == 0) {
      h = W.src(cl);
    }
    if (h == 0) {
      if (const auto it = W.layer_by_new.find(base::js_string(cl)); it != W.layer_by_new.end()) {
        h = ref(it->second->v, "h").as_number();
      }
    }
    Hh["clayer"] = h != 0 ? h : W.H["layer0"];
  }
  Hh["textstyle"] = W.style_ref(ref(hdr_in, "TEXTSTYLE"));
  {
    const double c = num(get(W.ltype_ref(ref(hdr_in, "CELTYPE")), "h"));
    Hh["celtype"] = c != 0 ? c : W.H["ltByLayer"];
  }
  Hh["dimstyleCur"] = W.dim_ref(ref(hdr_in, "DIMSTYLE"));
  Hh["cmlstyle"] = W.H["mlstyle"];
  parts.header = encode_header(Hh, hv, ctx);
  parts.classes = encode_classes(ctx);
  const JulianPair jd = julian(created);
  parts.created = {jd.day, jd.ms};
  parts.updated = {jd.day, jd.ms};
  parts.measurement = ctx.metric;
  parts.handseed = handseed;
  parts.keys = {handseed,        W.H["blockCtl"], W.H["layerCtl"], W.H["styleCtl"], W.H["ltypeCtl"],
                W.H["viewCtl"],  W.H["ucsCtl"],   W.H["vportCtl"], W.H["appidCtl"], W.H["dimstyleCtl"],
                W.H["vpentCtl"], W.H["nod"],      W.H["mlsDict"],  W.H["groupDict"]};
  parts.acds = W.SOL->data_storage();
  WriteResult res;
  if (version == "AC1015") {
    res.bytes = layout_r2000(parts);
  } else if (version == "AC1021") {
    res.bytes = layout_r2007(parts);
  } else {
    res.bytes = layout_r2004(parts, version);
  }
  if (W.lost_rgb != 0) {
    note(msg("ccdwg.write.true_colours", {{"n", W.lost_rgb}}));
  }
  if (text_loss().lost != 0) {
    note(msg("ccdwg.write.chars_lost", {{"n", text_loss().lost}}));
  }
  W.report.set("objects", static_cast<double>(parts.objects.size()));
  W.report.set("entities", entities);
  res.report = std::move(W.report);
  return res;
}

}  // namespace cc::dwg
