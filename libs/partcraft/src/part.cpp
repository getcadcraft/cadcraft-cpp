// SPDX-License-Identifier: Apache-2.0
// part.js (PartCraft 0.5): the part document — features, their references and order, the file and its reader.
#include "cc/partcraft/part.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/base/json.hpp"
#include "cc/partcraft/bom.hpp"
#include "cc/partcraft/configs.hpp"
#include "cc/partcraft/form.hpp"
#include "cc/partcraft/gdt.hpp"
#include "cc/partcraft/params_part.hpp"
#include "cc/partcraft/sketch.hpp"
#include "cc/partcraft/threads.hpp"
#include "jsx.hpp"

namespace cc::partcraft::part {
namespace {

using base::Value;

/// A JS throw inside the reader: its message.
struct Thrown : std::runtime_error {
  using std::runtime_error::runtime_error;
};
[[noreturn]] void type_error(const Value& of, std::string_view key) {
  throw Thrown(std::string("Cannot read properties of ") + (of.is_null() ? "null" : "undefined") + " (reading '" +
               std::string(key) + "')");
}

bool proto_name(std::string_view k) {
  constexpr std::array<std::string_view, 12> kProto{
      "constructor",      "hasOwnProperty",   "isPrototypeOf",    "propertyIsEnumerable",
      "toLocaleString",   "toString",         "valueOf",          "__proto__",
      "__defineGetter__", "__defineSetter__", "__lookupGetter__", "__lookupSetter__"};
  return std::ranges::find(kProto, k) != kProto.end();
}

// FEATURE_LABELS' keys in their order: part.js's own, then design-features, surface, bodies, sheet metal, rib, thread
const std::vector<std::string_view>& feature_types() {
  static const std::vector<std::string_view> t = {
      "sketch",   "extrude",   "revolve",     "hole",      "lpattern",    "cpattern",   "mirror",   "fillet", "chamfer",
      "shell",    "import",    "plane",       "axis",      "point",       "sweep",      "loft",     "draft",  "form",
      "patch",    "surfloft",  "stitch",      "thicken",   "offsetsurf",  "extendsurf", "trimsurf", "gordon", "combine",
      "movebody", "scalebody", "pathpattern", "sheetbase", "sheetflange", "rib",        "thread"};
  return t;
}
bool known_type(const Value& t) {
  const std::string k = base::js_string(t);
  return std::ranges::find(feature_types(), k) != feature_types().end();
}
/// FEATURE_LABELS[type] truthy: a type, or a name every object inherits.
bool label_truthy(const Value& t) {
  return known_type(t) || proto_name(base::js_string(t));
}
bool surface_type(std::string_view t) {
  constexpr std::array<std::string_view, 9> kSurf{"form",       "patch",      "surfloft", "stitch", "thicken",
                                                  "offsetsurf", "extendsurf", "trimsurf", "gordon"};
  return std::ranges::find(kSurf, t) != kSurf.end();
}
bool surface_label(const Value& t) {
  return surface_type(base::js_string(t)) || proto_name(base::js_string(t));
}

std::string base_name(std::string_view type) {
  static const std::map<std::string_view, std::string_view> kBase = {{"sketch", "Sketch"},
                                                                     {"extrude", "Extrude"},
                                                                     {"revolve", "Revolve"},
                                                                     {"hole", "Hole"},
                                                                     {"lpattern", "LPattern"},
                                                                     {"cpattern", "CirPattern"},
                                                                     {"mirror", "Mirror"},
                                                                     {"fillet", "Fillet"},
                                                                     {"chamfer", "Chamfer"},
                                                                     {"shell", "Shell"},
                                                                     {"import", "Imported"},
                                                                     {"plane", "Plane"},
                                                                     {"axis", "Axis"},
                                                                     {"point", "Point"},
                                                                     {"sweep", "Sweep"},
                                                                     {"loft", "Loft"},
                                                                     {"draft", "Draft"},
                                                                     {"form", "Form"},
                                                                     {"patch", "Patch"},
                                                                     {"surfloft", "LoftSurface"},
                                                                     {"stitch", "Stitch"},
                                                                     {"thicken", "Thicken"},
                                                                     {"offsetsurf", "OffsetSurface"},
                                                                     {"extendsurf", "Extend"},
                                                                     {"trimsurf", "Trim"},
                                                                     {"gordon", "Gordon"},
                                                                     {"combine", "Combine"},
                                                                     {"movebody", "Move"},
                                                                     {"scalebody", "Scale"},
                                                                     {"pathpattern", "PathPattern"},
                                                                     {"sheetbase", "BaseFlange"},
                                                                     {"sheetflange", "EdgeFlange"},
                                                                     {"rib", "Rib"},
                                                                     {"thread", "Thread"}};
  const auto it = kBase.find(type);
  return it != kBase.end() ? std::string(it->second) : std::string("undefined");
}

Value obj(std::initializer_list<base::Init> m) {
  return base::make_object(m);
}
Value plane_base(std::string_view n) {
  return obj({{"kind", "base"}, {"name", n}});
}

Value defaults(std::string_view type) {
  if (type == "sketch") {
    return obj({{"plane", plane_base("front")}, {"sketch", sketch::new_sketch()}});
  }
  if (type == "extrude") {
    return obj({{"sketch", nullptr}, {"op", "add"}, {"end", "blind"}, {"depth", 10}, {"reverse", false}});
  }
  if (type == "revolve") {
    return obj({{"sketch", nullptr}, {"op", "add"}, {"axis", nullptr}, {"angle", 360}});
  }
  if (type == "hole") {
    return obj({{"sketch", nullptr},
                {"kind", "simple"},
                {"d", 6},
                {"depth", 10},
                {"through", true},
                {"cboreD", 11},
                {"cboreDepth", 6},
                {"reverse", false}});
  }
  if (type == "lpattern") {
    return obj({{"seeds", base::Array{}},
                {"dir", obj({{"kind", "world"}, {"axis", "x"}})},
                {"count", 3},
                {"spacing", 20},
                {"reverse", false}});
  }
  if (type == "cpattern") {
    return obj({{"seeds", base::Array{}},
                {"axis", obj({{"kind", "world"}, {"axis", "y"}})},
                {"count", 6},
                {"angle", 360},
                {"reverse", false}});
  }
  if (type == "mirror") {
    return obj({{"seeds", base::Array{}}, {"plane", plane_base("right")}});
  }
  if (type == "fillet") {
    return obj({{"edges", base::Array{}}, {"r", 2}});
  }
  if (type == "chamfer") {
    return obj({{"edges", base::Array{}}, {"d", 1}});
  }
  if (type == "shell") {
    return obj({{"faces", base::Array{}}, {"t", 2}, {"outward", false}});
  }
  if (type == "import") {
    return obj({{"file", ""}, {"step", ""}, {"up", "z"}});
  }
  if (type == "plane") {
    return obj({{"mode", "offset"},
                {"planes", base::Array{plane_base("top")}},
                {"d", 20},
                {"angle", 30},
                {"axis", nullptr},
                {"points", base::Array{}},
                {"face", nullptr},
                {"edge", nullptr},
                {"flip", false}});
  }
  if (type == "axis") {
    return obj({{"mode", "planes"},
                {"planes", base::Array{plane_base("front"), plane_base("right")}},
                {"points", base::Array{}},
                {"face", nullptr},
                {"edge", nullptr}});
  }
  if (type == "point") {
    return obj({{"mode", "xyz"}, {"points", base::Array{}}, {"edge", nullptr}, {"p", base::Array{0, 0, 0}}});
  }
  if (type == "sweep") {
    return obj({{"sketch", nullptr},
                {"path", base::Array{}},
                {"op", "add"},
                {"orient", "corrected"},
                {"binormal", nullptr},
                {"guide", nullptr},
                {"scale", 1}});
  }
  if (type == "loft") {
    return obj({{"sections", base::Array{}}, {"op", "add"}, {"ruled", false}});
  }
  if (type == "draft") {
    return obj({{"faces", base::Array{}}, {"neutral", plane_base("top")}, {"pull", nullptr}, {"angle", 3}});
  }
  if (type == "form") {
    return obj({{"cage", form::box(2, 2, 2, 100, 100, 100)}, {"op", "new"}, {"level", nullptr}});
  }
  if (type == "patch") {
    return obj({{"edges", base::Array{}}, {"sheet", nullptr}, {"at", nullptr}, {"cont", "G0"}});
  }
  if (type == "surfloft") {
    return obj({{"sketches", base::Array{}}});
  }
  if (type == "stitch") {
    return obj({{"sheets", base::Array{}}, {"op", "new"}, {"tol", 0.01}});
  }
  if (type == "thicken") {
    return obj({{"sheet", nullptr}, {"t", 2}, {"side", "out"}, {"op", "new"}});
  }
  if (type == "offsetsurf") {
    return obj({{"sheet", nullptr}, {"d", 5}});
  }
  if (type == "extendsurf") {
    return obj({{"sheet", nullptr}, {"d", 10}, {"sides", base::Array{"umin", "umax", "vmin", "vmax"}}});
  }
  if (type == "trimsurf") {
    return obj({{"sheet", nullptr}, {"tool", nullptr}, {"at", nullptr}, {"keep", "near"}});
  }
  if (type == "gordon") {
    return obj(
        {{"profiles", base::Array{}}, {"guides", base::Array{}}, {"tol", 0.01}, {"solid", false}, {"op", "new"}});
  }
  if (type == "combine") {
    return obj({{"op", "join"}, {"target", nullptr}, {"tools", base::Array{}}, {"keep", false}});
  }
  if (type == "movebody") {
    return obj({{"bodies", base::Array{}},
                {"mode", "translate"},
                {"dx", 0},
                {"dy", 0},
                {"dz", 0},
                {"from", nullptr},
                {"to", nullptr},
                {"axis", obj({{"kind", "world"}, {"axis", "y"}})},
                {"edge", nullptr},
                {"face", nullptr},
                {"angle", 90},
                {"planes", base::Array{}},
                {"flip", false},
                {"copy", false}});
  }
  if (type == "scalebody") {
    return obj(
        {{"bodies", base::Array{}}, {"uniform", true}, {"s", 2}, {"sx", 1}, {"sy", 1}, {"sz", 1}, {"point", nullptr}});
  }
  if (type == "pathpattern") {
    return obj({{"seeds", base::Array{}},
                {"bodies", base::Array{}},
                {"path", base::Array{}},
                {"count", 4},
                {"spacing", "equal"},
                {"d", 20},
                {"orient", "path"},
                {"reverse", false}});
  }
  if (type == "sheetbase") {
    return obj({{"t", 2}, {"r", 2}, {"k", 0.44}, {"depth", 50}, {"reverse", false}});
  }
  if (type == "sheetflange") {
    return obj({{"edge", nullptr},
                {"angle", 90},
                {"length", 20},
                {"flip", false},
                {"offsets", base::Array{0, 0}},
                {"relief", "rect"},
                {"reliefWidth", 2}});
  }
  if (type == "rib") {
    return obj({{"sketch", nullptr}, {"t", 3}, {"side", "mid"}, {"flip", false}});
  }
  if (type == "thread") {
    return obj({{"faces", base::Array{}},
                {"form", "iso"},
                {"size", "auto"},
                {"length", nullptr},
                {"flip", false},
                {"modelled", false},
                {"clearance", 0.1},
                {"hand", "right"}});
  }
  return {};
}

Value* features(Value& part) {
  Value* f = part.is_object() ? part.find("features") : nullptr;
  if (f == nullptr || !f->is_array()) {
    type_error(f == nullptr ? Value{} : *f, "find");
  }
  return f;
}
const base::Array& features_of(const Value& part) {
  const Value* f = part.is_object() ? part.find("features") : nullptr;
  if (f == nullptr || !f->is_array()) {
    type_error(f == nullptr ? Value{} : *f, "find");
  }
  return f->as_array();
}

template <class F>
auto guard(F&& f) -> base::Result<decltype(f())> {
  try {
    return std::forward<F>(f)();
  } catch (const Thrown& e) {
    return base::fail(base::Errc::invalid, e.what());
  } catch (const std::runtime_error& e) {
    return base::fail(base::Errc::invalid, e.what());
  }
}

void add_unique(std::vector<Value>& d, const Value& v) {
  if (std::ranges::find(d, v) == d.end()) {
    d.push_back(v);
  }
}

}  // namespace

Value new_part(const Value& name) {
  return obj({{"format", kFormat},
              {"version", 2},
              {"name", name.is_undefined() ? Value("Part1") : name},
              {"units", "mm"},
              {"resolution", 64},
              {"seq", 1},
              {"rollback", nullptr},
              {"features", base::Array{}}});
}

Value* feature_by_id(Value& part, const Value& id) {
  for (Value& f : features(part)->as_array()) {
    if (f.get("id") == id) {
      return &f;
    }
  }
  return nullptr;
}

double built_count(const Value& part) {
  const auto n = static_cast<double>(features_of(part).size());
  const Value rb = part.get("rollback");
  if (rb.is_null()) {
    return n;
  }
  const double r = base::js_to_number(rb);
  return std::isnan(r) ? r : std::min(r, n);
}

std::vector<std::string> name_features(const Value& name_v) {
  std::vector<std::string> out;
  const std::string name = name_v.truthy() ? base::js_string(name_v) : std::string();
  const auto add = [&](std::string s) {
    if (std::ranges::find(out, s) == out.end()) {
      out.push_back(std::move(s));
    }
  };
  const auto digits = [&](std::size_t i) {
    std::size_t j = i;
    while (j < name.size() && name[j] >= '0' && name[j] <= '9') {
      ++j;
    }
    return j;
  };
  // /^(f\d+)\./
  if (name.size() > 1 && name[0] == 'f') {
    const std::size_t j = digits(1);
    if (j > 1 && j < name.size() && name[j] == '.') {
      add(name.substr(0, j));
    }
  }
  // /[@~](f\d+)/g
  for (std::size_t i = 0; i + 2 < name.size() + 1; ++i) {
    if ((name[i] == '@' || name[i] == '~') && i + 1 < name.size() && name[i + 1] == 'f') {
      const std::size_t j = digits(i + 2);
      if (j > i + 2) {
        add(name.substr(i + 1, j - i - 1));
        i = j - 1;
      }
    }
  }
  return out;
}

base::Result<Value> add_feature(Value& part, const Value& type, const Value& props) {
  return guard([&] {
    const std::string t = base::js_string(type);
    Value def = defaults(t);
    if (def.is_undefined()) {
      throw Thrown("DEFAULTS[type] is not a function");
    }
    const double seq = base::js_to_number(part.get("seq"));
    part.set("seq", seq + 1);
    std::set<std::string> names;
    for (const Value& f : features_of(part)) {
      names.insert(base::js_string(f.get("name")));
    }
    const std::string b = base_name(t);
    double n = 1;
    while (names.contains(b + base::js_string(Value(n)))) {
      n++;
    }
    Value f = obj({{"id", "f" + base::js_string(Value(seq))}, {"type", type}, {"name", b + base::js_string(Value(n))}});
    for (auto& [k, v] : jsx::entries(def)) {
      f.set(k, std::move(v));
    }
    for (auto& [k, v] : jsx::entries(props)) {
      f.set(k, std::move(v));
    }
    const double at = built_count(part);
    auto& list = features(part)->as_array();
    const auto pos = static_cast<std::ptrdiff_t>(std::clamp(at, 0.0, static_cast<double>(list.size())));
    list.insert(list.begin() + pos, f);
    if (!part.get("rollback").is_null()) {
      part.set("rollback", at + 1);
    }
    if (!part.get("rollback").is_null() &&
        base::js_to_number(part.get("rollback")) >= static_cast<double>(list.size())) {
      part.set("rollback", nullptr);
    }
    return f;
  });
}

namespace {

/// design-features.js refDeps
// NOLINTNEXTLINE(misc-no-recursion) — references nest a few levels
void ref_deps(const Value& r, std::vector<Value>& d) {
  if (!r.truthy() || !r.is_object()) {
    return;
  }
  const auto add = [&](const Value& x) {
    if (x.truthy()) {
      add_unique(d, x);
    }
  };
  const auto names = [&](const Value& n) {
    for (const std::string& x : name_features(n)) {
      add(Value(x));
    }
  };
  const Value kind = r.get("kind");
  if (kind == Value("ref") || (kind == Value("face") && r.get("feature").truthy()) || kind == Value("axis")) {
    add(r.get("feature"));
  }
  if (kind == Value("face") && r.get("name").truthy()) {
    names(r.get("name"));
  }
  if (kind == Value("line") || kind == Value("sketch")) {
    add(r.get("sketch"));
  }
  if (kind == Value("vertex")) {
    ref_deps(r.get("edge"), d);
  }
  const Value faces = r.get("faces");
  if (faces.is_array() && std::ranges::all_of(faces.as_array(), [](const Value& n) { return n.is_string(); })) {
    for (const Value& n : faces.as_array()) {
      names(n);
    }
  }
  if (r.get("name").is_string() && !kind.truthy()) {
    names(r.get("name"));
  }
  if (!kind.truthy() && r.get("sketch").is_string()) {
    add(r.get("sketch"));
  }
  if (!kind.truthy() && r.get("edge").truthy()) {
    ref_deps(r.get("edge"), d);
  }
  if (!kind.truthy() && r.get("face").truthy()) {
    ref_deps(r.get("face"), d);
  }
  if (!kind.truthy() && r.get("point").truthy()) {
    ref_deps(r.get("point"), d);
  }
}

std::vector<Value> design_deps(const Value& f) {
  std::vector<Value> d;
  const auto each = [&](const Value& v) {
    if (v.is_array()) {
      for (const Value& r : v.as_array()) {
        ref_deps(r, d);
      }
    } else {
      ref_deps(v, d);
    }
  };
  for (const std::string_view k : {"planes", "points", "path", "guide", "sections", "rails"}) {
    each(f.get(k));
  }
  for (const std::string_view k : {"face", "edge", "upto", "vertex", "neutral", "pull", "binormal"}) {
    each(f.get(k));
  }
  const Value t = f.get("type");
  if (t == Value("plane") || t == Value("axis") || t == Value("point")) {
    each(f.get("axis"));
  }
  if (t == Value("revolve") && f.get("axis").truthy() && f.get("axis").is_object()) {
    each(f.get("axis"));
  }
  return d;
}

std::vector<Value> surface_deps(const Value& f) {
  std::vector<Value> d;
  const Value t = f.get("type");
  if (!surface_label(t)) {
    return d;
  }
  if (f.get("sheet").is_string()) {
    d.push_back(f.get("sheet"));
  }
  if (f.get("sheets").is_array()) {
    for (const Value& s : f.get("sheets").as_array()) {
      d.push_back(s);
    }
  }
  if (f.get("tool").is_string() && !(f.get("tool") == Value("body"))) {
    d.push_back(f.get("tool"));
  }
  if (f.get("sketches").is_array()) {
    for (const Value& s : f.get("sketches").as_array()) {
      d.push_back(s);
    }
  }
  if (t == Value("gordon")) {
    for (const std::string_view k : {"profiles", "guides"}) {
      const Value list = f.get(k);
      if (!list.truthy()) {
        continue;
      }
      for (const Value& c : list.as_array()) {
        if (jsx::get(c, "sketch").truthy()) {
          d.push_back(c.get("sketch"));
        }
        if (jsx::get(c, "edge").truthy()) {
          for (const Value& n : c.get("edge").get("faces").as_array()) {
            for (const std::string& x : name_features(n)) {  // idsIn: the same rule, duplicates kept
              d.emplace_back(x);
            }
          }
        }
      }
    }
    const Value tan = f.get("tangent");
    if (tan.truthy() && tan.is_array()) {
      for (const Value& p : tan.as_array()) {
        if (p.get("kind") == Value("ref")) {
          d.push_back(p.get("feature"));
        }
        if (p.get("kind") == Value("face")) {
          d.push_back(p.get("feature"));
          if (p.get("name").truthy()) {
            for (const std::string& x : name_features(p.get("name"))) {
              d.emplace_back(x);
            }
          }
        }
      }
    }
  }
  return d;
}

bool good_body(const Value& id);
std::vector<std::string> body_features(const Value& id);

std::vector<Value> bodies_deps(const Value& f) {
  std::vector<Value> d;
  std::vector<Value> ids;
  for (const std::string_view k : {"bodies", "tools"}) {
    if (f.get(k).is_array()) {
      for (const Value& b : f.get(k).as_array()) {
        ids.push_back(b);
      }
    }
  }
  if (f.get("target").is_string()) {
    ids.push_back(f.get("target"));
  }
  const std::string t = base::js_string(f.get("type"));
  const bool body_type = t == "combine" || t == "movebody" || t == "scalebody" || t == "pathpattern" || proto_name(t);
  if (body_type || t == "lpattern" || t == "cpattern") {
    for (const Value& b : ids) {
      if (good_body(b)) {
        for (const std::string& x : body_features(b)) {
          add_unique(d, Value(x));
        }
      }
    }
  }
  if (t == "movebody" || t == "scalebody") {
    for (const std::string_view k : {"axis", "from", "to", "point"}) {
      const Value r = f.get(k);
      if (!r.truthy() || !r.is_object()) {
        continue;
      }
      const Value kind = r.get("kind");
      if (kind == Value("ref") || kind == Value("axis")) {
        add_unique(d, r.get("feature"));
      }
      if (kind == Value("line") || kind == Value("sketch")) {
        add_unique(d, r.get("sketch"));
      }
      if (kind == Value("vertex") && r.get("edge").truthy() && r.get("edge").get("faces").is_array()) {
        for (const Value& n : r.get("edge").get("faces").as_array()) {
          const std::string s = base::js_string(n);
          for (std::size_t i = 0; i < s.size(); ++i) {  // /f\d+/g
            if (s[i] == 'f' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') {
              std::size_t j = i + 1;
              while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
                ++j;
              }
              add_unique(d, Value(s.substr(i, j - i)));
              i = j - 1;
            }
          }
        }
      }
    }
  }
  return d;
}

std::vector<Value> sheet_deps(const Value& f) {
  std::vector<Value> d;
  if (f.get("type") == Value("sheetflange") && f.get("edge").truthy() && f.get("edge").get("faces").is_array()) {
    for (const Value& n : f.get("edge").get("faces").as_array()) {
      for (const std::string& x : name_features(n)) {
        d.emplace_back(x);
      }
    }
  }
  return d;
}

}  // namespace

base::Result<std::vector<Value>> depends_on(const Value& f) {
  return guard([&] {
    std::vector<Value> d;
    const auto add = [&](const Value& v) { add_unique(d, v); };
    if (f.get("sketch").is_string()) {
      add(f.get("sketch"));
    }
    const Value t = f.get("type");
    const Value plane = t == Value("sketch") || t == Value("mirror") ? f.get("plane") : Value(nullptr);
    if (plane.truthy() && plane.get("kind") == Value("face")) {
      add(plane.get("feature"));
      if (plane.get("name").truthy()) {
        for (const std::string& x : name_features(plane.get("name"))) {
          add(Value(x));
        }
      }
    }
    if (f.get("edges").is_array()) {
      for (const Value& e : f.get("edges").as_array()) {
        const Value faces = jsx::get(e, "faces");
        if (e.is_null() || e.is_undefined()) {
          type_error(e, "faces");
        }
        if (!faces.is_array()) {
          throw Thrown("e.faces is not iterable");
        }
        for (const Value& n : faces.as_array()) {
          for (const std::string& x : name_features(n)) {
            add(Value(x));
          }
        }
      }
    }
    if (f.get("faces").is_array()) {
      for (const Value& r : f.get("faces").as_array()) {
        if (r.is_null() || r.is_undefined()) {
          type_error(r, "name");
        }
        for (const std::string& x : name_features(r.get("name"))) {
          add(Value(x));
        }
      }
    }
    if (f.get("seeds").is_array()) {
      for (const Value& s : f.get("seeds").as_array()) {
        add(s);
      }
    }
    for (const Value& r :
         {f.get("dir"), f.get("axis").truthy() && f.get("axis").is_object() ? f.get("axis") : Value(nullptr)}) {
      if (!r.truthy()) {
        continue;
      }
      if (r.get("kind") == Value("line")) {
        add(r.get("sketch"));
      }
      if (r.get("kind") == Value("axis")) {
        add(r.get("feature"));
      }
    }
    for (const Value& x : design_deps(f)) {
      add(x);
    }
    for (const Value& x : surface_deps(f)) {
      add(x);
    }
    for (const Value& x : bodies_deps(f)) {
      add(x);
    }
    for (const Value& x : sheet_deps(f)) {
      add(x);
    }
    if (f.get("plane").truthy() && f.get("plane").get("kind") == Value("ref")) {
      add(f.get("plane").get("feature"));
    }
    return d;
  });
}

base::Result<Value> dependents(const Value& part, const Value& id) {
  return guard([&] {
    base::Array out;
    for (const Value& f : features_of(part)) {
      auto d = depends_on(f);
      if (!d) {
        throw Thrown(d.error().message);
      }
      if (std::ranges::find(*d, id) != d->end()) {
        out.push_back(f);
      }
    }
    return Value(std::move(out));
  });
}

base::Result<Value> can_move(const Value& part, const Value& id, double to, const i18n::Catalog& w) {
  return guard([&]() -> Value {
    const auto& fs = features_of(part);
    std::ptrdiff_t from = -1;
    for (std::size_t i = 0; i < fs.size(); ++i) {
      if (fs[i].get("id") == id) {
        from = static_cast<std::ptrdiff_t>(i);
        break;
      }
    }
    if (from < 0 || to < 0 || to >= static_cast<double>(fs.size()) || std::isnan(to)) {
      return obj({{"ok", false}, {"reason", w.t("model.part.move.at_end")}});
    }
    if (to == static_cast<double>(from)) {
      return obj({{"ok", false}, {"reason", w.t("model.part.move.there")}});
    }
    std::vector<const Value*> order;
    for (std::size_t i = 0; i < fs.size(); ++i) {
      if (std::cmp_not_equal(i, from)) {
        order.push_back(&fs[i]);
      }
    }
    const auto at = static_cast<std::size_t>(std::trunc(to));
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(std::min(at, order.size())),
                 &fs[static_cast<std::size_t>(from)]);
    const auto pos = [&](const Value& fid) -> std::optional<std::size_t> {
      std::optional<std::size_t> p;
      for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i]->get("id") == fid) {
          p = i;  // a Map from the list: the last of an id wins
        }
      }
      return p;
    };
    for (const Value* g : order) {
      auto d = depends_on(*g);
      if (!d) {
        throw Thrown(d.error().message);
      }
      for (const Value& dep : *d) {
        const auto pd = pos(dep);
        const auto pg = pos(g->get("id"));
        if (pd && pg && *pd > *pg) {
          const Value* df = nullptr;
          for (const Value& f : fs) {
            if (f.get("id") == dep) {
              df = &f;
              break;
            }
          }
          return obj({{"ok", false},
                      {"reason", w.t("model.part.move.uses", {{"name", base::js_string(g->get("name"))},
                                                              {"dep", base::js_string(df->get("name"))}})}});
        }
      }
    }
    return obj({{"ok", true}});
  });
}

base::Result<Value> move_feature(Value& part, const Value& id, double to, const i18n::Catalog& w) {
  auto ok = can_move(part, id, to, w);
  if (!ok || !ok->get("ok").truthy()) {
    return ok;
  }
  return guard([&] {
    auto& list = features(part)->as_array();
    std::size_t from = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
      if (list[i].get("id") == id) {
        from = i;
        break;
      }
    }
    const double built = built_count(part);
    Value f = std::move(list[from]);
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(from));
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(to), std::move(f));
    if (!part.get("rollback").is_null()) {
      part.set("rollback", built);
    }
    return obj({{"ok", true}});
  });
}

base::Result<Value> delete_features(Value& part, const Value& ids) {
  return guard([&] {
    std::vector<Value> gone;
    if (!ids.is_array()) {
      throw Thrown("ids is not iterable");
    }
    for (const Value& v : ids.as_array()) {
      add_unique(gone, v);
    }
    bool grew = true;
    while (grew) {
      grew = false;
      for (const Value& f : features_of(part)) {
        if (std::ranges::find(gone, f.get("id")) != gone.end()) {
          continue;
        }
        auto d = depends_on(f);
        if (!d) {
          throw Thrown(d.error().message);
        }
        if (std::ranges::any_of(*d, [&](const Value& x) { return std::ranges::find(gone, x) != gone.end(); })) {
          gone.push_back(f.get("id"));
          grew = true;
        }
      }
    }
    const double built = built_count(part);
    const auto& list = features_of(part);
    double before = 0;
    for (std::size_t i = 0; i < list.size() && static_cast<double>(i) < built; ++i) {
      if (std::ranges::find(gone, list[i].get("id")) != gone.end()) {
        before++;
      }
    }
    base::Array names;
    base::Array keep;
    for (const Value& f : list) {
      if (std::ranges::find(gone, f.get("id")) != gone.end()) {
        names.push_back(f.get("name"));
      } else {
        keep.push_back(f);
      }
    }
    const std::size_t n = keep.size();
    part.set("features", std::move(keep));
    if (!part.get("rollback").is_null()) {
      part.set("rollback", built - before);
      if (built - before >= static_cast<double>(n)) {
        part.set("rollback", nullptr);
      }
    }
    return Value(std::move(names));
  });
}

namespace {

bool f_uses_design(const Value& f) {
  const std::string t = base::js_string(f.get("type"));
  const bool label =
      t == "plane" || t == "axis" || t == "point" || t == "sweep" || t == "loft" || t == "draft" || proto_name(t);
  const Value end = f.get("end");
  const bool ends = end == Value("upto") || end == Value("next") || end == Value("vertex") || end == Value("offset");
  const Value kind = f.get("kind");
  const Value mode = f.get("mode");
  const auto refk = [](const Value& d) { return d.truthy() && d.is_object() && d.get("kind") == Value("ref"); };
  return label || ends || f.get("two").truthy() || f.get("taper").truthy() || f.get("regions").truthy() ||
         kind == Value("csink") || kind == Value("tapped") ||
         (mode.truthy() && t == "chamfer" && !(mode == Value("equal"))) || f.get("r2").is_number() ||
         (t == "revolve" && f.get("axis").truthy() && f.get("axis").is_object()) ||
         (f.get("plane").truthy() && f.get("plane").get("kind") == Value("ref")) || refk(f.get("dir")) ||
         refk(f.get("axis"));
}

bool has_keys(const Value& v) {
  return v.is_object() && !jsx::entries(v).empty();
}

bool has_params_key(const Value& part) {
  const Value p = part.get("params");
  if (!p.truthy()) {
    return false;
  }
  const auto nonempty = [](const Value& v) { return v.truthy() && v.is_array() && !v.as_array().empty(); };
  return nonempty(p.get("vars")) || nonempty(p.get("links"));
}
bool has_configs(const Value& part) {
  const Value c = part.get("configs");
  return c.truthy() && c.get("rows").truthy() && c.get("rows").is_array() && !c.get("rows").as_array().empty();
}

}  // namespace

double file_version(const Value& part) {
  const auto& fs = features_of(part);
  const auto any = [&](auto pred) { return std::ranges::any_of(fs, pred); };
  if (part.get("studies").truthy() && has_keys(part.get("studies"))) {
    return 3;
  }
  if (any(f_uses_design)) {
    return 3;
  }
  if (any([](const Value& f) { return surface_label(f.get("type")); })) {
    return 3;
  }
  const bool bodies =
      (part.get("bodyInfo").truthy() && has_keys(part.get("bodyInfo"))) || any([](const Value& f) {
        const std::string t = base::js_string(f.get("type"));
        const bool label = t == "combine" || t == "movebody" || t == "scalebody" || t == "pathpattern" || proto_name(t);
        return label ||
               ((t == "extrude" || t == "revolve") &&
                (f.get("op") == Value("new") || f.get("op") == Value("intersect"))) ||
               (f.get("bodies").is_array() && !f.get("bodies").as_array().empty());
      });
  if (bodies || has_params_key(part)) {
    return 3;
  }
  const Value d = part.get("drawing");
  const bool sheet =
      part.get("material").truthy() ||
      (d.truthy() &&
       ((d.get("sheet").truthy() && !(d.get("sheet") == Value("a3"))) || d.get("title").truthy() ||
        (d.get("revisions").truthy() && d.get("revisions").is_array() && !d.get("revisions").as_array().empty()) ||
        d.get("holeTable").truthy()));
  if (sheet) {
    return 3;
  }
  if (any([](const Value& f) {
        const std::string t = base::js_string(f.get("type"));
        return t == "sheetbase" || t == "sheetflange" || proto_name(t);
      })) {
    return 3;
  }
  if (has_configs(part) || any([](const Value& f) { return f.get("type") == Value("rib"); }) || any([](const Value& f) {
        return f.get("type") == Value("thread") || (f.get("type") == Value("hole") && f.get("modelled").truthy());
      })) {
    return 3;
  }
  return 2;
}

std::string to_json(const Value& part) {
  Value out = Value::object();
  for (auto& [k, v] : jsx::entries(part)) {
    if (k == "params" || k == "configs") {
      continue;
    }
    out.append(k, std::move(v));
  }
  if (has_params_key(part)) {
    out.set("params", part.get("params"));
  }
  if (has_configs(part)) {
    out.set("configs", part.get("configs"));
  }
  out.set("version", file_version(part));
  return jsx::stringify(out, 1);
}

// ---------------------------------------------------------------- reading untrusted files

namespace {

bool num(const Value& v, double lo = -1e9, double hi = 1e9) {
  return v.is_number() && std::isfinite(v.as_number()) && v.as_number() >= lo && v.as_number() <= hi;
}
bool vec3(const Value& v, double lim = 1e9, bool hyp = true) {
  if (!v.is_array() || v.as_array().size() != 3) {
    return false;
  }
  const auto& a = v.as_array();
  if (!std::ranges::all_of(a, [lim](const Value& x) { return num(x, -lim, lim); })) {
    return false;
  }
  return !hyp || base::js_hypot(a[0].as_number(), a[1].as_number(), a[2].as_number()) <= 1e9;
}
bool is_int(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number()) && v.as_number() == std::trunc(v.as_number());
}
bool feature_id(const Value& v) {  // /^f\d{1,9}$/
  if (!v.is_string()) {
    return false;
  }
  const std::string& s = v.as_string();
  return s.size() >= 2 && s.size() <= 10 && s[0] == 'f' &&
         std::ranges::all_of(s.substr(1), [](char c) { return c >= '0' && c <= '9'; });
}
/// String(s ?? '') without control characters, cut to max UTF-16 units.
std::string text(const Value& s, std::size_t max = 120) {
  return jsx::utf16_slice(jsx::strip_control(jsx::text_of(s)), max);
}

/// NAME_RE: /^f\d{1,9}\.[a-z]+(:[A-Za-z0-9_:-]{1,40})?([@~]f\d{1,9}(#(\d{1,4}|m))?){0,24}$/
bool good_name(const Value& v) {
  if (!v.is_string() || base::utf16_length(v.as_string()) > 400) {
    return false;
  }
  const std::string& s = v.as_string();
  std::size_t i = 0;
  const auto digits = [&](std::size_t lo, std::size_t hi) {
    std::size_t n = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && n < hi) {
      ++i;
      ++n;
    }
    if (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      return false;  // more digits than the pattern allows: the next part cannot start with one
    }
    return n >= lo;
  };
  if (i >= s.size() || s[i] != 'f') {
    return false;
  }
  ++i;
  if (!digits(1, 9) || i >= s.size() || s[i] != '.') {
    return false;
  }
  ++i;
  const std::size_t a = i;
  while (i < s.size() && s[i] >= 'a' && s[i] <= 'z') {
    ++i;
  }
  if (i == a) {
    return false;
  }
  if (i < s.size() && s[i] == ':') {
    ++i;
    const std::size_t b = i;
    while (i < s.size() && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
                            (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == ':' || s[i] == '-')) {
      ++i;
    }
    if (i - b < 1 || i - b > 40) {
      return false;
    }
  }
  int parts = 0;
  while (i < s.size()) {
    if ((s[i] != '@' && s[i] != '~') || ++parts > 24) {
      return false;
    }
    ++i;
    if (i >= s.size() || s[i] != 'f') {
      return false;
    }
    ++i;
    if (!digits(1, 9)) {
      return false;
    }
    if (i < s.size() && s[i] == '#') {
      ++i;
      if (i < s.size() && s[i] == 'm') {
        ++i;
      } else if (!digits(1, 4)) {
        return false;
      }
    }
  }
  return true;
}

struct Ctx {
  std::vector<std::string> ids;
  std::vector<std::pair<std::string, std::string>> types;
  const i18n::Catalog& w;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): lives for one read
  [[nodiscard]] bool has(const Value& id) const {
    return id.is_string() && std::ranges::find(ids, id.as_string()) != ids.end();
  }
  [[nodiscard]] std::optional<std::string> type_of(const Value& id) const {
    if (!id.is_string()) {
      return std::nullopt;
    }
    for (const auto& [k, t] : types) {
      if (k == id.as_string()) {
        return t;
      }
    }
    return std::nullopt;
  }
  [[nodiscard]] bool is_type(const Value& id, std::string_view t) const {
    const auto x = type_of(id);
    return x && *x == t;
  }
  [[nodiscard]] bool known(const Value& n) const {
    if (!good_name(n)) {
      return false;
    }
    return std::ranges::all_of(name_features(n), [&](const std::string& x) { return has(Value(x)); });
  }
};

[[noreturn]] void fail(const std::string& m) {
  throw Thrown(m);
}

}  // namespace

static Value read_plane(const Value& p, const Ctx& c, bool typed) {
  if (!p.truthy() || (!p.is_object() && !p.is_array())) {
    fail(c.w.t("model.part.file.plane_missing"));
  }
  const Value kind = p.get("kind");
  const Value feat = p.get("feature");
  if (kind == Value("ref") && c.has(feat) && (!typed || c.is_type(feat, "plane"))) {
    return obj({{"kind", "ref"}, {"feature", feat}});
  }
  const Value name = p.get("name");
  if (kind == Value("base") && (name == Value("front") || name == Value("top") || name == Value("right"))) {
    return obj({{"kind", "base"}, {"name", name}});
  }
  const Value face = p.get("face");
  if (kind == Value("face") && c.has(feat) &&
      (face == Value("start") || face == Value("end") || face == Value("side"))) {
    Value r = obj({{"kind", "face"}, {"feature", feat}, {"face", face}});
    if (face == Value("side")) {
      const Value e = p.get("entity");
      if (!e.is_string() || base::utf16_length(e.as_string()) > 40) {
        fail(c.w.t("model.part.file.face_bad"));
      }
      r.append("entity", e);
    }
    return r;
  }
  if (kind == Value("face") && face == Value("named") && good_name(name)) {
    const auto nf = name_features(name);
    if (!nf.empty() && c.has(Value(nf[0]))) {
      return obj({{"kind", "face"}, {"feature", nf[0]}, {"face", "named"}, {"name", name}});
    }
  }
  if (kind == Value("fixed") && vec3(p.get("o")) && vec3(p.get("n"))) {
    const auto& n = p.get("n").as_array();
    if (base::js_hypot(n[0].as_number(), n[1].as_number(), n[2].as_number()) > 1e-9) {
      return obj({{"kind", "fixed"}, {"o", p.get("o")}, {"n", p.get("n")}});
    }
  }
  fail(c.w.t("model.part.file.plane_bad"));
}

static Value read_dir(const Value& d, const Ctx& c, bool typed) {
  if (!d.truthy() || (!d.is_object() && !d.is_array())) {
    fail(c.w.t("model.part.file.dir_missing"));
  }
  const Value kind = d.get("kind");
  const Value feat = d.get("feature");
  if (kind == Value("ref") && c.has(feat) && (!typed || c.is_type(feat, "axis") || c.is_type(feat, "plane"))) {
    return obj({{"kind", "ref"}, {"feature", feat}});
  }
  const Value axis = d.get("axis");
  if (kind == Value("world") && (axis == Value("x") || axis == Value("y") || axis == Value("z"))) {
    return obj({{"kind", "world"}, {"axis", axis}});
  }
  const Value ent = d.get("entity");
  if (kind == Value("line") && c.has(d.get("sketch")) && ent.is_string() && base::utf16_length(ent.as_string()) <= 40) {
    return obj({{"kind", "line"}, {"sketch", d.get("sketch")}, {"entity", ent}});
  }
  if (kind == Value("axis") && c.has(feat)) {
    return obj({{"kind", "axis"}, {"feature", feat}});
  }
  if (kind == Value("fixed") && vec3(d.get("o")) && vec3(d.get("d"))) {
    const auto& v = d.get("d").as_array();
    if (base::js_hypot(v[0].as_number(), v[1].as_number(), v[2].as_number()) > 1e-9) {
      return obj({{"kind", "fixed"}, {"o", d.get("o")}, {"d", d.get("d")}});
    }
  }
  fail(c.w.t("model.part.file.dir_bad"));
}

namespace {

/// design-features.js makeReaders
struct Readers {
  const Ctx& c;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): lives for one feature's read
  [[nodiscard]] static bool ent(const Value& s) {
    if (!s.is_string()) {
      return false;
    }
    const std::string& x = s.as_string();
    return !x.empty() && base::utf16_length(x) <= 40 && std::ranges::all_of(x, [](char ch) {
      return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
             ch == ':' || ch == '-';
    });
  }
  [[nodiscard]] Value edge_ref(const Value& e) const {
    const Value faces = e.truthy() ? e.get("faces") : Value{};
    const Value at = e.truthy() ? e.get("at") : Value{};
    if (!e.truthy() || !faces.is_array() || faces.as_array().size() != 2 ||
        !std::ranges::all_of(faces.as_array(), [&](const Value& n) { return c.known(n); }) ||
        (!at.is_undefined() && !vec3(at))) {
      fail(c.w.t("model.design_features.ref.an_edge_reference_is_bad"));
    }
    Value r = obj({{"faces", faces}});
    if (at.truthy()) {
      r.append("at", at);
    }
    return r;
  }
  [[nodiscard]] Value face_ref(const Value& f) const {
    const Value at = f.truthy() ? f.get("at") : Value{};
    if (!f.truthy() || !c.known(f.get("name")) || (!at.is_undefined() && !vec3(at))) {
      fail(c.w.t("model.design_features.ref.a_face_reference_is_bad"));
    }
    Value r = obj({{"name", f.get("name")}});
    if (at.truthy()) {
      r.append("at", at);
    }
    return r;
  }
  [[nodiscard]] Value point_ref(const Value& p) const {
    if (!p.truthy() || (!p.is_object() && !p.is_array())) {
      fail(c.w.t("model.design_features.ref.a_point_reference_is_missing"));
    }
    const Value kind = p.get("kind");
    if (kind == Value("sketch") && c.is_type(p.get("sketch"), "sketch") && ent(p.get("entity"))) {
      return obj({{"kind", "sketch"}, {"sketch", p.get("sketch")}, {"entity", p.get("entity")}});
    }
    if (kind == Value("vertex") && vec3(p.get("p"))) {
      return obj({{"kind", "vertex"}, {"edge", edge_ref(p.get("edge"))}, {"p", p.get("p")}});
    }
    if (kind == Value("ref") && c.is_type(p.get("feature"), "point")) {
      return obj({{"kind", "ref"}, {"feature", p.get("feature")}});
    }
    if (kind == Value("fixed") && vec3(p.get("p"))) {
      return obj({{"kind", "fixed"}, {"p", p.get("p")}});
    }
    fail(c.w.t("model.design_features.ref.a_point_reference_is_bad"));
  }
  [[nodiscard]] Value path_item(const Value& it) const {
    if (it.truthy() && it.get("sketch").is_string()) {
      if (!c.is_type(it.get("sketch"), "sketch")) {
        fail(c.w.t("model.design_features.ref.a_path_sketch_is_bad"));
      }
      const Value e = it.get("entities");
      const bool list_ok = e.is_array() && e.as_array().size() <= 500 && std::ranges::all_of(e.as_array(), ent);
      if (!e.is_undefined() && !e.is_null() && !list_ok) {
        fail(c.w.t("model.design_features.ref.a_path_curve_list_is_bad"));
      }
      Value r = obj({{"sketch", it.get("sketch")}});
      if (e.is_array()) {
        r.append("entities", e);
      }
      return r;
    }
    if (it.truthy() && it.get("edge").truthy()) {
      return obj({{"edge", edge_ref(it.get("edge"))}});
    }
    fail(c.w.t("model.design_features.ref.a_path_piece_is_bad"));
  }
  template <class F>
  [[nodiscard]] Value list(const Value& v, const F& fn, std::size_t max, std::string_view key) const {
    if (!v.is_array() || v.as_array().size() > max) {
      fail(c.w.t(key));
    }
    base::Array out;
    for (const Value& x : v.as_array()) {
      out.push_back(fn(x));
    }
    return out;
  }
};

/// `{name}: {problem} (…).` around a reader's own refusal
template <class F>
Value wrap(const F& fn, std::string_view key, const std::string& name, const i18n::Catalog& w) {
  try {
    return fn();
  } catch (const Thrown& e) {
    std::string m = e.what();
    if (!m.empty() && m.back() == '.') {
      m.pop_back();
    }
    fail(w.t(key, {{"name", name}, {"problem", m}}));
  }
}

constexpr std::array<std::string_view, 8> kPlaneModes = {"offset",  "angle",  "3pt",    "mid",
                                                         "tangent", "normal", "edgept", "midnormal"};
constexpr std::array<std::string_view, 5> kAxisModes = {"2pt", "cyl", "planes", "edge", "normal"};
constexpr std::array<std::string_view, 6> kPointModes = {"at", "center", "xyz", "mid", "meet", "planes3"};
constexpr std::array<std::string_view, 4> kOps = {"add", "cut", "intersect", "new"};
constexpr std::array<std::string_view, 5> kOrients = {"frenet", "corrected", "fixed", "binormal", "guide"};

bool among(const Value& v, std::span<const std::string_view> list) {
  return v.is_string() && std::ranges::find(list, v.as_string()) != list.end();
}
bool among(const Value& v, std::initializer_list<std::string_view> list) {
  return among(v, std::span<const std::string_view>(list.begin(), list.size()));
}

bool iso_coarse(const Value& size) {
  static const std::vector<std::string_view> k = {"M2",  "M2.5", "M3",  "M4",  "M5",  "M6",  "M8", "M10",
                                                  "M12", "M14",  "M16", "M18", "M20", "M22", "M24"};
  return among(size, k);
}

/// readDesign(f, g, c): the design features and the new fields of the old ones → true when it handled the type
bool read_design(const Value& f, Value& g, const Ctx& c) {
  const Readers R{c};
  const std::string name = base::js_string(g.get("name"));
  const auto need = [&](bool ok, std::string_view key) {
    if (!ok) {
      fail(c.w.t(key, {{"name", name}}));
    }
  };
  const auto planes = [&](std::size_t n) {
    const Value v =
        f.get("planes").is_null() || f.get("planes").is_undefined() ? Value(base::Array{}) : f.get("planes");
    return wrap(
        [&] {
          return R.list(v, [&](const Value& p) { return read_plane(p, c, true); }, n, "model.file.list.the_plane_list");
        },
        "model.file.in.its_planes", name, c.w);
  };
  const auto points = [&](std::size_t n) {
    const Value v =
        f.get("points").is_null() || f.get("points").is_undefined() ? Value(base::Array{}) : f.get("points");
    return wrap(
        [&] { return R.list(v, [&](const Value& p) { return R.point_ref(p); }, n, "model.file.list.the_point_list"); },
        "model.file.in.its_points", name, c.w);
  };
  const auto opt = [&](const Value& v, auto fn, std::string_view what) -> Value {
    if (v.is_null() || v.is_undefined()) {
      return nullptr;
    }
    return wrap([&] { return fn(v); }, what, name, c.w);
  };
  const auto dir = [&](const Value& a) { return read_dir(a, c, true); };
  const auto face = [&](const Value& a) { return R.face_ref(a); };
  const auto edge = [&](const Value& a) { return R.edge_ref(a); };
  const auto or0 = [&](std::string_view k, double d) {
    return f.get(k).is_null() || f.get(k).is_undefined() ? Value(d) : f.get(k);
  };
  const std::string t = base::js_string(f.get("type"));
  if (t == "plane") {
    need(among(f.get("mode"), kPlaneModes), "model.file.bad.the_plane_type");
    need(num(or0("d", 0)), "model.file.bad.the_distance");
    need(num(or0("angle", 0), -360, 360), "model.file.bad.the_angle");
    g.set("mode", f.get("mode"));
    g.set("planes", planes(2));
    g.set("d", or0("d", 0));
    g.set("angle", or0("angle", 0));
    g.set("axis", opt(f.get("axis"), dir, "model.file.in.its_axis"));
    g.set("points", points(3));
    g.set("face", opt(f.get("face"), face, "model.file.in.its_face"));
    g.set("edge", opt(f.get("edge"), edge, "model.file.in.its_edge"));
    g.set("flip", f.get("flip") == Value(true));
    return true;
  }
  if (t == "axis") {
    need(among(f.get("mode"), kAxisModes), "model.file.bad.the_axis_type");
    g.set("mode", f.get("mode"));
    g.set("planes", planes(2));
    g.set("points", points(2));
    g.set("face", opt(f.get("face"), face, "model.file.in.its_face"));
    g.set("edge", opt(f.get("edge"), edge, "model.file.in.its_edge"));
    return true;
  }
  if (t == "point") {
    need(among(f.get("mode"), kPointModes), "model.file.bad.the_point_type");
    need(f.get("p").is_undefined() || vec3(f.get("p")), "model.file.bad.the_coordinates");
    g.set("mode", f.get("mode"));
    g.set("points", points(2));
    g.set("edge", opt(f.get("edge"), edge, "model.file.in.its_edge"));
    g.set("p", f.get("p").truthy() ? f.get("p") : Value(base::Array{0, 0, 0}));
    if (!f.get("planes").is_undefined() && !f.get("planes").is_null()) {
      g.set("planes", planes(3));
    }
    if (!f.get("axis").is_undefined() && !f.get("axis").is_null()) {
      g.set("axis", opt(f.get("axis"), dir, "model.file.in.its_axis"));
    }
    if (!f.get("face").is_undefined() && !f.get("face").is_null()) {
      g.set("face", opt(f.get("face"), face, "model.file.in.its_face"));
    }
    if (f.get("flip") == Value(true)) {
      g.set("flip", true);
    }
    return true;
  }
  if (t == "sweep") {
    need(c.is_type(f.get("sketch"), "sketch"), "model.file.bad.its_profile_sketch");
    need(among(f.get("op"), kOps), "model.file.bad.the_operation");
    need(among(f.get("orient"), kOrients), "model.file.bad.the_orientation");
    need(num(or0("scale", 1), 1e-3, 1e3), "model.file.bad.the_end_scale");
    g.set("sketch", f.get("sketch"));
    g.set("path", wrap(
                      [&] {
                        return R.list(
                            f.get("path"), [&](const Value& x) { return R.path_item(x); }, 200,
                            "model.file.list.the_path");
                      },
                      "model.file.in.its_path", name, c.w));
    g.set("op", f.get("op"));
    g.set("orient", f.get("orient"));
    g.set("binormal", opt(f.get("binormal"), dir, "model.file.in.its_direction"));
    g.set("guide", opt(
                       f.get("guide"),
                       [&](const Value& v) {
                         return R.list(
                             v, [&](const Value& x) { return R.path_item(x); }, 200, "model.file.list.the_guide");
                       },
                       "model.file.in.its_guide"));
    g.set("scale", or0("scale", 1));
    return true;
  }
  if (t == "loft") {
    need(among(f.get("op"), kOps), "model.file.bad.the_operation");
    g.set("sections", wrap(
                          [&] {
                            return R.list(
                                f.get("sections"),
                                [&](const Value& s) -> Value {
                                  if (s.truthy() && s.get("sketch").is_string()) {
                                    if (!c.is_type(s.get("sketch"), "sketch")) {
                                      fail(c.w.t("model.design_features.ref.a_section_sketch_is_bad"));
                                    }
                                    return obj({{"sketch", s.get("sketch")}});
                                  }
                                  if (s.truthy() && s.get("face").truthy()) {
                                    return obj({{"face", R.face_ref(s.get("face"))}});
                                  }
                                  if (s.truthy() && s.get("point").truthy()) {
                                    return obj({{"point", R.point_ref(s.get("point"))}});
                                  }
                                  fail(c.w.t("model.design_features.ref.a_section_is_bad"));
                                },
                                100, "model.file.list.the_section_list");
                          },
                          "model.file.in.its_sections", name, c.w));
    g.set("op", f.get("op"));
    g.set("ruled", f.get("ruled") == Value(true));
    if (!f.get("rails").is_undefined() && !f.get("rails").is_null()) {
      const Value r = wrap(
          [&] {
            return R.list(
                f.get("rails"), [&](const Value& x) { return R.path_item(x); }, 200, "model.file.list.the_rail_list");
          },
          "model.file.in.its_guide_rails", name, c.w);
      if (!r.as_array().empty()) {
        g.set("rails", r);
      }
    }
    if (!f.get("railTol").is_undefined()) {
      need(num(f.get("railTol"), 1e-7, 10), "model.file.bad.the_rail_tolerance");
      g.set("railTol", f.get("railTol"));
    }
    return true;
  }
  if (t == "draft") {
    need(num(f.get("angle"), -89, 89), "model.file.bad.the_angle");
    g.set("faces", wrap([&] { return R.list(f.get("faces"), face, 500, "model.file.list.the_face_list"); },
                        "model.file.in.its_faces", name, c.w));
    g.set("neutral",
          wrap([&] { return read_plane(f.get("neutral"), c, true); }, "model.file.in.its_neutral_plane", name, c.w));
    g.set("pull", opt(f.get("pull"), dir, "model.file.in.its_pull_direction"));
    g.set("angle", f.get("angle"));
    return true;
  }
  if (t == "extrude") {
    need(f.get("offset").is_undefined() || num(f.get("offset"), -1e6, 1e6), "model.file.bad.the_offset");
    need(f.get("depth2").is_undefined() || num(f.get("depth2"), 1e-6, 1e6), "model.file.bad.the_second_depth");
    need(f.get("taper").is_undefined() || num(f.get("taper"), -89, 89), "model.file.bad.the_taper_angle");
    const Value regions = f.get("regions");
    need(regions.is_undefined() || regions.is_null() ||
             (regions.is_array() && regions.as_array().size() <= 200 &&
              std::ranges::all_of(regions.as_array(),
                                  [](const Value& p) {
                                    return p.is_array() && p.as_array().size() == 2 &&
                                           std::ranges::all_of(p.as_array(), [](const Value& v) { return num(v); });
                                  })),
         "model.file.bad.the_picked_regions");
    const std::vector<std::pair<std::string_view, Value>> extra = {
        {"upto", opt(
                     f.get("upto"), [&](const Value& p) { return read_plane(p, c, true); },
                     "model.file.in.the_face_it_goes_up_to")},
        {"vertex", opt(
                       f.get("vertex"), [&](const Value& p) { return R.point_ref(p); }, "model.file.in.its_vertex")},
        {"offset", or0("offset", 0)},
        {"two", f.get("two") == Value(true)},
        {"depth2", or0("depth2", 10)},
        {"taper", or0("taper", 0)},
        {"regions", regions.is_array() ? regions : Value(nullptr)}};
    const std::vector<std::pair<std::string_view, Value>> defaults = {
        {"upto", nullptr}, {"vertex", nullptr}, {"offset", 0},       {"two", false},
        {"depth2", 10},    {"taper", 0},        {"regions", nullptr}};
    for (std::size_t i = 0; i < extra.size(); ++i) {
      if (jsx::stringify(extra[i].second, 0) != jsx::stringify(defaults[i].second, 0)) {
        g.set(extra[i].first, extra[i].second);
      }
    }
    return true;
  }
  if (t == "hole") {
    need(f.get("csinkD").is_undefined() || num(f.get("csinkD"), 1e-6, 1e6), "model.file.bad.the_countersink_diameter");
    need(f.get("csinkAngle").is_undefined() || f.get("csinkAngle") == Value(90) || f.get("csinkAngle") == Value(82),
         "model.file.bad.the_countersink_angle");
    need(f.get("size").is_undefined() || iso_coarse(f.get("size")), "model.file.bad.the_thread_size");
    const Value td = f.get("threadDepth");
    need(td.is_undefined() || td.is_null() || num(td, 1e-6, 1e6), "model.file.bad.the_thread_depth");
    const Value kind = f.get("kind");
    if (kind == Value("csink") || kind == Value("tapped") || !f.get("csinkD").is_undefined() ||
        !f.get("size").is_undefined()) {
      g.set("csinkD", or0("csinkD", 12));
      g.set("csinkAngle", or0("csinkAngle", 90));
      g.set("size", f.get("size").is_null() || f.get("size").is_undefined() ? Value("M6") : f.get("size"));
      g.set("threadDepth", td.is_undefined() ? Value(nullptr) : td);
    }
    return true;
  }
  if (t == "chamfer") {
    const Value mode = f.get("mode");
    need(mode.is_undefined() || mode == Value("equal") || mode == Value("two") || mode == Value("angle"),
         "model.file.bad.the_chamfer_type");
    need(f.get("d2").is_undefined() || num(f.get("d2"), 1e-6, 1e5), "model.file.bad.the_second_distance");
    need(f.get("angle").is_undefined() || num(f.get("angle"), 1e-3, 89.999), "model.file.bad.the_angle");
    if (mode.truthy() && !(mode == Value("equal"))) {
      g.set("mode", mode);
      g.set("d2", f.get("d2").is_null() || f.get("d2").is_undefined() ? g.get("d") : f.get("d2"));
      g.set("angle", or0("angle", 45));
      g.set("flip", f.get("flip") == Value(true));
    }
    return true;
  }
  if (t == "fillet") {
    const Value r2 = f.get("r2");
    need(r2.is_undefined() || r2.is_null() || num(r2, 1e-6, 1e5), "model.file.bad.the_end_radius");
    if (r2.is_number()) {
      g.set("r2", r2);
      g.set("flip", f.get("flip") == Value(true));
    }
    return true;
  }
  return false;
}

}  // namespace

namespace {

bool good_body(const Value& id) {  // /^f\d{1,9}(:\d{1,4})?(@f\d{1,9}#\d{1,4}){0,24}$/, ≤ 300
  if (!id.is_string() || base::utf16_length(id.as_string()) > 300) {
    return false;
  }
  const std::string& s = id.as_string();
  std::size_t i = 0;
  const auto digits = [&](std::size_t lo, std::size_t hi) {
    std::size_t n = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
      ++n;
    }
    return n >= lo && n <= hi;
  };
  if (i >= s.size() || s[i] != 'f') {
    return false;
  }
  ++i;
  if (!digits(1, 9)) {
    return false;
  }
  if (i < s.size() && s[i] == ':') {
    ++i;
    if (!digits(1, 4)) {
      return false;
    }
  }
  int parts = 0;
  while (i < s.size()) {
    if (s[i] != '@' || ++parts > 24) {
      return false;
    }
    ++i;
    if (i >= s.size() || s[i] != 'f') {
      return false;
    }
    ++i;
    if (!digits(1, 9) || i >= s.size() || s[i] != '#') {
      return false;
    }
    ++i;
    if (!digits(1, 4)) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> body_features(const Value& id) {  // [...new Set(String(id || '').match(/f\d+/g) || [])]
  std::vector<std::string> out;
  const std::string s = id.truthy() ? base::js_string(id) : std::string();
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == 'f' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') {
      std::size_t j = i + 1;
      while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
        ++j;
      }
      std::string k = s.substr(i, j - i);
      if (std::ranges::find(out, k) == out.end()) {
        out.push_back(std::move(k));
      }
      i = j - 1;
    }
  }
  return out;
}

bool read_bodies(const Value& f, Value& g, const Ctx& c) {
  const Readers R{c};
  const std::string name = base::js_string(g.get("name"));
  const auto need = [&](bool ok, std::string_view key,
                        std::initializer_list<std::pair<std::string_view, std::string>> more = {}) {
    if (!ok) {
      std::vector<std::pair<std::string_view, std::string>> fills{{"name", name}};
      fills.insert(fills.end(), more.begin(), more.end());
      fail(fills.size() == 1 ? c.w.t(key, {fills[0]}) : c.w.t(key, {fills[0], fills[1]}));
    }
  };
  const auto body = [&](const Value& b) {
    const auto bf = body_features(b);
    if (!good_body(b) || !std::ranges::all_of(bf, [&](const std::string& x) { return c.has(Value(x)); })) {
      fail(c.w.t("model.bodies.bad_ref"));
    }
    return b;
  };
  const auto bodies = [&](const Value& v, std::size_t max = 500) {
    need(v.is_array() && v.as_array().size() <= max, "model.file.bad.its_body_list");
    return wrap(
        [&] {
          base::Array out;
          for (const Value& b : v.as_array()) {
            out.push_back(body(b));
          }
          return Value(std::move(out));
        },
        "model.file.in.its_bodies", name, c.w);
  };
  const auto opt = [&](const Value& v, auto fn, std::string_view what) -> Value {
    if (v.is_null() || v.is_undefined()) {
      return nullptr;
    }
    return wrap([&] { return fn(v); }, what, name, c.w);
  };
  const auto or_ = [&](std::string_view k, double d) {
    return f.get(k).is_null() || f.get(k).is_undefined() ? Value(d) : f.get(k);
  };
  const std::string t = base::js_string(f.get("type"));
  if (t == "combine") {
    need(f.get("op") == Value("join") || f.get("op") == Value("cut") || f.get("op") == Value("intersect"),
         "model.file.bad.the_operation");
    g.set("op", f.get("op"));
    g.set("target", f.get("target").is_null() || f.get("target").is_undefined()
                        ? Value(nullptr)
                        : wrap([&] { return body(f.get("target")); }, "model.file.in.its_target", name, c.w));
    g.set("tools", bodies(f.get("tools")));
    g.set("keep", f.get("keep") == Value(true));
    return true;
  }
  if (t == "movebody") {
    const Value mode = f.get("mode");
    need(mode == Value("translate") || mode == Value("points") || mode == Value("rotate") || mode == Value("align"),
         "model.file.bad.the_move_type");
    for (const std::string_view k : {"dx", "dy", "dz"}) {
      need(num(or_(k, 0), -1e6, 1e6), "model.file.bad.field", {{"field", std::string(k)}});
    }
    need(num(or_("angle", 0), -3600, 3600), "model.file.bad.the_angle");
    const Value planes = f.get("planes");
    need(planes.is_undefined() || (planes.is_array() && planes.as_array().size() <= 2),
         "model.file.bad.its_align_planes");
    g.set("bodies", bodies(f.get("bodies")));
    g.set("mode", mode);
    g.set("dx", or_("dx", 0));
    g.set("dy", or_("dy", 0));
    g.set("dz", or_("dz", 0));
    g.set("from", opt(f.get("from"), [&](const Value& p) { return R.point_ref(p); }, "model.file.in.its_from_point"));
    g.set("to", opt(f.get("to"), [&](const Value& p) { return R.point_ref(p); }, "model.file.in.its_to_point"));
    g.set("axis", opt(f.get("axis"), [&](const Value& a) { return read_dir(a, c, true); }, "model.file.in.its_axis"));
    g.set("edge", opt(f.get("edge"), [&](const Value& e) { return R.edge_ref(e); }, "model.file.in.its_axis_edge"));
    g.set("face", opt(f.get("face"), [&](const Value& e) { return R.face_ref(e); }, "model.file.in.its_axis_face"));
    g.set("angle", or_("angle", 0));
    g.set("planes", wrap(
                        [&] {
                          base::Array out;
                          if (planes.truthy()) {
                            for (const Value& p : planes.as_array()) {
                              out.push_back(read_plane(p, c, true));
                            }
                          }
                          return Value(std::move(out));
                        },
                        "model.file.in.its_align_planes", name, c.w));
    g.set("flip", f.get("flip") == Value(true));
    g.set("copy", f.get("copy") == Value(true));
    return true;
  }
  if (t == "scalebody") {
    for (const std::string_view k : {"s", "sx", "sy", "sz"}) {
      need(num(or_(k, 1), 1e-6, 1e6), "model.file.bad.the_scale", {{"k", std::string(k)}});
    }
    g.set("bodies", bodies(f.get("bodies")));
    g.set("uniform", !(f.get("uniform") == Value(false)));
    g.set("s", or_("s", 1));
    g.set("sx", or_("sx", 1));
    g.set("sy", or_("sy", 1));
    g.set("sz", or_("sz", 1));
    g.set("point", opt(f.get("point"), [&](const Value& p) { return R.point_ref(p); }, "model.file.in.its_point"));
    return true;
  }
  if (t == "pathpattern") {
    const Value seeds = f.get("seeds");
    need(seeds.is_array() && seeds.as_array().size() <= 500 &&
             std::ranges::all_of(seeds.as_array(), [&](const Value& s) { return c.has(s); }),
         "model.file.bad.the_features_to_pattern");
    need(is_int(f.get("count")) && f.get("count").as_number() >= 1 && f.get("count").as_number() <= 1000,
         "model.file.bad.the_count");
    need(f.get("spacing") == Value("equal") || f.get("spacing") == Value("distance"),
         "model.file.bad.the_spacing_type");
    need(num(f.get("d"), 1e-6, 1e6), "model.file.bad.the_spacing");
    need(f.get("orient") == Value("path") || f.get("orient") == Value("fixed"), "model.file.bad.the_orientation");
    g.set("seeds", seeds);
    g.set("bodies",
          bodies(f.get("bodies").is_null() || f.get("bodies").is_undefined() ? Value(base::Array{}) : f.get("bodies")));
    g.set("path", wrap(
                      [&] {
                        return R.list(
                            f.get("path"), [&](const Value& x) { return R.path_item(x); }, 200,
                            "model.file.list.the_path");
                      },
                      "model.file.in.its_path", name, c.w));
    g.set("count", f.get("count"));
    g.set("spacing", f.get("spacing"));
    g.set("d", f.get("d"));
    g.set("orient", f.get("orient"));
    g.set("reverse", f.get("reverse") == Value(true));
    return true;
  }
  if (t == "lpattern" || t == "cpattern") {
    if (!f.get("bodies").is_undefined() && !f.get("bodies").is_null()) {
      const Value b = bodies(f.get("bodies"));
      if (!b.as_array().empty()) {
        g.set("bodies", b);
      }
    }
    return false;
  }
  return false;
}

void read_sheet(const Value& f, Value& g, const Ctx& c) {
  const Readers R{c};
  const std::string name = base::js_string(g.get("name"));
  const auto need = [&](bool ok, std::string_view key) {
    if (!ok) {
      fail(c.w.t(key, {{"name", name}}));
    }
  };
  const auto in = [](const Value& v, double lo, double hi) { return num(v, lo, hi); };
  const std::string t = base::js_string(f.get("type"));
  if (t == "sheetbase") {
    need(c.has(f.get("sketch")) && c.is_type(f.get("sketch"), "sketch"), "model.file.bad.its_sketch");
    need(
        in(f.get("t"), 0.01, 100) && in(f.get("r"), 0, 1000) && in(f.get("k"), 0, 0.5) && in(f.get("depth"), 0.01, 1e5),
        "model.file.bad.its_thickness_radius_k_factor_or_depth");
    g.set("sketch", f.get("sketch"));
    g.set("t", f.get("t"));
    g.set("r", f.get("r"));
    g.set("k", f.get("k"));
    g.set("depth", f.get("depth"));
    g.set("reverse", f.get("reverse") == Value(true));
  } else {
    need(in(f.get("angle"), 1, 180) && in(f.get("length"), 0.01, 1e5), "model.file.bad.its_angle_or_length");
    const Value off = f.get("offsets");
    need(off.is_array() && off.as_array().size() == 2 &&
             std::ranges::all_of(off.as_array(), [&](const Value& o) { return in(o, 0, 1e5); }),
         "model.file.bad.its_offsets");
    need((f.get("relief") == Value("rect") || f.get("relief") == Value("none")) && in(f.get("reliefWidth"), 0.01, 100),
         "model.file.bad.its_relief");
    g.set("edge", f.get("edge").is_null() || f.get("edge").is_undefined() ? Value(nullptr) : R.edge_ref(f.get("edge")));
    g.set("angle", f.get("angle"));
    g.set("length", f.get("length"));
    g.set("flip", f.get("flip") == Value(true));
    g.set("offsets", off);
    g.set("relief", f.get("relief"));
    g.set("reliefWidth", f.get("reliefWidth"));
  }
}

void read_rib(const Value& f, Value& g, const Ctx& c) {
  const std::string name = base::js_string(g.get("name"));
  const auto need = [&](bool ok, std::string_view key) {
    if (!ok) {
      fail(c.w.t(key, {{"name", name}}));
    }
  };
  need(c.has(f.get("sketch")) && c.is_type(f.get("sketch"), "sketch"), "model.file.bad.its_sketch");
  need(num(f.get("t"), 1e-4, 1e5), "model.file.bad.its_thickness");
  need(f.get("side") == Value("mid") || f.get("side") == Value("front") || f.get("side") == Value("back"),
       "model.file.bad.its_side");
  g.set("sketch", f.get("sketch"));
  g.set("t", f.get("t"));
  g.set("side", f.get("side"));
  g.set("flip", f.get("flip") == Value(true));
}

void read_thread(const Value& f, Value& g, const Ctx& c) {
  const Readers R{c};
  const std::string name = base::js_string(g.get("name"));
  const auto need = [&](bool ok, std::string_view key) {
    if (!ok) {
      fail(c.w.t(key, {{"name", name}}));
    }
  };
  const Value form = f.get("form");
  need(form == Value("iso") || form == Value("tr"), "thread.file.bad.form");
  const Value size = f.get("size");
  bool size_ok = size == Value("auto");
  if (!size_ok && size.is_string()) {
    for (const std::string_view s : threads::sizes(form.as_string())) {
      size_ok = size_ok || s == size.as_string();
    }
  }
  need(size_ok, "model.file.bad.the_thread_size");
  const Value len = f.get("length");
  need(len.is_null() || len.is_undefined() || num(len, 1e-3, 1e5), "thread.file.bad.length");
  need(f.get("clearance").is_undefined() || num(f.get("clearance"), 0, 5), "thread.file.bad.clearance");
  const Value hand = f.get("hand");
  need(hand.is_undefined() || hand == Value("right") || hand == Value("left"), "thread.file.bad.hand");
  const Value faces = f.get("faces");
  need(faces.is_array() && faces.as_array().size() <= 100, "model.file.list.the_face_list");
  Value fs;
  try {
    base::Array out;
    for (const Value& r : faces.as_array()) {
      out.push_back(R.face_ref(r));
    }
    fs = Value(std::move(out));
  } catch (const Thrown& e) {
    std::string m = e.what();
    if (!m.empty() && m.back() == '.') {
      m.pop_back();
    }
    fail(c.w.t("model.file.in.its_faces", {{"name", name}, {"problem", m}}));
  }
  g.set("faces", fs);
  g.set("form", form);
  g.set("size", size);
  g.set("length", len.is_undefined() ? Value(nullptr) : len);
  g.set("flip", f.get("flip") == Value(true));
  g.set("modelled", f.get("modelled") == Value(true));
  g.set("clearance",
        f.get("clearance").is_null() || f.get("clearance").is_undefined() ? Value(0.1) : f.get("clearance"));
  g.set("hand", hand.is_null() || hand.is_undefined() ? Value("right") : hand);
}

void read_hole_thread(const Value& f, Value& g, const Ctx& c) {
  if (f.get("modelled").is_undefined() && f.get("clearance").is_undefined()) {
    return;
  }
  if (!f.get("clearance").is_undefined() && !num(f.get("clearance"), 0, 5)) {
    fail(c.w.t("thread.file.bad.clearance", {{"name", base::js_string(g.get("name"))}}));
  }
  if (f.get("modelled") == Value(true)) {
    g.set("modelled", true);
    g.set("clearance",
          f.get("clearance").is_null() || f.get("clearance").is_undefined() ? Value(0.1) : f.get("clearance"));
  }
}

}  // namespace

namespace {

void read_surface(const Value& f, Value& g, const Ctx& c) {
  const std::string name = base::js_string(g.get("name"));
  const auto need = [&](bool ok, std::string_view key) {
    if (!ok) {
      fail(c.w.t(key, {{"name", name}}));
    }
  };
  const auto vec3s = [](const Value& v) { return vec3(v, 1e7, false); };
  const auto sheet_ref = [&](const Value& id) {
    const auto t = c.type_of(id);
    need(id.is_string() && c.has(id) && t && surface_label(Value(*t)), "model.file.bad.its_surface");
    return id;
  };
  const auto op = [&] {
    need(f.get("op") == Value("new") || f.get("op") == Value("add") || f.get("op") == Value("cut") ||
             f.get("op") == Value("intersect"),
         "model.file.bad.the_operation");
    return f.get("op");
  };
  const auto edge_list = [&](const Value& e) {
    const Value faces = e.truthy() ? e.get("faces") : Value{};
    return e.truthy() && faces.is_array() && faces.as_array().size() == 2 &&
           std::ranges::all_of(faces.as_array(), [&](const Value& n) { return c.known(n); });
  };
  const std::string t = base::js_string(f.get("type"));
  if (t == "form") {
    auto cage = form::read_form(f.get("cage"), c.w);
    if (!cage) {
      fail(c.w.t("model.surface.form_bad", {{"name", name}, {"problem", cage.error().message}}));
    }
    need(!cage->get("faces").as_array().empty(), "model.file.bad.its_cage");
    const Value level = f.get("level");
    need(level.is_null() || level.is_undefined() || (is_int(level) && level.as_number() >= 0 && level.as_number() <= 3),
         "model.file.bad.the_level");
    g.set("cage", std::move(*cage));
    g.set("op", op());
    g.set("level", level.is_undefined() ? Value(nullptr) : level);
  } else if (t == "patch") {
    const Value edges = f.get("edges");
    need(edges.is_array() && edges.as_array().size() <= 500, "model.file.bad.its_edge_list");
    base::Array es;
    for (const Value& e : edges.as_array()) {
      need(edge_list(e), "model.file.bad.an_edge_reference");
      need(e.get("at").is_undefined() || vec3s(e.get("at")), "model.file.bad.an_edge_position");
      Value r = obj({{"faces", e.get("faces")}});
      if (e.get("at").truthy()) {
        r.append("at", e.get("at"));
      }
      es.push_back(std::move(r));
    }
    g.set("edges", es);
    g.set("sheet",
          f.get("sheet").is_null() || f.get("sheet").is_undefined() ? Value(nullptr) : sheet_ref(f.get("sheet")));
    const bool sheet = g.get("sheet").truthy();
    const bool any = !es.empty();
    need(sheet != any || (!sheet && !any), "model.file.bad.its_boundary_edges_or_a_surface_not_both");
    need(f.get("at").is_null() || f.get("at").is_undefined() || vec3s(f.get("at")), "model.file.bad.its_position");
    const Value cont = f.get("cont").is_null() || f.get("cont").is_undefined() ? Value("G0") : f.get("cont");
    need(cont == Value("G0") || cont == Value("G1"), "model.file.bad.the_continuity");
    g.set("at", f.get("at").truthy() ? f.get("at") : Value(nullptr));
    g.set("cont", cont);
  } else if (t == "surfloft") {
    const Value s = f.get("sketches");
    std::vector<Value> seen;
    bool ok = s.is_array() && s.as_array().size() <= 50;
    if (ok) {
      for (const Value& x : s.as_array()) {
        ok = ok && c.has(x) && c.is_type(x, "sketch") && std::ranges::find(seen, x) == seen.end();
        seen.push_back(x);
      }
    }
    need(ok, "model.file.bad.its_sketches");
    g.set("sketches", s);
  } else if (t == "stitch") {
    const Value s = f.get("sheets");
    std::vector<Value> seen;
    bool ok = s.is_array() && s.as_array().size() <= 200;
    if (ok) {
      for (const Value& x : s.as_array()) {
        ok = ok && std::ranges::find(seen, x) == seen.end();
        seen.push_back(x);
      }
    }
    need(ok, "model.file.bad.its_surfaces");
    base::Array sheets;
    for (const Value& x : s.as_array()) {
      sheets.push_back(sheet_ref(x));
    }
    g.set("sheets", std::move(sheets));
    const Value tol = f.get("tol").is_null() || f.get("tol").is_undefined() ? Value(0.01) : f.get("tol");
    need(num(tol, 1e-7, 10), "model.file.bad.the_tolerance");
    g.set("op", op());
    g.set("tol", tol);
  } else if (t == "thicken") {
    g.set("sheet", f.get("sheet").is_null() ? Value(nullptr) : sheet_ref(f.get("sheet")));
    need(num(f.get("t"), 1e-6, 1e5), "model.file.bad.the_thickness");
    need(f.get("side") == Value("out") || f.get("side") == Value("in") || f.get("side") == Value("both"),
         "model.file.bad.the_side");
    g.set("t", f.get("t"));
    g.set("side", f.get("side"));
    g.set("op", op());
  } else if (t == "offsetsurf") {
    g.set("sheet", f.get("sheet").is_null() ? Value(nullptr) : sheet_ref(f.get("sheet")));
    need(num(f.get("d"), -1e5, 1e5) && !(f.get("d") == Value(0)), "model.file.bad.the_distance");
    g.set("d", f.get("d"));
  } else if (t == "extendsurf") {
    g.set("sheet", f.get("sheet").is_null() ? Value(nullptr) : sheet_ref(f.get("sheet")));
    need(num(f.get("d"), 1e-6, 1e5), "model.file.bad.the_distance");
    const Value sides = f.get("sides");
    const auto side_ok = [](const Value& s) {
      return s == Value("umin") || s == Value("umax") || s == Value("vmin") || s == Value("vmax");
    };
    need(sides.is_array() && !sides.as_array().empty() && std::ranges::all_of(sides.as_array(), side_ok),
         "model.file.bad.the_sides");
    base::Array uniq;
    for (const Value& s : sides.as_array()) {
      if (std::ranges::find(uniq, s) == uniq.end()) {
        uniq.push_back(s);
      }
    }
    g.set("d", f.get("d"));
    g.set("sides", std::move(uniq));
  } else if (t == "trimsurf") {
    g.set("sheet", f.get("sheet").is_null() ? Value(nullptr) : sheet_ref(f.get("sheet")));
    const Value tool = f.get("tool");
    const auto tt = c.type_of(tool);
    need(tool.is_null() || tool == Value("body") ||
             (tool.is_string() && c.has(tool) && tt && surface_label(Value(*tt)) && !(tool == f.get("sheet"))),
         "model.file.bad.its_cutting_tool");
    need(f.get("at").is_null() || f.get("at").is_undefined() || vec3s(f.get("at")), "model.file.bad.the_point_to_keep");
    need(f.get("keep") == Value("near") || f.get("keep") == Value("far"), "model.file.bad.which_piece_to_keep");
    g.set("tool", tool);
    g.set("at", f.get("at").truthy() ? f.get("at") : Value(nullptr));
    g.set("keep", f.get("keep"));
  } else if (t == "gordon") {
    for (const std::string_view k : {"profiles", "guides"}) {
      const std::string kk(k);
      const auto key = [&](std::string_view what) { return "model.file.bad." + std::string(what) + kk; };
      const Value list = f.get(k);
      need(list.is_array() && list.as_array().size() <= 200, key("its_"));
      base::Array out;
      for (const Value& x : list.as_array()) {
        need(x.truthy() && (x.is_object() || x.is_array()), key("a_curve_of_its_"));
        if (!x.get("edge").is_undefined()) {
          const Value e = x.get("edge");
          need(edge_list(e) && (e.get("at").is_undefined() || vec3s(e.get("at"))), key("an_edge_of_its_"));
          Value r = obj({{"faces", e.get("faces")}});
          if (e.get("at").truthy()) {
            r.append("at", e.get("at"));
          }
          out.push_back(obj({{"edge", std::move(r)}}));
          continue;
        }
        need(x.get("sketch").is_string() && c.has(x.get("sketch")) && c.is_type(x.get("sketch"), "sketch"),
             key("a_sketch_of_its_"));
        const Value ents = x.get("entities");
        need(ents.is_undefined() ||
                 (ents.is_array() && !ents.as_array().empty() && ents.as_array().size() <= 5000 &&
                  std::ranges::all_of(
                      ents.as_array(),
                      [](const Value& y) { return y.is_string() && base::utf16_length(y.as_string()) <= 40; })),
             key("the_curves_picked_from_a_sketch_of_its_"));
        Value r = obj({{"sketch", x.get("sketch")}});
        if (ents.truthy()) {
          r.append("entities", ents);
        }
        out.push_back(std::move(r));
      }
      g.set(k, std::move(out));
    }
    const Value tan = f.get("tangent");
    need(tan.is_undefined() || (tan.is_array() && tan.as_array().size() <= 4), "model.file.bad.its_tangent_planes");
    if (tan.is_array() && !tan.as_array().empty()) {
      base::Array ps;
      for (const Value& p : tan.as_array()) {
        ps.push_back(read_plane(p, c, true));
      }
      g.set("tangent", std::move(ps));
    }
    const Value tol = f.get("tol").is_null() || f.get("tol").is_undefined() ? Value(0.01) : f.get("tol");
    need(num(tol, 1e-7, 10), "model.file.bad.the_tolerance");
    g.set("tol", tol);
    g.set("solid", f.get("solid") == Value(true));
    g.set("op", op());
  }
}

}  // namespace

namespace {

bool date_ok(const std::string& s) {  // /^\d{4}-\d{2}-\d{2}$/
  if (s.size() != 10) {
    return false;
  }
  for (std::size_t i = 0; i < 10; ++i) {
    const bool dash = i == 4 || i == 7;
    const bool digit = s[i] >= '0' && s[i] <= '9';
    if (dash ? s[i] != '-' : !digit) {
      return false;
    }
  }
  return true;
}

constexpr std::array<std::string_view, 10> kSheets = {"a4",     "a3",     "a2",     "a1",     "a0",
                                                      "ansi-a", "ansi-b", "ansi-c", "ansi-d", "ansi-e"};
constexpr std::array<std::pair<std::string_view, std::size_t>, 10> kTitle = {{{"title", 60},
                                                                              {"number", 40},
                                                                              {"rev", 8},
                                                                              {"company", 60},
                                                                              {"drawnBy", 40},
                                                                              {"checkedBy", 40},
                                                                              {"approvedBy", 40},
                                                                              {"date", 10},
                                                                              {"finish", 40},
                                                                              {"tolerance", 40}}};

Value read_body_info(const Value& raw, const Ctx& c) {
  if (!raw.truthy() || !raw.is_object()) {
    fail(c.w.t("file.bodies.bad"));
  }
  const auto keys = jsx::entries(raw);
  if (keys.size() > 500) {
    fail(c.w.t("file.bodies.too_many"));
  }
  Value out = Value::object();
  for (const auto& [id, v] : keys) {
    if (!good_body(Value(id)) || !v.truthy() || !v.is_object()) {
      fail(c.w.t("file.bodies.entry_bad"));
    }
    if (!v.get("name").is_undefined() && !v.get("name").is_string()) {
      fail(c.w.t("file.bodies.name_bad"));
    }
    if (!std::ranges::all_of(body_features(Value(id)), [&](const std::string& x) { return c.has(Value(x)); })) {
      continue;
    }
    Value e = Value::object();
    const std::string nm =
        jsx::utf16_slice(std::string(base::js_trim(jsx::strip_control(jsx::text_of(v.get("name"))))), 60);
    if (!nm.empty()) {
      e.append("name", nm);
    }
    if (v.get("hidden") == Value(true)) {
      e.append("hidden", true);
    }
    if (!nm.empty() || v.get("hidden") == Value(true)) {
      out.set(id, std::move(e));
    }
  }
  return jsx::entries(out).empty() ? Value(nullptr) : out;
}

}  // namespace

base::Result<Value> read_drawing(const Value& d, const i18n::Catalog& w) {
  return guard([&] {
    const auto bad = [&](std::string_view k) { fail(w.t(k)); };
    if (!d.truthy() || (!d.is_object() && !d.is_array())) {
      bad("model.part.file.drawing_bad");
    }
    Value out = obj({{"section", nullptr}, {"detail", nullptr}, {"notes", base::Array{}}});
    const auto view = [](const Value& v) { return v == Value("front") || v == Value("top") || v == Value("right"); };
    const Value sec = d.get("section");
    if (sec.truthy()) {
      if (!view(jsx::get(sec, "of")) || (!jsx::get(sec, "at").is_undefined() && !num(jsx::get(sec, "at")))) {
        bad("model.part.file.section_bad");
      }
      Value s = obj({{"of", sec.get("of")}});
      if (!sec.get("at").is_undefined()) {
        s.append("at", sec.get("at"));
      }
      out.set("section", std::move(s));
    }
    const Value dt = d.get("detail");
    if (dt.truthy()) {
      const Value at = jsx::get(dt, "at");
      const Value scale = jsx::get(dt, "scale");
      const bool scale_ok = scale == Value(2) || scale == Value(4) || scale == Value(5) || scale == Value(10);
      if (!(view(jsx::get(dt, "of")) || jsx::get(dt, "of") == Value("section")) || !at.is_array() ||
          at.as_array().size() != 2 || !std::ranges::all_of(at.as_array(), [](const Value& v) { return num(v); }) ||
          !num(jsx::get(dt, "r"), 1e-3, 1e6) || !scale_ok) {
        bad("model.part.file.detail_bad");
      }
      out.set("detail", obj({{"of", dt.get("of")}, {"at", at}, {"r", dt.get("r")}, {"scale", scale}}));
    }
    if (!d.get("notes").is_undefined()) {
      auto n = gdt::read_notes(d.get("notes"), w);
      if (!n) {
        fail(n.error().message);
      }
      out.set("notes", std::move(*n));
    }
    const Value sheet = d.get("sheet");
    if (!sheet.is_undefined() && !sheet.is_null()) {
      if (!among(sheet, kSheets)) {
        bad("file.drawing.sheet_bad");
      }
      if (!(sheet == Value("a3"))) {
        out.set("sheet", sheet);
      }
    }
    const Value title = d.get("title");
    if (!title.is_undefined() && !title.is_null()) {
      if (!title.is_object()) {
        bad("file.drawing.title_bad");
      }
      Value tt = Value::object();
      for (const auto& [k, max] : kTitle) {
        const Value v = title.get(k);
        if (v.is_undefined() || v.is_null() || v == Value("")) {
          continue;
        }
        if (!v.is_string()) {
          bad("file.drawing.title_bad");
        }
        const std::string cl(base::js_trim(text(v, max)));
        if (k == "date" && !cl.empty() && !date_ok(cl)) {
          bad("file.drawing.date_bad");
        }
        if (!cl.empty()) {
          tt.append(k, cl);
        }
      }
      if (!jsx::entries(tt).empty()) {
        out.set("title", std::move(tt));
      }
    }
    const Value ht = d.get("holeTable");
    if (!ht.is_undefined() && !ht.is_null()) {
      if (!ht.is_object() || !view(ht.get("view"))) {
        bad("file.drawing.holes_bad");
      }
      out.set("holeTable", obj({{"view", ht.get("view")}}));
    }
    const Value revs = d.get("revisions");
    if (!revs.is_undefined() && !revs.is_null()) {
      if (!revs.is_array() || revs.as_array().size() > 20) {
        bad("file.drawing.revisions_bad");
      }
      base::Array rs;
      for (const Value& r : revs.as_array()) {
        if (!r.truthy() || (!r.is_object() && !r.is_array())) {
          bad("file.drawing.revisions_bad");
        }
        for (const std::string_view k : {"rev", "desc", "date", "by"}) {
          if (!r.get(k).is_undefined() && !r.get(k).is_string()) {
            bad("file.drawing.revisions_bad");
          }
        }
        const std::string date(base::js_trim(text(r.get("date"), 10)));
        if (!date.empty() && !date_ok(date)) {
          bad("file.drawing.date_bad");
        }
        rs.push_back(obj({{"rev", std::string(base::js_trim(text(r.get("rev"), 8)))},
                          {"desc", std::string(base::js_trim(text(r.get("desc"), 120)))},
                          {"date", date},
                          {"by", std::string(base::js_trim(text(r.get("by"), 40)))}}));
      }
      if (!rs.empty()) {
        out.set("revisions", std::move(rs));
      }
    }
    return out;
  });
}

base::Result<Value> read_part(const Value& raw, const i18n::Catalog& w) {
  return guard([&]() -> Value {
    Value j = raw;
    if (raw.is_string()) {
      auto p = base::parse_json(raw.as_string());
      if (!p) {
        fail(w.t("model.part.file.not_json"));
      }
      j = std::move(*p);
    }
    if (!j.truthy() || !(jsx::get(j, "format") == Value(kFormat))) {
      fail(w.t("model.part.file.not_partcraft"));
    }
    const Value ver = j.get("version");
    if (!is_int(ver) || ver.as_number() < 1) {
      fail(w.t("model.part.file.no_version"));
    }
    if (ver.as_number() > kVersion) {
      fail(w.t("model.part.file.newer", {{"v", base::js_string(ver)}, {"max", base::js_string(Value(kVersion))}}));
    }
    const Value feats = j.get("features");
    if (!feats.is_array() || feats.as_array().size() > 2000) {
      fail(w.t("model.part.file.no_features"));
    }
    const std::string nm = text(j.get("name"));
    Value part = new_part(nm.empty() ? Value("Part1") : Value(nm));
    if (num(j.get("resolution"), 8, 512)) {
      part.set("resolution", base::js_round(j.get("resolution").as_number()));
    }
    Ctx c{.ids = {}, .types = {}, .w = w};
    double max_n = 0;
    for (const Value& f : feats.as_array()) {
      if (!f.truthy() || !feature_id(jsx::get(f, "id")) || c.has(f.get("id")) || !label_truthy(f.get("type"))) {
        fail(w.t("model.part.file.feature_bad"));
      }
      const std::string fname = text(f.get("name"), 60);
      Value g =
          obj({{"id", f.get("id")}, {"type", f.get("type")}, {"name", fname.empty() ? f.get("id") : Value(fname)}});
      if (f.get("suppressed") == Value(true)) {
        g.append("suppressed", true);
      }
      const std::string gname = base::js_string(g.get("name"));
      const auto need = [&](bool ok, std::string_view key,
                            std::initializer_list<std::pair<std::string_view, std::string>> more = {}) {
        if (!ok) {
          if (more.size() == 0) {
            fail(w.t(key, {{"name", gname}}));
          }
          fail(w.t(key, {{"name", gname}, *more.begin()}));
        }
      };
      const auto sketch_ref = [&] {
        const Value s = f.get("sketch");
        bool ok = c.has(s);
        if (ok) {
          const Value* sf = nullptr;
          for (const Value& x : features_of(part)) {
            if (x.get("id") == s) {
              sf = &x;
              break;
            }
          }
          ok = sf != nullptr && sf->get("type") == Value("sketch");
        }
        need(ok, "model.file.bad.its_sketch");
        return s;
      };
      const std::string t = base::js_string(f.get("type"));
      const auto ops4 = [&] { need(among(f.get("op"), kOps), "model.file.bad.the_operation"); };
      if (t == "sketch") {
        g.set("plane", read_plane(f.get("plane"), c, true));
        auto sk = sketch::read_sketch(f.get("sketch"), w);
        if (!sk) {
          fail(sk.error().message);
        }
        g.set("sketch", std::move(*sk));
      } else if (t == "extrude") {
        g.set("sketch", sketch_ref());
        ops4();
        need(among(f.get("end"), {"blind", "mid", "through", "upto", "next", "vertex", "offset"}),
             "model.file.bad.the_end_condition");
        need(num(f.get("depth"), 1e-6, 1e6), "model.file.bad.the_depth");
        g.set("op", f.get("op"));
        g.set("end", f.get("end"));
        g.set("depth", f.get("depth"));
        g.set("reverse", f.get("reverse") == Value(true));
        (void)read_design(f, g, c);
      } else if (t == "revolve") {
        g.set("sketch", sketch_ref());
        ops4();
        need(num(f.get("angle"), 1e-3, 360), "model.file.bad.the_angle");
        const Value axis = f.get("axis");
        need(axis.is_null() || axis.is_undefined() || axis.is_string() || axis.is_object() || axis.is_array(),
             "model.file.bad.the_axis");
        g.set("op", f.get("op"));
        g.set("angle", f.get("angle"));
        if (axis.truthy() && (axis.is_object() || axis.is_array())) {
          g.set("axis", read_dir(axis, c, true));
        } else {
          g.set("axis", axis.is_undefined() ? Value(nullptr) : axis);
        }
      } else if (t == "hole") {
        g.set("sketch", sketch_ref());
        need(among(f.get("kind"), {"simple", "cbore", "csink", "tapped"}), "model.file.bad.the_hole_type");
        for (const std::string_view k : {"d", "depth", "cboreD", "cboreDepth"}) {
          need(num(f.get(k), 1e-6, 1e6), "model.file.bad.field", {{"field", std::string(k)}});
        }
        g.set("kind", f.get("kind"));
        g.set("d", f.get("d"));
        g.set("depth", f.get("depth"));
        g.set("through", f.get("through") == Value(true));
        g.set("cboreD", f.get("cboreD"));
        g.set("cboreDepth", f.get("cboreDepth"));
        g.set("reverse", f.get("reverse") == Value(true));
        (void)read_design(f, g, c);
        read_hole_thread(f, g, c);
      } else if (t == "lpattern" || t == "cpattern") {
        const Value seeds = f.get("seeds");
        need(seeds.is_array() && std::ranges::all_of(seeds.as_array(), [&](const Value& s) { return c.has(s); }),
             "model.file.bad.the_features_to_pattern");
        need(is_int(f.get("count")) && f.get("count").as_number() >= 1 && f.get("count").as_number() <= 1000,
             "model.file.bad.the_count");
        if (t == "lpattern") {
          need(num(f.get("spacing"), -1e6, 1e6), "model.file.bad.the_spacing");
          g.set("seeds", seeds);
          g.set("dir", read_dir(f.get("dir"), c, false));
          g.set("count", f.get("count"));
          g.set("spacing", f.get("spacing"));
        } else {
          need(num(f.get("angle"), 1e-3, 360), "model.file.bad.the_angle");
          g.set("seeds", seeds);
          g.set("axis", read_dir(f.get("axis"), c, false));
          g.set("count", f.get("count"));
          g.set("angle", f.get("angle"));
        }
        g.set("reverse", f.get("reverse") == Value(true));
        (void)read_bodies(f, g, c);
      } else if (t == "mirror") {
        const Value seeds = f.get("seeds");
        need(seeds == Value("body") ||
                 (seeds.is_array() && std::ranges::all_of(seeds.as_array(), [&](const Value& s) { return c.has(s); })),
             "model.file.bad.what_to_mirror");
        g.set("seeds", seeds);
        g.set("plane", read_plane(f.get("plane"), c, false));
      } else if (t == "fillet" || t == "chamfer") {
        const Value edges = f.get("edges");
        need(edges.is_array() && edges.as_array().size() <= 2000, "model.file.bad.its_edge_list");
        base::Array es;
        for (const Value& e : edges.as_array()) {
          const Value faces = e.truthy() ? e.get("faces") : Value{};
          need(e.truthy() && faces.is_array() && faces.as_array().size() == 2 &&
                   std::ranges::all_of(faces.as_array(), [](const Value& n) { return good_name(n); }) &&
                   std::ranges::all_of(faces.as_array(),
                                       [&](const Value& n) {
                                         return std::ranges::all_of(
                                             name_features(n), [&](const std::string& x) { return c.has(Value(x)); });
                                       }),
               "model.file.bad.an_edge_reference");
          need(e.get("at").is_undefined() || vec3(e.get("at")), "model.file.bad.an_edge_position");
          Value r = obj({{"faces", faces}});
          if (e.get("at").truthy()) {
            r.append("at", e.get("at"));
          }
          es.push_back(std::move(r));
        }
        g.set("edges", std::move(es));
        const std::string_view key = t == "fillet" ? "r" : "d";
        need(num(f.get(key), 1e-6, 1e5), t == "fillet" ? "model.file.bad.the_radius" : "model.file.bad.the_distance");
        g.set(key, f.get(key));
        (void)read_design(f, g, c);
      } else if (surface_type(t)) {
        read_surface(f, g, c);
      } else if (t == "plane" || t == "axis" || t == "point" || t == "sweep" || t == "loft" || t == "draft") {
        (void)read_design(f, g, c);
      } else if (t == "combine" || t == "movebody" || t == "scalebody" || t == "pathpattern") {
        (void)read_bodies(f, g, c);
      } else if (t == "sheetbase" || t == "sheetflange") {
        read_sheet(f, g, c);
      } else if (t == "rib") {
        read_rib(f, g, c);
      } else if (t == "thread") {
        read_thread(f, g, c);
      } else if (t == "shell") {
        const Value faces = f.get("faces");
        need(faces.is_array() && faces.as_array().size() <= 500, "model.file.bad.its_face_list");
        base::Array fs;
        for (const Value& r : faces.as_array()) {
          need(r.truthy() && good_name(r.get("name")) &&
                   std::ranges::all_of(name_features(r.get("name")),
                                       [&](const std::string& x) { return c.has(Value(x)); }),
               "model.file.bad.a_face_reference");
          need(r.get("at").is_undefined() || vec3(r.get("at")), "model.file.bad.a_face_position");
          Value x = obj({{"name", r.get("name")}});
          if (r.get("at").truthy()) {
            x.append("at", r.get("at"));
          }
          fs.push_back(std::move(x));
        }
        g.set("faces", std::move(fs));
        need(num(f.get("t"), 1e-6, 1e5), "model.file.bad.the_thickness");
        g.set("t", f.get("t"));
        g.set("outward", f.get("outward") == Value(true));
      } else if (t == "import") {
        const Value step = f.get("step");
        const bool ok = step.is_string() && base::utf16_length(step.as_string()) <= std::size_t{60} * 1024 * 1024 &&
                        base::js_trim_start(step.as_string()).starts_with("ISO-10303-21;");
        need(ok, "model.file.bad.its_step_data");
        g.set("file", text(f.get("file"), 200));
        g.set("step", step);
        g.set("up", f.get("up") == Value("y") ? "y" : "z");
      }
      c.ids.push_back(g.get("id").as_string());
      c.types.emplace_back(g.get("id").as_string(), t);
      max_n = jsx::max(max_n, base::js_number(g.get("id").as_string().substr(1)));
      features(part)->push(std::move(g));
    }
    part.set("seq", max_n + 1);
    if (!j.get("drawing").is_undefined() && !j.get("drawing").is_null()) {
      auto d = read_drawing(j.get("drawing"), w);
      if (!d) {
        fail(d.error().message);
      }
      part.set("drawing", std::move(*d));
    }
    const Value studies = j.get("studies");
    if (!studies.is_undefined() && !studies.is_null()) {
      if (!studies.truthy() || !studies.is_object()) {
        fail(w.t("model.part.file.studies_bad"));
      }
      const auto keys = jsx::entries(studies);
      if (keys.size() > 32) {
        fail(w.t("model.part.file.too_many_studies"));
      }
      if (!keys.empty()) {
        // no workspace has registered a study reader here (0.5's parity driver loads none either)
        fail(w.t("model.part.file.unknown_study", {{"key", text(Value(keys.front().first), 40)}}));
      }
    }
    if (!j.get("material").is_undefined() && !j.get("material").is_null()) {
      auto m = bom::read_material(j.get("material"), w);
      if (!m) {
        fail(m.error().message);
      }
      part.set("material", std::move(*m));
    }
    if (!j.get("bodyInfo").is_undefined() && !j.get("bodyInfo").is_null()) {
      Value bi = read_body_info(j.get("bodyInfo"), c);
      if (bi.truthy()) {
        part.set("bodyInfo", std::move(bi));
      }
    }
    if (!j.get("params").is_undefined() && !j.get("params").is_null()) {
      auto pp = pp::read_part_params(j.get("params"), part, w);
      if (!pp) {
        fail(pp.error().message);
      }
      if (pp->truthy()) {
        part.set("params", std::move(*pp));
      }
    }
    if (!j.get("configs").is_undefined() && !j.get("configs").is_null()) {
      auto cf = configs::read_configs(j.get("configs"), part.get("params"), w);
      if (!cf) {
        fail(cf.error().message);
      }
      if (cf->truthy()) {
        part.set("configs", std::move(*cf));
      }
    }
    const Value rb = j.get("rollback");
    if (rb.is_null() || rb.is_undefined()) {
      part.set("rollback", nullptr);
    } else if (is_int(rb) && rb.as_number() >= 0 && rb.as_number() < static_cast<double>(features_of(part).size())) {
      part.set("rollback", rb);
    }
    return part;
  });
}

}  // namespace cc::partcraft::part
