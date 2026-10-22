// SPDX-License-Identifier: Apache-2.0
// lib/shapes/items.js: sections and standard parts as library items.
#include "cc/shapes/items.hpp"

#include <array>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/shapes/parts.hpp"
#include "jsnum.hpp"

namespace cc::shapes {

std::string utf16_prefix(std::string_view s, std::size_t n) {
  std::string out;
  std::size_t units = 0;
  std::size_t i = 0;
  while (i < s.size() && units < n) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t len = 4;
    if (c < 0x80) {
      len = 1;
    } else if (c < 0xE0) {
      len = 2;
    } else if (c < 0xF0) {
      len = 3;
    }
    if (len == 4) {
      if (units + 2 > n) {  // keep the lead surrogate alone, as String#slice does
        std::uint32_t cp = c & 0x07U;
        for (std::size_t k = 1; k < 4 && i + k < s.size(); ++k) {
          cp = (cp << 6U) | (static_cast<unsigned char>(s[i + k]) & 0x3FU);
        }
        base::append_code_point(out, 0xD800U + ((cp - 0x10000U) >> 10U));
        return out;
      }
      units += 2;
    } else {
      units += 1;
    }
    out.append(s.substr(i, len));
    i += len;
  }
  return out;
}

namespace {

constexpr double PI = std::numbers::pi;

double r3(double v) {
  return base::js_round(v * 1000) / 1000;
}
double r1(double v) {
  return base::js_round(v * 10) / 10;
}
double kgm(double v) {
  return base::js_round(v * 100) / 100;
}
/// String(Number(Number(v).toFixed(2))) with thousands commas (the JS's /\B(?=(\d{3})+(?!\d))/g).
std::string num_text(double v) {
  const std::string s = ns(fixed(v, 2));
  const auto digit = [](char c) { return c >= '0' && c <= '9'; };
  const auto word = [&](char c) { return digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
  std::string out;
  for (std::size_t p = 0; p < s.size(); ++p) {
    if (p > 0 && word(s[p - 1]) == word(s[p])) {
      std::size_t run = 0;
      while (p + run < s.size() && digit(s[p + run])) {
        ++run;
      }
      if (run >= 3 && run % 3 == 0) {
        out += ',';
      }
    }
    out += s[p];
  }
  return out;
}

constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kFamilyType = {{
    {"W", "W shape"},
    {"S", "S shape"},
    {"C", "Channel"},
    {"L", "Angle"},
    {"HSS", "Hollow structural section"},
    {"Pipe", "Pipe"},
    {"IPE", "IPE beam"},
    {"HEA", "HEA beam"},
    {"HEB", "HEB beam"},
    {"UPN", "UPN channel"},
}};

base::Value obj(std::initializer_list<base::Init> m) {
  return base::make_object(m);
}

base::Value layers(const std::string& prefix) {
  return base::Array{obj({{"name", prefix}, {"color", 7}}), obj({{"name", prefix + "-THIN"}, {"color", 8}}),
                     obj({{"name", prefix + "-CENTER"}, {"color", 1}}),
                     obj({{"name", prefix + "-HIDDEN"}, {"color", 4}}),
                     obj({{"name", prefix + "-FILL"}, {"color", 254}})};
}

struct P3 {
  double x;
  double y;
  double b = 0;
};

class Drawer {
 public:
  explicit Drawer(std::string prefix) : prefix_(std::move(prefix)) {}
  base::Array ents;
  void line(double x1, double y1, double x2, double y2, std::string_view k = "") {
    ents.push_back(obj({{"type", "line"},
                        {"layer", layer(k)},
                        {"color", nullptr},
                        {"x1", r3(x1)},
                        {"y1", r3(y1)},
                        {"x2", r3(x2)},
                        {"y2", r3(y2)}}));
  }
  void poly(const std::vector<P3>& pts, bool closed = true, std::string_view k = "") {
    base::Array a;
    for (const auto& p : pts) {
      if (p.b != 0 && !std::isnan(p.b)) {
        a.emplace_back(base::Array{r3(p.x), r3(p.y), r3(p.b)});
      } else {
        a.emplace_back(base::Array{r3(p.x), r3(p.y)});
      }
    }
    ents.push_back(
        obj({{"type", "poly"}, {"layer", layer(k)}, {"color", nullptr}, {"pts", std::move(a)}, {"closed", closed}}));
  }
  void poly_loop(const Loop& l) {
    std::vector<P3> pts;
    pts.reserve(l.size());
    for (const auto& [x, y] : l) {
      pts.push_back({x, y});
    }
    poly(pts);
  }
  void circle(double x, double y, double r, std::string_view k = "") {
    ents.push_back(
        obj({{"type", "circle"}, {"layer", layer(k)}, {"color", nullptr}, {"x", r3(x)}, {"y", r3(y)}, {"r", r3(r)}}));
  }
  void arc(double x, double y, double r, double a0, double a1, std::string_view k = "") {
    ents.push_back(obj({{"type", "arc"},
                        {"layer", layer(k)},
                        {"color", nullptr},
                        {"x", r3(x)},
                        {"y", r3(y)},
                        {"r", r3(r)},
                        {"a0", a0},
                        {"a1", a1}}));
  }
  void fill(const std::vector<Loop>& loops) {
    base::Array ls;
    for (const auto& l : loops) {
      base::Array flat;
      for (const auto& [x, y] : l) {
        flat.emplace_back(r3(x));
        flat.emplace_back(r3(y));
      }
      ls.emplace_back(std::move(flat));
    }
    ents.push_back(obj(
        {{"type", "hatch"}, {"layer", layer("FILL")}, {"color", nullptr}, {"solid", true}, {"loops", std::move(ls)}}));
  }
  void cross(double x, double y, double a) { cross(x, y, a, a); }
  void cross(double x, double y, double a, double b) {
    line(x - a, y, x + a, y, "CENTER");
    line(x, y - b, x, y + b, "CENTER");
  }

 private:
  [[nodiscard]] std::string layer(std::string_view k) const {
    return k.empty() ? prefix_ : prefix_ + "-" + std::string(k);
  }
  std::string prefix_;
};

base::Value item(std::string name, std::string description, base::Value plan, base::Value params, base::Value attrs) {
  return obj({{"format", "cadcraft-library-item"},
              {"format_version", 1},
              {"name", std::move(name)},
              {"kind", "detail"},
              {"category", "details/generic"},
              {"description", std::move(description)},
              {"maker", ""},
              {"model", ""},
              {"plan", std::move(plan)},
              {"body", nullptr},
              {"params", std::move(params)},
              {"source_attrs", std::move(attrs)}});
}

std::string sec_dims(const Section& s) {
  const auto mm = [](double v) { return ns(r1(v)); };
  const auto g = [&](std::string_view k) { return s.get(k); };
  if (s.kind == "I" || s.kind == "C") {
    const double r = g("r");
    return "d " + mm(g("d")) + ", bf " + mm(g("bf")) + ", tw " + mm(g("tw")) + ", tf " + mm(g("tf")) +
           (r != 0 && !std::isnan(r) ? ", r " + mm(r) : "") + " mm";
  }
  if (s.kind == "L") {
    return "legs " + mm(s.has("h") ? g("h") : g("b")) + " × " + mm(g("b")) + ", t " + mm(g("t")) + " mm";
  }
  if (s.kind == "RHS") {
    return mm(g("h")) + " × " + mm(g("b")) + " mm, wall " + mm(g("t")) + " mm (design)";
  }
  if (s.kind == "CHS") {
    return "OD " + mm(g("D")) + " mm, wall " + mm(g("t")) + " mm (design)";
  }
  return "";
}

double dim(const base::Value& dims, std::string_view k) {
  return dims.get(k).num_or(std::numeric_limits<double>::quiet_NaN());
}

void draw_bolt(Drawer& d, const base::Value& x) {
  const double D = dim(x, "d");
  const double e = dim(x, "e");
  const double k = dim(x, "k");
  const double l = dim(x, "l");
  const double b = dim(x, "b");
  const double pitch = dim(x, "P");
  d.poly({{-e / 2, 0}, {e / 2, 0}, {e / 2, k}, {-e / 2, k}});
  for (const double xx : {-e / 4, e / 4}) {
    d.line(xx, 0, xx, k);
  }
  const double c = js_min(pitch, D / 4);
  d.poly({{-D / 2, 0}, {-D / 2, -l + c}, {-D / 2 + c, -l}, {D / 2 - c, -l}, {D / 2, -l + c}, {D / 2, 0}}, false);
  const double d3 = D - 1.22687 * pitch;
  const double y_top = b >= l ? -js_min(2 * pitch, l / 4) : -(l - b);
  for (const double xx : {-d3 / 2, d3 / 2}) {
    d.line(xx, -l + c, xx, y_top, "THIN");
  }
  if (b < l) {
    d.line(-D / 2, y_top, D / 2, y_top);
  }
  d.line(0, k + 3, 0, -l - 3, "CENTER");
}
void draw_nut(Drawer& d, const base::Value& x) {
  const double D = dim(x, "d");
  const double s = dim(x, "s");
  const double e = dim(x, "e");
  const double pitch = dim(x, "P");
  std::vector<P3> hex;
  for (int k = 0; k < 6; ++k) {
    const double i = k;
    const double a = PI / 3 * i;
    hex.push_back({e / 2 * base::js_cos(a), e / 2 * base::js_sin(a)});
  }
  d.poly(hex);
  d.circle(0, 0, 0.95 * s / 2, "THIN");
  d.circle(0, 0, (D - 1.0825 * pitch) / 2);
  d.arc(0, 0, D / 2, 100, 10, "THIN");
  d.cross(0, 0, e / 2 + 3);
}
void draw_washer(Drawer& d, const base::Value& x) {
  const double d1 = dim(x, "d1");
  const double d2 = dim(x, "d2");
  d.circle(0, 0, d2 / 2);
  d.circle(0, 0, d1 / 2);
  d.cross(0, 0, d2 / 2 + 3);
}
void draw_pin(Drawer& d, const base::Value& x) {
  const double D = dim(x, "d");
  const double c = dim(x, "c");
  const double l = dim(x, "l");
  const double a = l / 2;
  const double h = D / 2;
  d.poly({{-a + c, -h}, {a - c, -h}, {a, -h + c}, {a, h - c}, {a - c, h}, {-a + c, h}, {-a, h - c}, {-a, -h + c}});
  for (const double xx : {-a + c, a - c}) {
    d.line(xx, -h, xx, h, "THIN");
  }
  d.line(-a - 3, 0, a + 3, 0, "CENTER");
}
void draw_bearing(Drawer& d, const base::Value& x) {
  const double di = dim(x, "d");
  const double D = dim(x, "D");
  const double w = (D - di) / 2;
  d.circle(0, 0, D / 2);
  d.circle(0, 0, di / 2);
  d.circle(0, 0, D / 2 - 0.3 * w, "THIN");
  d.circle(0, 0, di / 2 + 0.3 * w, "THIN");
  d.circle(0, 0, (D + di) / 4, "CENTER");
  d.cross(0, 0, D / 2 + 3);
}
void draw_key(Drawer& d, const base::Value& x) {
  const double b = dim(x, "b");
  const double l = dim(x, "l");
  const double xx = l / 2 - b / 2;
  const double y = b / 2;
  d.poly({{-xx, -y, 0}, {xx, -y, 1}, {xx, y, 0}, {-xx, y, 1}});
  d.line(-l / 2 - 3, 0, l / 2 + 3, 0, "CENTER");
}
void draw_gear(Drawer& d, const base::Value& x) {
  const double da = dim(x, "da");
  const double dp = dim(x, "d");
  const double df = dim(x, "df");
  const double bore = dim(x, "bore");
  d.circle(0, 0, da / 2);
  d.circle(0, 0, dp / 2, "CENTER");
  d.circle(0, 0, df / 2, "THIN");
  if (bore > 0) {
    d.circle(0, 0, bore / 2);
  }
  d.cross(0, 0, da / 2 + 3);
}

std::string_view draw_of(std::string_view type) {
  if (type == "hex-screw" || type == "hex-bolt") {
    return "bolt";
  }
  if (type == "hex-nut") {
    return "nut";
  }
  for (const std::string_view t : {"washer", "pin", "bearing", "key", "gear"}) {
    if (type == t) {
      return t;
    }
  }
  return "";
}

std::string_view view_text(std::string_view how) {
  if (how == "bolt") {
    return "Side view across corners; thread drawn by the ISO 6410 convention.";
  }
  if (how == "nut") {
    return "Face view; thread drawn by the ISO 6410 convention.";
  }
  if (how == "washer") {
    return "Face view.";
  }
  if (how == "pin") {
    return "Side view.";
  }
  if (how == "bearing") {
    return "Face view, simplified (rings and ball pitch circle).";
  }
  if (how == "key") {
    return "Top view (form A, round ends).";
  }
  return "Face view by the drawing convention: tip circle, pitch circle as a centre line, root circle thin; teeth not "
         "drawn one by one.";
}

std::string part_dims(const base::Value& p) {
  const base::Value x = p.get("dims");
  const auto mm = [](double v) { return ns(r1(v)) + " mm"; };
  const auto n = [&](std::string_view k) { return dim(x, k); };
  const std::string type = p.get("type").as_string();
  const auto thread = [&] {
    return p.get("units").as_string() == "in"
               ? base::js_string(p.get("size")) + " in, " + ns(n("tpi")) + " threads per inch"
               : "M" + ns(n("d")) + " × " + ns(n("P"));
  };
  if (type == "hex-screw" || type == "hex-bolt") {
    return "thread " + thread() + ", across flats " + mm(n("s")) + ", head " + mm(n("k")) + ", length " + mm(n("l")) +
           (n("b") < n("l") ? ", thread " + mm(n("b")) : ", threaded to the head");
  }
  if (type == "hex-nut") {
    return "thread " + thread() + ", across flats " + mm(n("s")) + ", height " + mm(n("m"));
  }
  if (type == "washer") {
    return "inside " + mm(n("d1")) + ", outside " + mm(n("d2")) + ", thickness " + mm(n("h"));
  }
  if (type == "pin") {
    return "diameter " + mm(n("d")) + ", length " + mm(n("l"));
  }
  if (type == "bearing") {
    return "bore " + mm(n("d")) + ", outside " + mm(n("D")) + ", width " + mm(n("B"));
  }
  if (type == "key") {
    const base::Value shaft = x.get("shaft");
    return ns(n("b")) + " × " + ns(n("h")) + " mm, length " + mm(n("l")) + ", for shafts " +
           base::js_string(shaft.as_array().at(0)) + "–" + base::js_string(shaft.as_array().at(1)) +
           " mm (keyseat depth " + ns(n("t1")) + " mm in the shaft, " + ns(n("t2")) + " mm in the hub)";
  }
  if (type == "gear") {
    const double bore = n("bore");
    return "module " + ns(n("m")) + ", " + ns(n("z")) + " teeth, 20° pressure angle, pitch diameter " + mm(n("d")) +
           ", tip " + mm(n("da")) + ", root " + mm(n("df")) + ", face width " + mm(n("width")) +
           (bore != 0 && !std::isnan(bore) ? ", bore " + mm(bore) : "");
  }
  return "";
}

}  // namespace

base::Value part_tags() {
  return obj({{"type", "CC-PART-TYPE"},
              {"std", "CC-PART-STD"},
              {"size", "CC-PART-SIZE"},
              {"length", "CC-PART-LENGTH"},
              {"material", "CC-PART-MATERIAL"}});
}
base::Value cc0() {
  return obj(
      {{"id", "CC0-1.0"}, {"holder", "CadCraft"}, {"url", "https://creativecommons.org/publicdomain/zero/1.0/"}});
}

std::vector<Loop> member_loops(const Section& sec) {
  return outline(sec, 8);
}

base::Result<base::Value> section_item(const Section& sec, const base::Value& options) {
  const base::Value view = options.get("view");
  const base::Value length_v = options.is_object() && options.has("length") && !options.get("length").is_undefined()
                                   ? options.get("length")
                                   : base::Value(3000);
  const std::vector<Loop> loops = outline(sec, 8);
  const Box bb = bbox(loops[0]);
  Drawer d("STEEL");
  const double mass_m = sec.mass_pub ? *sec.mass_pub : exact_area(sec) * 1e-6 * kSteelDensity;
  std::string type = sec.family;
  for (const auto& [f, t] : kFamilyType) {
    if (f == sec.family) {
      type = t;
    }
  }
  if (type.empty()) {
    type = sec.family;
  }
  const SectionSource* src = section_source(sec.source);
  if (!src) {
    return base::fail(base::Errc::invalid, "Cannot read properties of undefined (reading 'title')");
  }
  base::Value attrs =
      obj({{"CC-PART-TYPE", type}, {"CC-PART-STD", src->title}, {"CC-PART-SIZE", sec.name}, {"CC-PART-MATERIAL", ""}});
  const double r = sec.get("r");
  std::string how;
  if (sec.kind == "I" && r != 0 && !std::isnan(r)) {
    how = "";
  } else if (sec.kind == "I" || sec.kind == "C") {
    how = " Drawn with parallel flanges and square corners.";
  } else if (sec.kind == "L") {
    how = " Drawn with square corners.";
  }
  if (view.is_string() && view.as_string() == "elevation") {
    const double lm = base::js_to_number(length_v);
    if (!(lm > 0 && lm <= 1e6)) {  // NOLINT(readability-simplify-boolean-expr): NaN must be refused too
      return base::fail(base::Errc::invalid, "length out of range");
    }
    const double h = bb.h / 2;
    const double e = js_min(lm * 0.04, js_max(10, h * 0.5));
    d.poly({{0, -h}, {lm, -h}, {lm, h}, {0, h}});
    if (sec.kind == "I" || sec.kind == "C") {
      for (const double s : {1.0, -1.0}) {
        d.line(0, s * (h - sec.get("tf")), lm, s * (h - sec.get("tf")));
      }
    }
    if (sec.kind == "L") {
      d.line(0, -h + sec.get("t"), lm, -h + sec.get("t"));
    }
    if (sec.kind == "RHS" || sec.kind == "CHS") {
      for (const double s : {1.0, -1.0}) {
        d.line(0, s * (h - sec.get("t")), lm, s * (h - sec.get("t")), "HIDDEN");
      }
    }
    d.line(-e, 0, lm + e, 0, "CENTER");
    attrs.append("CC-PART-LENGTH", ns(r1(lm)));
    return item(sec.name + " elevation, " + num_text(lm) + " mm",
                sec.name + " (" + type + ") from the " + std::string(src->title) + ": " + sec_dims(sec) + "; " +
                    ns(kgm(mass_m)) + " kg/m. Side elevation, " + num_text(lm) +
                    " mm long, from the middle of the left end." + how,
                obj({{"layers", layers("STEEL")}, {"entities", std::move(d.ents)}}),
                obj({{"width", r1(lm)},
                     {"depth", r1(bb.h)},
                     {"weight_kg", base::js_round(mass_m * lm / 1000 * 100) / 100},
                     {"remarks", ns(kgm(mass_m)) + " kg/m"}}),
                std::move(attrs));
  }
  d.fill(loops);
  for (const auto& l : loops) {
    d.poly_loop(l);
  }
  d.cross(0, 0, bb.w / 2 + js_max(5, bb.w * 0.12), bb.h / 2 + js_max(5, bb.h * 0.12));
  return item(sec.name + " section",
              sec.name + " (" + type + ") from the " + std::string(src->title) + ": " + sec_dims(sec) + "; " +
                  ns(kgm(mass_m)) + " kg/m. Cross-section at its centre." + how,
              obj({{"layers", layers("STEEL")}, {"entities", std::move(d.ents)}}),
              obj({{"width", r1(bb.w)}, {"depth", r1(bb.h)}, {"remarks", ns(kgm(mass_m)) + " kg/m"}}),
              std::move(attrs));
}

base::Value part_item(const base::Value& p) {
  const PartFamily* fam = find_family(p.get("family").as_string());
  const std::string_view how = draw_of(p.get("type").as_string());
  Drawer d("PART");
  const base::Value dims = p.get("dims");
  if (how == "bolt") {
    draw_bolt(d, dims);
  } else if (how == "nut") {
    draw_nut(d, dims);
  } else if (how == "washer") {
    draw_washer(d, dims);
  } else if (how == "pin") {
    draw_pin(d, dims);
  } else if (how == "bearing") {
    draw_bearing(d, dims);
  } else if (how == "key") {
    draw_key(d, dims);
  } else if (how == "gear") {
    draw_gear(d, dims);
  }
  base::Value attrs = obj({{"CC-PART-TYPE", p.get("name")},
                           {"CC-PART-STD", p.get("standard")},
                           {"CC-PART-SIZE", p.get("sizeText")},
                           {"CC-PART-MATERIAL", p.get("material")}});
  const double l = dim(dims, "l");
  if (l != 0 && !std::isnan(l)) {
    attrs.append("CC-PART-LENGTH", ns(r1(l)));
  }
  const base::Value box = p.get("box");
  base::Value params = obj({{"width", r1(dim(box, "w"))}, {"depth", r1(dim(box, "d"))}, {"height", r1(dim(box, "h"))}});
  const base::Value mass = p.get("massKg");
  if (mass.truthy()) {
    params.append("weight_kg", mass);
  }
  std::string ref;
  if (p.get("type").as_string() == "gear") {
    ref = "Modules from ISO 54:1996, tooth form from the ISO 53:1998 basic rack.";
  } else {
    const std::string title = p.get("standardTitle").as_string();
    const std::size_t s1 = title.find(' ');
    const std::size_t s2 = s1 == std::string::npos ? s1 : title.find(' ', s1 + 1);
    ref = "Dimensions from " + title.substr(0, s2) + ".";
  }
  const std::string fname(fam->name);
  std::string description =
      fname + ", " + part_dims(p) + ". " + std::string(view_text(how)) + " " + ref +
      (mass.truthy() ? " Weight from the nominal shape in steel (7850 kg/m³), threads not taken off." : "");
  return item(fname + " " + p.get("designation").as_string(), utf16_prefix(description, 500),
              obj({{"layers", layers("PART")}, {"entities", std::move(d.ents)}}), std::move(params), std::move(attrs));
}

base::Value local_item(const base::Value& it) {
  base::Value out = it;
  const base::Value attrs = it.get("source_attrs");
  base::Value src = attrs.is_object() ? attrs.get("CC-PART-STD") : base::Value{};
  const std::string s = src.truthy() ? base::js_string(src) : "";
  base::Value lic = cc0();
  lic.append("attribution", "“" + base::js_string(it.get("name")) + "” by CadCraft" +
                                (!s.empty() ? ", dimensions from " + s : "") + ", CC0 1.0");
  out.set("license", std::move(lic));
  out.set("provenance", obj({{"library", "standard"}}));
  return out;
}

}  // namespace cc::shapes
