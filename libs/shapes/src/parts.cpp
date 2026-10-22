// SPDX-License-Identifier: Apache-2.0
// lib/shapes/parts.js: standard part families, sizes, default options and part records.
#include "cc/shapes/parts.hpp"

#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/shapes/sections.hpp"
#include "jsnum.hpp"

namespace cc::shapes {
namespace {

struct MetricRow {
  const char* size;
  std::array<double, 8> v;  // d, P, s, k, m, d1, d2, h
};
struct InchRow {
  const char* size;
  std::array<double, 5> v;  // D, tpi, F, H (bolt), H (nut)
};
struct BearingRow {
  const char* size;
  std::array<double, 3> v;  // d, D, B
};
#include "parts_data.inc"

constexpr double IN = kIn;
constexpr double PI = std::numbers::pi;

constexpr std::array<PartStandard, 11> kStandards = {{
    {"ISO 4014", "ISO 4014:2022 Fasteners — Hexagon head bolts — Product grades A and B",
     "https://www.iso.org/standard/72579.html"},
    {"ISO 4017", "ISO 4017:2022 Fasteners — Hexagon head screws — Product grades A and B",
     "https://www.iso.org/standard/72585.html"},
    {"ISO 4032", "ISO 4032:2023 Fasteners — Hexagon regular nuts (style 1)", "https://www.iso.org/standard/75016.html"},
    {"ISO 7089", "ISO 7089:2000 Plain washers — Normal series — Product grade A",
     "https://www.iso.org/standard/13666.html"},
    {"ISO 2338", "ISO 2338:1997 Parallel pins, of unhardened steel and austenitic stainless steel",
     "https://www.iso.org/standard/20001.html"},
    {"ISO 15", "ISO 15:2017 Rolling bearings — Radial bearings — Boundary dimensions, general plan",
     "https://www.iso.org/standard/69977.html"},
    {"DIN 6885-1", "DIN 6885-1 Drive type fastenings without taper action — Parallel keys, keyways, deep pattern",
     "https://www.din.de/en/getting-involved/standards-committees/nam/publications/wdc-beuth:din21:346315269"},
    {"ASME B18.2.1",
     "ASME B18.2.1 Square, hex, heavy hex and askew head bolts and hex, heavy hex, hex flange, lobed head and lag "
     "screws (inch series)",
     "https://www.asme.org/codes-standards/find-codes-standards/"
     "b18-2-1-square-hex-heavy-hex-askew-head-bolts-hex-heavy-hex-hex-flange-lobed-head-lag-screws"},
    {"ASME B18.2.2",
     "ASME B18.2.2-2022 Nuts for general applications: machine screw nuts, and hex, square, hex flange and coupling "
     "nuts (inch series)",
     "https://www.asme.org/codes-standards/find-codes-standards/"
     "b18-2-2-nuts-general-applications-machine-screw-nuts-hex-square-hex-flange-coupling-nuts"},
    {"ISO 54", "ISO 54:1996 Cylindrical gears for general engineering and for heavy engineering — Modules",
     "https://www.iso.org/standard/22644.html"},
    {"ISO 53", "ISO 53:1998 Cylindrical gears for general and heavy engineering — Standard basic rack tooth profile",
     "https://www.iso.org/standard/22643.html"},
}};

const std::vector<PartFamily>& families() {
  static const std::vector<PartFamily> f = {
      {"iso4017",
       "hex-screw",
       "Hex head screw",
       "ISO 4017",
       "mm",
       "Steel, property class 8.8",
       {{"length", "mm", 4, 500}}},
      {"iso4014", "hex-bolt", "Hex bolt", "ISO 4014", "mm", "Steel, property class 8.8", {{"length", "mm", 20, 500}}},
      {"iso4032", "hex-nut", "Hex nut", "ISO 4032", "mm", "Steel, property class 8", {}},
      {"iso7089", "washer", "Plain washer", "ISO 7089", "mm", "Steel, 200 HV", {}},
      {"asme-hex-bolt", "hex-bolt", "Hex bolt", "ASME B18.2.1", "in", "Steel", {{"length", "in", 0.25, 24}}},
      {"asme-hex-nut", "hex-nut", "Hex nut", "ASME B18.2.2", "in", "Steel", {}},
      {"iso2338", "pin", "Parallel pin", "ISO 2338", "mm", "Steel, unhardened", {{"length", "mm", 4, 300}}},
      {"iso15", "bearing", "Deep groove ball bearing", "ISO 15", "mm", "Bearing steel", {}},
      {"din6885", "key", "Parallel key", "DIN 6885-1", "mm", "Steel, key steel", {{"length", "mm", 4, 800}}},
      {"spur-gear",
       "gear",
       "Spur gear",
       "ISO 54",
       "mm",
       "Steel",
       {{"teeth", "", 6, 400, true}, {"width", "mm", 1, 1000}, {"bore", "mm", 0, 5000}}},
  };
  return f;
}

template <std::size_t N>
double near(const std::array<double, N>& list, double v) {
  double a = list[0];
  for (const double b : list) {
    if (std::abs(b - v) < std::abs(a - v)) {
      a = b;
    }
  }
  return a;
}

std::string fmt(double v) {
  return ns(fixed(v, 3));
}
std::string inch_text(double v) {  // 1.5 → "1-1/2", 0.75 → "3/4" (to 1/64)
  const double n = base::js_round(v * 64);
  const double w = std::floor(n / 64);
  double num = n - w * 64;
  double den = 64;
  while (num != 0 && std::fmod(num, 2) == 0) {
    num /= 2;
    den /= 2;
  }
  if (num == 0) {
    return ns(w);
  }
  return (w != 0 ? ns(w) + "-" : "") + ns(num) + "/" + ns(den);
}
double hex_area(double s) {
  return (std::numbers::sqrt3 / 2) * s * s;
}
double kg(double mm3) {
  return fixed(mm3 * 1e-9 * kSteelDensity, 4);
}

base::Value obj(std::initializer_list<base::Init> m) {
  return base::make_object(m);
}

double num(const base::Value& o, std::string_view k) {
  return o.get(k).num_or(std::numeric_limits<double>::quiet_NaN());
}

/// opts[key] as JS reads a property of any value (null throws, as `null[key]` does).
base::Value prop(const base::Value& o, std::string_view key) {
  if (o.is_null()) {
    throw std::runtime_error("Cannot read properties of null (reading '" + std::string(key) + "')");
  }
  if (o.is_object() && !o.is_point()) {
    const base::Value* v = o.find(key);
    return v ? *v : base::Value{};
  }
  if (o.is_point()) {
    return o.get(key);
  }
  if (key == "length") {
    if (o.is_array()) {
      return static_cast<double>(o.as_array().size());
    }
    if (o.is_string()) {
      return static_cast<double>(base::utf16_length(o.as_string()));
    }
  }
  return {};
}

const base::Value* find_size(const base::Value& sizes, std::string_view size) {
  for (const auto& z : sizes.as_array()) {
    if (z.get("size").as_string() == size) {
      return &z;
    }
  }
  return nullptr;
}

}  // namespace

std::span<const PartStandard> part_standards() {
  return kStandards;
}
const PartStandard* find_standard(std::string_view id) {
  for (const auto& s : kStandards) {
    if (s.id == id) {
      return &s;
    }
  }
  return nullptr;
}
std::span<const PartFamily> part_families() {
  return families();
}
const PartFamily* find_family(std::string_view id) {
  for (const auto& f : families()) {
    if (f.id == id) {
      return &f;
    }
  }
  return nullptr;
}

base::Value part_sizes(std::string_view id) {
  base::Array out;
  if (id == "iso4014" || id == "iso4017") {
    for (const auto& r : kMetric) {
      out.push_back(
          obj({{"size", r.size}, {"label", r.size}, {"d", r.v[0]}, {"P", r.v[1]}, {"s", r.v[2]}, {"k", r.v[3]}}));
    }
  } else if (id == "iso4032") {
    for (const auto& r : kMetric) {
      out.push_back(
          obj({{"size", r.size}, {"label", r.size}, {"d", r.v[0]}, {"P", r.v[1]}, {"s", r.v[2]}, {"m", r.v[4]}}));
    }
  } else if (id == "iso7089") {
    for (const auto& r : kMetric) {
      out.push_back(obj({{"size", ns(r.v[0])},
                         {"label", ns(r.v[0])},
                         {"thread", r.size},
                         {"d1", r.v[5]},
                         {"d2", r.v[6]},
                         {"h", r.v[7]}}));
    }
  } else if (id == "asme-hex-bolt" || id == "asme-hex-nut") {
    const bool bolt = id == "asme-hex-bolt";
    for (const auto& r : kInch) {
      const auto [D, tpi, F, H, Hn] = r.v;
      base::Value z = obj({{"size", r.size},
                           {"label", std::string(r.size) + "-" + ns(tpi) + " UNC"},
                           {"d", D * IN},
                           {"tpi", tpi},
                           {"P", IN / tpi},
                           {"s", F * IN}});
      z.append(bolt ? "k" : "m", (bolt ? H : Hn) * IN);
      out.push_back(std::move(z));
    }
  } else if (id == "iso2338") {
    for (const auto& [d, c] : kPins) {
      out.push_back(obj({{"size", ns(d)}, {"label", ns(d)}, {"d", d}, {"c", c}}));
    }
  } else if (id == "iso15") {
    for (const auto& r : kBearings) {
      out.push_back(obj({{"size", r.size}, {"label", r.size}, {"d", r.v[0]}, {"D", r.v[1]}, {"B", r.v[2]}}));
    }
  } else if (id == "din6885") {
    for (const auto& [b, h, s0, s1, t1, t2] : kKeys) {
      out.push_back(obj({{"size", ns(b) + "x" + ns(h)},
                         {"label", ns(b) + " × " + ns(h)},
                         {"b", b},
                         {"h", h},
                         {"shaft", base::Array{s0, s1}},
                         {"t1", t1},
                         {"t2", t2}}));
    }
  } else if (id == "spur-gear") {
    for (const double m : kModules) {
      out.push_back(obj({{"size", "m" + ns(m)}, {"label", "m " + ns(m)}, {"m", m}}));
    }
  }
  return out;
}

base::Value part_options(std::string_view id, const base::Value& size) {
  if (!size.is_string()) {
    return base::Value::object();
  }
  const base::Value sizes = part_sizes(id);
  const base::Value* zp = find_size(sizes, size.as_string());
  if (!zp) {
    return base::Value::object();
  }
  const base::Value& z = *zp;
  if (id == "iso4017") {
    return obj({{"length", near(kRLengths, 4 * num(z, "d"))}});
  }
  if (id == "iso4014") {
    const double d = num(z, "d");
    return obj({{"length", near(kRLengths, js_max(6 * d, 2 * d + 6 + 10))}});
  }
  if (id == "asme-hex-bolt") {
    return obj({{"length", near(kInchLengths, 4 * num(z, "d") / IN)}});
  }
  if (id == "iso2338") {
    return obj({{"length", near(kRLengths, 5 * num(z, "d"))}});
  }
  if (id == "din6885") {
    return obj({{"length", near(kKeyLengths, 4 * num(z, "b"))}});
  }
  if (id == "spur-gear") {
    const double teeth = 20;
    const double m = num(z, "m");
    return obj({{"teeth", teeth}, {"width", 10 * m}, {"bore", js_max(0, base::js_round(m * (teeth - 2.5) / 3))}});
  }
  return base::Value::object();
}

base::Value make_part(std::string_view id, const base::Value& size, const base::Value& opts) {
  const PartFamily* fam = find_family(id);
  if (!fam) {
    return obj({{"ok", false}, {"error", "unknown family"}});
  }
  const base::Value sizes = part_sizes(id);
  const base::Value* zp = find_size(sizes, base::js_string(size));
  if (!zp) {
    return obj({{"ok", false}, {"error", "unknown size"}});
  }
  const base::Value& z = *zp;
  const std::string zsize = z.get("size").as_string();
  base::Value o = part_options(id, zsize);
  const base::Value given = opts.is_undefined() ? base::Value::object() : opts;
  for (const auto& spec : fam->options) {
    const base::Value raw = prop(given, spec.key);
    if (raw.is_undefined() || raw.is_null() || (raw.is_string() && raw.as_string().empty())) {
      continue;
    }
    const double v = base::js_to_number(raw);
    if (!std::isfinite(v) || v < spec.min || v > spec.max || (spec.integer && v != std::trunc(v))) {
      return obj({{"ok", false}, {"error", "bad " + std::string(spec.key)}, {"key", spec.key}});
    }
    o.set(spec.key, v);
  }
  const PartStandard* st = find_standard(fam->standard);
  base::Value part = obj({{"family", id},
                          {"type", fam->type},
                          {"name", fam->name},
                          {"standard", fam->standard},
                          {"standardTitle", st->title},
                          {"url", st->url},
                          {"size", zsize},
                          {"units", fam->units},
                          {"material", fam->material},
                          {"options", o}});
  const auto bad = [](std::string_view key) {
    return obj({{"ok", false}, {"error", "bad " + std::string(key)}, {"key", key}});
  };
  base::Value dims;
  base::Value mass = nullptr;
  base::Value box;
  std::string designation;
  std::string size_text;
  const double sq3 = std::numbers::sqrt3;
  if (id == "iso4017" || id == "iso4014") {
    const double l = num(o, "length");
    const double d = num(z, "d");
    const double k = num(z, "k");
    const double s = num(z, "s");
    if (!(l > k)) {
      return bad("length");
    }
    double b = l;
    if (id == "iso4014") {
      b = 2 * d + 25;
      if (l <= 125) {
        b = 2 * d + 6;
      } else if (l <= 200) {
        b = 2 * d + 12;
      }
    }
    if (id == "iso4014" && !(b < l)) {
      return bad("length");
    }
    const double e = s * 2 / sq3;
    dims = obj({{"d", d}, {"P", num(z, "P")}, {"s", s}, {"e", e}, {"k", k}, {"l", l}, {"b", b}});
    size_text = zsize + " × " + fmt(l);
    designation = std::string(fam->standard) + " " + size_text;
    mass = kg(hex_area(s) * k + PI / 4 * d * d * l);
    box = obj({{"w", e}, {"d", k + l}, {"h", s}});
  } else if (id == "asme-hex-bolt") {
    const double lin = num(o, "length");
    const double l = lin * IN;
    const double d = num(z, "d");
    const double k = num(z, "k");
    const double s = num(z, "s");
    const double e = s * 2 / sq3;
    dims = obj({{"d", d},
                {"P", num(z, "P")},
                {"tpi", num(z, "tpi")},
                {"s", s},
                {"e", e},
                {"k", k},
                {"l", l},
                {"b", js_min(l, (lin <= 6 ? 2 * d / IN + 0.25 : 2 * d / IN + 0.5) * IN)}});
    size_text = z.get("label").as_string() + " × " + inch_text(lin) + " in";
    designation = size_text + ", " + std::string(fam->standard);
    mass = kg(hex_area(s) * k + PI / 4 * d * d * l);
    box = obj({{"w", e}, {"d", k + l}, {"h", s}});
  } else if (id == "iso4032" || id == "asme-hex-nut") {
    const double d = num(z, "d");
    const double s = num(z, "s");
    const double m = num(z, "m");
    const double e = s * 2 / sq3;
    dims = obj({{"d", d}, {"P", num(z, "P")}, {"s", s}, {"e", e}, {"m", m}});
    const double tpi = num(z, "tpi");
    if (tpi != 0 && !std::isnan(tpi)) {
      dims.append("tpi", tpi);
    }
    const bool iso = id == "iso4032";
    size_text = iso ? zsize : z.get("label").as_string();
    designation =
        iso ? std::string(fam->standard) + " " + zsize : z.get("label").as_string() + ", " + std::string(fam->standard);
    mass = kg((hex_area(s) - PI / 4 * d * d) * m);
    box = obj({{"w", e}, {"d", e}, {"h", m}});
  } else if (id == "iso7089") {
    const double d1 = num(z, "d1");
    const double d2 = num(z, "d2");
    const double h = num(z, "h");
    dims = obj({{"d1", d1}, {"d2", d2}, {"h", h}});
    size_text = zsize;
    designation = std::string(fam->standard) + " " + zsize;
    mass = kg(PI / 4 * (d2 * d2 - d1 * d1) * h);
    box = obj({{"w", d2}, {"d", d2}, {"h", h}});
  } else if (id == "iso2338") {
    const double l = num(o, "length");
    const double d = num(z, "d");
    const double c = num(z, "c");
    if (!(l > 2 * c)) {
      return bad("length");
    }
    dims = obj({{"d", d}, {"c", c}, {"l", l}});
    size_text = zsize + " × " + fmt(l);
    designation = std::string(fam->standard) + " " + size_text;
    mass = kg(PI / 4 * d * d * l);
    box = obj({{"w", d}, {"d", l}, {"h", d}});
  } else if (id == "iso15") {
    const double d = num(z, "d");
    const double D = num(z, "D");
    const double B = num(z, "B");
    dims = obj({{"d", d}, {"D", D}, {"B", B}});
    size_text = zsize + " (" + ns(d) + " × " + ns(D) + " × " + ns(B) + ")";
    designation = size_text;
    box = obj({{"w", D}, {"d", D}, {"h", B}});
  } else if (id == "din6885") {
    const double l = num(o, "length");
    const double b = num(z, "b");
    const double h = num(z, "h");
    if (!(l > b)) {
      return bad("length");
    }
    dims = obj({{"b", b}, {"h", h}, {"l", l}, {"t1", num(z, "t1")}, {"t2", num(z, "t2")}, {"shaft", z.get("shaft")}});
    size_text = "A " + ns(b) + " × " + ns(h) + " × " + fmt(l);
    designation = std::string(fam->standard) + " " + size_text;
    mass = kg((b * (l - b) + PI / 4 * b * b) * h);
    box = obj({{"w", l}, {"d", b}, {"h", h}});
  } else if (id == "spur-gear") {
    const double m = num(z, "m");
    const double teeth = num(o, "teeth");
    const double width = num(o, "width");
    const double bore = num(o, "bore");
    const double d = m * teeth;
    const double da = m * (teeth + 2);
    const double df = m * (teeth - 2.5);
    if (!(bore < df)) {
      return bad("bore");
    }
    dims = obj({{"m", m},
                {"z", teeth},
                {"d", d},
                {"da", da},
                {"df", df},
                {"db", d * base::js_cos(20 * PI / 180)},
                {"width", width},
                {"bore", bore},
                {"alpha", 20}});
    size_text = "m " + fmt(m) + ", z " + ns(teeth) + ", " + fmt(width) + " wide";
    designation = size_text + ", ISO 53 / ISO 54";
    box = obj({{"w", da}, {"d", da}, {"h", width}});
  }
  part.append("dims", std::move(dims));
  part.append("designation", designation);
  part.append("sizeText", size_text);
  part.append("massKg", std::move(mass));
  part.append("box", std::move(box));
  return obj({{"ok", true}, {"part", std::move(part)}});
}

base::Value library_set() {
  base::Array out;
  const auto add = [&](std::string_view fam, std::string size, base::Value opts) {
    out.emplace_back(base::Array{fam, std::move(size), std::move(opts)});
  };
  const auto len = [](double l) { return obj({{"length", l}}); };
  for (const auto& [s, l] : std::initializer_list<std::pair<const char*, double>>{{"M6", 20},
                                                                                  {"M6", 40},
                                                                                  {"M8", 25},
                                                                                  {"M8", 50},
                                                                                  {"M10", 30},
                                                                                  {"M10", 60},
                                                                                  {"M12", 40},
                                                                                  {"M12", 80},
                                                                                  {"M16", 50},
                                                                                  {"M16", 100},
                                                                                  {"M20", 60},
                                                                                  {"M20", 120},
                                                                                  {"M24", 80},
                                                                                  {"M24", 150}}) {
    add("iso4017", s, len(l));
  }
  for (const auto& [s, l] : std::initializer_list<std::pair<const char*, double>>{
           {"M10", 80}, {"M12", 100}, {"M16", 120}, {"M20", 150}, {"M24", 200}}) {
    add("iso4014", s, len(l));
  }
  for (const char* s : {"M6", "M8", "M10", "M12", "M16", "M20", "M24", "M30", "M36"}) {
    add("iso4032", s, base::Value::object());
  }
  for (const char* s : {"6", "8", "10", "12", "16", "20", "24", "30", "36"}) {
    add("iso7089", s, base::Value::object());
  }
  for (const auto& [s, l] : std::initializer_list<std::pair<const char*, double>>{
           {"1/4", 1}, {"3/8", 1.5}, {"1/2", 2}, {"5/8", 2.5}, {"3/4", 3}, {"1", 4}}) {
    add("asme-hex-bolt", s, len(l));
  }
  for (const auto& r : kInch) {
    add("asme-hex-nut", r.size, base::Value::object());
  }
  for (const auto& [s, l] :
       std::initializer_list<std::pair<const char*, double>>{{"4", 20}, {"6", 30}, {"8", 40}, {"10", 50}, {"12", 60}}) {
    add("iso2338", s, len(l));
  }
  for (const auto& r : kBearings) {
    add("iso15", r.size, base::Value::object());
  }
  for (const auto& k : kKeys) {
    add("din6885", ns(k[0]) + "x" + ns(k[1]), base::Value::object());
  }
  for (const auto& [s, z] : std::initializer_list<std::pair<const char*, double>>{
           {"m1", 20}, {"m1", 40}, {"m2", 20}, {"m2", 40}, {"m3", 30}}) {
    add("spur-gear", s, obj({{"teeth", z}}));
  }
  return out;
}

base::Value standards_value() {
  base::Value o = base::Value::object();
  for (const auto& s : kStandards) {
    o.append(s.id, obj({{"title", s.title}, {"url", s.url}}));
  }
  return o;
}
base::Value families_part_value() {
  base::Array out;
  for (const auto& f : families()) {
    base::Array opts;
    for (const auto& o : f.options) {
      base::Value v = obj({{"key", o.key}, {"unit", o.unit}, {"min", o.min}, {"max", o.max}});
      if (o.integer) {
        v.append("integer", true);
      }
      opts.emplace_back(std::move(v));
    }
    out.push_back(obj({{"id", f.id},
                       {"type", f.type},
                       {"name", f.name},
                       {"standard", f.standard},
                       {"units", f.units},
                       {"material", f.material},
                       {"options", std::move(opts)}}));
  }
  return out;
}

}  // namespace cc::shapes
