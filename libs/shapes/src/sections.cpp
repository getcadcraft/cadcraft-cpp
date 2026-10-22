// SPDX-License-Identifier: Apache-2.0
// lib/shapes/sections.js: the section table and its geometry.
#include "cc/shapes/sections.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "jsnum.hpp"

namespace cc::shapes {
namespace {

template <std::size_t W>
struct SecRow {
  const char* name;
  std::array<double, W> v;
};
#include "sections_data.inc"

constexpr std::array<SectionSource, 2> kSources = {{
    {"aisc", "AISC Shapes Database v16.0", "American Institute of Steel Construction", 2023, "in",
     "https://www.aisc.org/publications/steel-construction-manual-resources/16th-ed-steel-construction-manual/"
     "aisc-shapes-database-v16.0/"},
    {"en", "EN 10365:2017", "CEN (European Committee for Standardization)", 2017, "mm", ""},
}};
constexpr std::array<SectionFamily, 10> kFamilies = {{
    {"W", "aisc", "I"},
    {"S", "aisc", "I"},
    {"C", "aisc", "C"},
    {"L", "aisc", "L"},
    {"HSS", "aisc", "RHS"},
    {"Pipe", "aisc", "CHS"},
    {"IPE", "en", "I"},
    {"HEA", "en", "I"},
    {"HEB", "en", "I"},
    {"UPN", "en", "C"},
}};

using Pairs = std::vector<std::pair<std::string, double>>;

Section make(std::string name, std::string family, std::string kind, std::string source, Pairs dims, double area,
             double mass, std::string units, Pairs pub) {
  return Section{.name = std::move(name),
                 .family = std::move(family),
                 .kind = std::move(kind),
                 .source = std::move(source),
                 .dims = std::move(dims),
                 .area_pub = area,
                 .mass_pub = mass,
                 .pub_units = std::move(units),
                 .pub = std::move(pub)};
}

template <class T, class F>
void reversed(const T& table, const F& f) {
  for (auto it = table.rbegin(); it != table.rend(); ++it) {
    f(*it);
  }
}

std::vector<Section> build() {
  std::vector<Section> out;
  constexpr double IN = kIn;
  reversed(kAISC_W, [&](const SecRow<7>& r) {
    const auto [d, bf, tw, tf, k, A, W] = r.v;
    out.push_back(make(display_name(r.name), "W", "I", "aisc",
                       {{"d", d * IN}, {"bf", bf * IN}, {"tw", tw * IN}, {"tf", tf * IN}, {"r", (k - tf) * IN}},
                       A * IN * IN, W * kLbFt, "in",
                       {{"d", d}, {"bf", bf}, {"tw", tw}, {"tf", tf}, {"kdes", k}, {"A", A}, {"W", W}}));
  });
  reversed(kAISC_C, [&](const SecRow<6>& r) {
    const auto [d, bf, tw, tf, A, W] = r.v;
    out.push_back(make(display_name(r.name), "C", "C", "aisc",
                       {{"d", d * IN}, {"bf", bf * IN}, {"tw", tw * IN}, {"tf", tf * IN}, {"r", 0}}, A * IN * IN,
                       W * kLbFt, "in", {{"d", d}, {"bf", bf}, {"tw", tw}, {"tf", tf}, {"A", A}, {"W", W}}));
  });
  reversed(kAISC_L, [&](const SecRow<4>& r) {
    const auto [b, t, A, W] = r.v;
    out.push_back(make(display_name(r.name), "L", "L", "aisc", {{"b", b * IN}, {"t", t * IN}}, A * IN * IN, W * kLbFt,
                       "in", {{"b", b}, {"t", t}, {"A", A}, {"W", W}}));
  });
  reversed(kAISC_RHS, [&](const SecRow<6>& r) {
    const auto [H, B, tn, t, A, W] = r.v;
    out.push_back(
        make(display_name(r.name), "HSS", "RHS", "aisc",
             {{"h", H * IN}, {"b", B * IN}, {"t", t * IN}, {"tNom", tn * IN}, {"ro", 2 * t * IN}, {"ri", t * IN}},
             A * IN * IN, W * kLbFt, "in", {{"H", H}, {"B", B}, {"tNom", tn}, {"t", t}, {"A", A}, {"W", W}}));
  });
  reversed(kAISC_CHS, [&](const SecRow<5>& r) {
    const auto [D, tn, t, A, W] = r.v;
    out.push_back(make(display_name(r.name), "HSS", "CHS", "aisc", {{"D", D * IN}, {"t", t * IN}, {"tNom", tn * IN}},
                       A * IN * IN, W * kLbFt, "in", {{"D", D}, {"tNom", tn}, {"t", t}, {"A", A}, {"W", W}}));
  });
  for (const auto& r : kEN_I) {
    const auto [h, b, tw, tf, rr, A, G] = r.v;
    const std::string n = r.name;
    out.push_back(make(n, n.substr(0, 3), "I", "en", {{"d", h}, {"bf", b}, {"tw", tw}, {"tf", tf}, {"r", rr}}, A * 100,
                       G, "mm", {{"h", h}, {"b", b}, {"tw", tw}, {"tf", tf}, {"r", rr}, {"A", A}, {"G", G}}));
  }
  reversed(kAISC_S, [&](const SecRow<7>& r) {
    const auto [d, bf, tw, tf, k, A, W] = r.v;
    out.push_back(make(display_name(r.name), "S", "I", "aisc",
                       {{"d", d * IN}, {"bf", bf * IN}, {"tw", tw * IN}, {"tf", tf * IN}, {"r", 0}}, A * IN * IN,
                       W * kLbFt, "in",
                       {{"d", d}, {"bf", bf}, {"tw", tw}, {"tf", tf}, {"kdes", k}, {"A", A}, {"W", W}}));
  });
  reversed(kAISC_L2, [&](const SecRow<5>& r) {
    const auto [lng, shrt, t, A, W] = r.v;
    out.push_back(make(display_name(r.name), "L", "L", "aisc", {{"b", shrt * IN}, {"h", lng * IN}, {"t", t * IN}},
                       A * IN * IN, W * kLbFt, "in", {{"long", lng}, {"short", shrt}, {"t", t}, {"A", A}, {"W", W}}));
  });
  std::vector<const SecRow<5>*> pipes;
  pipes.reserve(kAISC_PIPE.size());
  for (const auto& r : kAISC_PIPE) {
    pipes.push_back(&r);
  }
  std::ranges::stable_sort(pipes, [](const SecRow<5>* x, const SecRow<5>* y) {  // by OD, then wall
    return x->v[0] < y->v[0] || (x->v[0] == y->v[0] && x->v[1] < y->v[1]);
  });
  for (const auto* r : pipes) {
    const auto [D, tn, t, A, W] = r->v;
    out.push_back(make(r->name, "Pipe", "CHS", "aisc", {{"D", D * IN}, {"t", t * IN}, {"tNom", tn * IN}}, A * IN * IN,
                       W * kLbFt, "in", {{"D", D}, {"tNom", tn}, {"t", t}, {"A", A}, {"W", W}}));
  }
  for (const auto& r : kEN_UPN) {
    const auto [h, b, tw, tf, r1, r2, A, G] = r.v;
    out.push_back(make(r.name, "UPN", "C", "en", {{"d", h}, {"bf", b}, {"tw", tw}, {"tf", tf}, {"r", 0}}, A * 100, G,
                       "mm", {{"h", h}, {"b", b}, {"tw", tw}, {"tf", tf}, {"r1", r1}, {"r2", r2}, {"A", A}, {"G", G}}));
  }
  return out;
}

struct Table {
  std::vector<Section> rows = build();
  std::map<std::string, std::size_t, std::less<>> by_name;
  Table() {
    for (std::size_t i = 0; i < rows.size(); ++i) {
      by_name[base::js_to_upper(rows[i].name)] = i;  // a later row of the same name wins, as Map construction does
    }
  }
};
const Table& table() {
  static const Table t;
  return t;
}

bool word_digit(char c) {
  return c >= '0' && c <= '9';
}

}  // namespace

double Section::get(std::string_view key) const noexcept {
  for (const auto& [k, v] : dims) {
    if (k == key) {
      return v;
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}
bool Section::has(std::string_view key) const noexcept {
  return std::ranges::any_of(dims, [&](const auto& kv) { return kv.first == key; });
}

std::span<const SectionSource> section_sources() {
  return kSources;
}
const SectionSource* section_source(std::string_view id) {
  for (const auto& s : kSources) {
    if (s.id == id) {
      return &s;
    }
  }
  return nullptr;
}
std::span<const SectionFamily> section_families() {
  return kFamilies;
}

std::string display_name(std::string_view n) {
  std::string out(n);
  for (std::size_t i = 1; i + 1 < n.size(); ++i) {
    const char p = n[i - 1];
    if (n[i] == 'X' && (word_digit(p) || p == '/' || p == '.') && word_digit(n[i + 1])) {
      out[i] = 'x';
    }
  }
  return out;
}

const std::vector<Section>& section_rows() {
  return table().rows;
}
const Section* find_section(std::string_view name) {
  const auto& t = table();
  const auto it = t.by_name.find(base::js_to_upper(base::js_trim(name)));
  return it == t.by_name.end() ? nullptr : &t.rows[it->second];
}
std::vector<const Section*> family_rows(std::string_view family) {
  std::vector<const Section*> out;
  for (const auto& s : section_rows()) {
    if (s.family == family) {
      out.push_back(&s);
    }
  }
  return out;
}

double exact_area(const Section& p) {
  constexpr double PI = std::numbers::pi;
  const auto g = [&](std::string_view k) { return p.get(k); };
  if (p.kind == "I") {
    return 2 * g("bf") * g("tf") + (g("d") - 2 * g("tf")) * g("tw") + (4 - PI) * g("r") * g("r");
  }
  if (p.kind == "C") {
    return 2 * g("bf") * g("tf") + (g("d") - 2 * g("tf")) * g("tw");
  }
  if (p.kind == "L") {
    return g("t") * (g("b") + (p.has("h") ? g("h") : g("b")) - g("t"));
  }
  if (p.kind == "RHS") {
    return 2 * g("t") * (g("h") + g("b") - 2 * g("t")) - (4 - PI) * (g("ro") * g("ro") - g("ri") * g("ri"));
  }
  if (p.kind == "CHS") {
    return PI * (g("D") - g("t")) * g("t");
  }
  if (p.kind == "PL") {
    return g("t") * g("b");
  }
  return 0;
}

namespace {

void arc(Loop& pts, double cx, double cy, double r, double a0, double a1, double n) {
  for (std::int64_t k = 1; static_cast<double>(k) < n; ++k) {  // n may be fractional, as in the JS
    const auto i = static_cast<double>(k);
    const double a = a0 + (a1 - a0) * i / n;
    pts.push_back({cx + r * base::js_cos(a), cy + r * base::js_sin(a)});
  }
}

Loop rounded_rect(double w, double h, double r, double seg, bool cw) {
  constexpr double PI = std::numbers::pi;
  const double a = w / 2;
  const double b = h / 2;
  Loop pts;
  const std::array<std::array<double, 3>, 4> corners = {
      {{a - r, -b + r, -PI / 2}, {a - r, b - r, 0}, {-a + r, b - r, PI / 2}, {-a + r, -b + r, PI}}};
  for (const auto& [cx, cy, a0] : corners) {
    for (std::int64_t k = 0; static_cast<double>(k) <= seg; ++k) {
      const auto i = static_cast<double>(k);
      const double t = a0 + (PI / 2) * i / seg;
      pts.push_back({cx + r * base::js_cos(t), cy + r * base::js_sin(t)});
    }
  }
  if (cw) {
    std::ranges::reverse(pts);
  }
  return pts;
}

}  // namespace

std::vector<Loop> outline(const Section& p, double seg) {
  constexpr double PI = std::numbers::pi;
  constexpr double H = PI / 2;
  const auto g = [&](std::string_view k) { return p.get(k); };
  if (p.kind == "I") {
    const double d = g("d");
    const double bf = g("bf");
    const double tw = g("tw");
    const double tf = g("tf");
    const double r = g("r");
    const double a = bf / 2;
    const double w = tw / 2;
    const double h = d / 2;
    Loop P = {{-a, -h}, {a, -h}, {a, -h + tf}};
    if (r > 0) {
      P.push_back({w + r, -h + tf});
      arc(P, w + r, -h + tf + r, r, -H, -2 * H, seg);
      P.push_back({w, -h + tf + r});
      P.push_back({w, h - tf - r});
      arc(P, w + r, h - tf - r, r, 2 * H, H, seg);
      P.push_back({w + r, h - tf});
    } else {
      P.push_back({w, -h + tf});
      P.push_back({w, h - tf});
    }
    P.push_back({a, h - tf});
    P.push_back({a, h});
    P.push_back({-a, h});
    P.push_back({-a, h - tf});
    if (r > 0) {
      P.push_back({-w - r, h - tf});
      arc(P, -w - r, h - tf - r, r, H, 0, seg);
      P.push_back({-w, h - tf - r});
      P.push_back({-w, -h + tf + r});
      arc(P, -w - r, -h + tf + r, r, 0, -H, seg);
      P.push_back({-w - r, -h + tf});
    } else {
      P.push_back({-w, h - tf});
      P.push_back({-w, -h + tf});
    }
    P.push_back({-a, -h + tf});
    return {P};
  }
  if (p.kind == "C") {
    const double d = g("d");
    const double bf = g("bf");
    const double tw = g("tw");
    const double tf = g("tf");
    const double a = bf / 2;
    const double h = d / 2;
    return {Loop{{-a, -h}, {a, -h}, {a, -h + tf}, {-a + tw, -h + tf}, {-a + tw, h - tf}, {a, h - tf}, {a, h}, {-a, h}}};
  }
  if (p.kind == "L") {
    const double b = g("b");
    const double t = g("t");
    const double a = b / 2;
    const double c = (p.has("h") ? g("h") : b) / 2;
    return {Loop{{-a, -c}, {a, -c}, {a, -c + t}, {-a + t, -c + t}, {-a + t, c}, {-a, c}}};
  }
  if (p.kind == "RHS") {
    const double h = g("h");
    const double b = g("b");
    const double t = g("t");
    const double ro = g("ro");
    const double ri = g("ri");
    return {rounded_rect(b, h, ro, seg, false), rounded_rect(b - 2 * t, h - 2 * t, ri, seg, true)};
  }
  if (p.kind == "CHS") {
    const double n = seg * 8;
    const double R = g("D") / 2;
    const double r = R - g("t");
    Loop o;
    Loop i;
    for (std::int64_t kk = 0; static_cast<double>(kk) < n; ++kk) {
      const auto k = static_cast<double>(kk);
      const double a = 2 * PI * k / n;
      o.push_back({R * base::js_cos(a), R * base::js_sin(a)});
      i.push_back({r * base::js_cos(-a), r * base::js_sin(-a)});
    }
    return {o, i};
  }
  if (p.kind == "PL") {
    const double a = g("b") / 2;
    const double c = g("t") / 2;
    return {Loop{{-a, -c}, {a, -c}, {a, c}, {-a, c}}};
  }
  return {Loop{}};
}

Box bbox(const Loop& loop) {
  constexpr double inf = std::numeric_limits<double>::infinity();
  double x0 = inf;
  double y0 = inf;
  double x1 = -inf;
  double y1 = -inf;
  for (const auto& [x, y] : loop) {
    x0 = js_min(x0, x);
    y0 = js_min(y0, y);
    x1 = js_max(x1, x);
    y1 = js_max(y1, y);
  }
  return {x0, y0, x1, y1, x1 - x0, y1 - y0};
}
double shoelace(const Loop& loop) {
  double s = 0;
  for (std::size_t i = 0; i < loop.size(); i++) {
    const auto& [x1, y1] = loop[i];
    const auto& [x2, y2] = loop[(i + 1) % loop.size()];
    s += x1 * y2 - x2 * y1;
  }
  return s / 2;
}
double loop_length(const Loop& loop) {
  double s = 0;
  for (std::size_t i = 0; i < loop.size(); i++) {
    const auto& [x1, y1] = loop[i];
    const auto& [x2, y2] = loop[(i + 1) % loop.size()];
    s += base::js_hypot(x2 - x1, y2 - y1);
  }
  return s;
}

base::Value to_value(const Section& s) {
  base::Value o = base::Value::object();
  o.append("name", s.name);
  o.append("family", s.family);
  o.append("kind", s.kind);
  o.append("source", s.source);
  for (const auto& [k, v] : s.dims) {
    o.set(k, v);
  }
  if (s.area_pub) {
    o.set("areaPub", *s.area_pub);
  }
  if (s.mass_pub) {
    o.set("massPub", *s.mass_pub);
  }
  if (!s.pub_units.empty()) {
    base::Value pub = base::Value::object();
    pub.append("units", s.pub_units);
    for (const auto& [k, v] : s.pub) {
      pub.set(k, v);
    }
    o.set("pub", std::move(pub));
  }
  return o;
}

Section from_value(const base::Value& v) {
  Section s;
  if (!v.is_object() || v.is_point()) {
    return s;
  }
  for (const base::Member* m : base::js_members(v.as_object())) {
    if (m->value.is_string()) {
      if (m->key == "name") {
        s.name = m->value.as_string();
      } else if (m->key == "family") {
        s.family = m->value.as_string();
      } else if (m->key == "kind") {
        s.kind = m->value.as_string();
      } else if (m->key == "source") {
        s.source = m->value.as_string();
      }
    } else if (m->value.is_number()) {
      if (m->key == "areaPub") {
        s.area_pub = m->value.as_number();
      } else if (m->key == "massPub") {
        s.mass_pub = m->value.as_number();
      } else {
        s.dims.emplace_back(m->key, m->value.as_number());
      }
    }
  }
  return s;
}

base::Value loops_value(const std::vector<Loop>& loops) {
  base::Array out;
  out.reserve(loops.size());
  for (const auto& l : loops) {
    base::Array pts;
    pts.reserve(l.size());
    for (const auto& [x, y] : l) {
      pts.emplace_back(base::Array{x, y});
    }
    out.emplace_back(std::move(pts));
  }
  return out;
}
base::Value box_value(const Box& b) {
  return base::make_object({{"x0", b.x0}, {"y0", b.y0}, {"x1", b.x1}, {"y1", b.y1}, {"w", b.w}, {"h", b.h}});
}
base::Value sources_value() {
  base::Value o = base::Value::object();
  for (const auto& s : kSources) {
    o.append(
        s.id,
        base::make_object(
            {{"title", s.title}, {"publisher", s.publisher}, {"year", s.year}, {"units", s.units}, {"url", s.url}}));
  }
  return o;
}
base::Value families_value() {
  base::Array out;
  for (const auto& f : kFamilies) {
    out.emplace_back(base::make_object({{"id", f.id}, {"source", f.source}, {"kind", f.kind}}));
  }
  return out;
}

}  // namespace cc::shapes
