// SPDX-License-Identifier: Apache-2.0
// project.js (CivilCraft 0.5) and the parts of gis-state.js and block-points.js the project file reads, ported
// function by function. Snapshots are JSON.stringify of the state (base::js_stringify: JS property order).
#include "cc/civil/project.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "cc/appkit/jsops.hpp"
#include "cc/base/js.hpp"
#include "cc/base/json.hpp"
#include "cc/civil/codes.hpp"
#include "cc/civil/deeds.hpp"
#include "cc/civil/design.hpp"
#include "cc/civil/drain.hpp"
#include "cc/civil/featureline.hpp"
#include "cc/civil/jsmap.hpp"
#include "cc/civil/sections.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::make_object;
using base::Value;
constexpr std::size_t kMaxUndo = 200;

double num(const Value& v) {
  return base::js_to_number(v);
}
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
bool fin(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number());
}
bool is_obj(const Value& v) {  // typeof v === 'object' && v (arrays too)
  return v.is_object() || v.is_array();
}
std::string sv(const Value& v) {
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
/// project.js str(v, n): String(v ?? '') with control characters as spaces, cut to n units (not trimmed)
std::string pstr(const Value& v, std::size_t n = 200) {
  std::string s = sv(v);
  for (char& c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) {
      c = ' ';
    }
  }
  return slice16(s, n);
}
/// trimmed (gis-state.js clean, block-points.js str)
std::string tclean(const Value& v, std::size_t n) {
  std::string s = sv(v);
  for (char& c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) {
      c = ' ';
    }
  }
  return slice16(std::string(base::js_trim(s)), n);
}
Value xy(const Value& q) {
  if (!(q.truthy() && fin(get(q, "x")) && fin(get(q, "y")))) {
    return nullptr;
  }
  return base::pt2(get(q, "x").as_number(), get(q, "y").as_number());
}
Value xyz(const Value& q) {
  if (!(q.truthy() && fin(get(q, "x")) && fin(get(q, "y")) && fin(get(q, "z")))) {
    return nullptr;
  }
  return base::pt3(get(q, "x").as_number(), get(q, "y").as_number(), get(q, "z").as_number());
}
template <class F>
Array list(const Value& a, F f, double max) {
  Array out;
  if (!a.is_array()) {
    return out;
  }
  const Array& in = a.as_array();
  for (std::size_t i = 0; i < in.size() && static_cast<double>(i) < max; ++i) {
    Value v = f(in[i]);
    if (v.truthy()) {
      out.push_back(std::move(v));
    }
  }
  return out;
}
/// 0.5 < scale < 2 (NaN: no)
bool scale_ok(const Value& v) {
  const double s = v.as_number();
  return s > 0.5 && s < 2;
}
bool id_like(const std::string& s) {  // /^[a-z]+\d+$/
  std::size_t i = 0;
  while (i < s.size() && s[i] >= 'a' && s[i] <= 'z') {
    ++i;
  }
  if (i == 0 || i == s.size()) {
    return false;
  }
  for (std::size_t j = i; j < s.size(); ++j) {
    if (s[j] < '0' || s[j] > '9') {
      return false;
    }
  }
  return true;
}

Value report_value(const TinReport& r, bool faces) {
  Array notes;
  for (const auto& n : r.notes) {
    notes.emplace_back(n);
  }
  Array errors;
  for (const auto& e : r.errors) {
    errors.emplace_back(e);
  }
  Value o = make_object({{"notes", std::move(notes)}, {"errors", std::move(errors)}});
  if (!faces) {
    o.set("skippedNoZ", r.skipped_no_z);
    o.set("merged", r.merged);
    o.set("crossings", r.crossings);
    o.set("zConflicts", r.z_conflicts);
    o.set("constraintFailures", r.constraint_failures);
    if (r.breakline_segments) {
      o.set("breaklineSegments", *r.breakline_segments);
    }
  }
  return o;
}

std::vector<Pi> pis_of(const Value& v) {
  std::vector<Pi> out;
  for (const Value& p : arr(v)) {
    const Value x = get(p, "x");
    const Value y = get(p, "y");
    out.push_back(
        {x.is_number() ? x.as_number() : std::nan(""), y.is_number() ? y.as_number() : std::nan(""), get(p, "r")});
  }
  return out;
}

/// gis-state.js readGisState
std::pair<Value, Value> read_gis_state(const Value& gis, const Value& gis_features, Array& notes,
                                       const i18n::Catalog& w) {
  Value out = make_object({{"layers", Array{}}, {"maps", Array{}}});
  Value features = Value::object();
  if (gis.is_null() || gis.is_undefined()) {
    return {out, features};
  }
  if (!is_obj(gis)) {
    notes.emplace_back(w.t("gstate.unreadable"));
    return {out, features};
  }
  double budget = 2e7;
  double total = 0;
  double dropped = 0;
  auto pos = [](const Value& p) {
    if (!p.is_array() || (p.as_array().size() != 2 && p.as_array().size() != 3)) {
      return false;
    }
    const Array& a = p.as_array();
    return std::ranges::all_of(a, fin) && std::abs(a[0].as_number()) < 1e10 && std::abs(a[1].as_number()) < 1e10;
  };
  auto P = [&](const Value& p) {
    budget--;
    const Array& a = p.as_array();
    return a.size() == 3 ? Value(Array{a[0], a[1], a[2]}) : Value(Array{a[0], a[1]});
  };
  // nullopt = undefined (a bad geometry), Value null = no geometry
  auto geom = [&](const Value& g, const std::string& kind) -> std::optional<Value> {
    if (g.is_null()) {
      return Value(nullptr);
    }
    if (!g.truthy() || !g.is_object() || !(get(g, "t").is_string() && get(g, "t").as_string() == kind) ||
        !get(g, "c").is_array()) {
      return std::nullopt;
    }
    const Array& c = get(g, "c").as_array();
    if (kind == "pt") {
      if (!pos(get(g, "c"))) {
        return std::nullopt;
      }
      return make_object({{"t", "pt"}, {"c", P(get(g, "c"))}});
    }
    if (kind == "ln") {
      Array parts;
      for (const Value& q : c) {
        if (q.is_array() && q.as_array().size() >= 2 && static_cast<double>(q.as_array().size()) <= budget &&
            std::ranges::all_of(q.as_array(), pos)) {
          Array part;
          for (const Value& p : q.as_array()) {
            part.push_back(P(p));
          }
          parts.emplace_back(std::move(part));
        }
      }
      if (parts.empty()) {
        return std::nullopt;
      }
      return make_object({{"t", "ln"}, {"c", std::move(parts)}});
    }
    Array polys;
    for (const Value& poly : c) {
      if (!poly.is_array() || poly.as_array().empty()) {
        continue;
      }
      Array rings;
      for (const Value& r : poly.as_array()) {
        if (r.is_array() && r.as_array().size() >= 4 && static_cast<double>(r.as_array().size()) <= budget &&
            std::ranges::all_of(r.as_array(), pos)) {
          Array ring;
          for (const Value& p : r.as_array()) {
            ring.push_back(P(p));
          }
          rings.emplace_back(std::move(ring));
        }
      }
      if (!rings.empty()) {
        polys.emplace_back(std::move(rings));
      }
    }
    if (polys.empty()) {
      return std::nullopt;
    }
    return make_object({{"t", "pg"}, {"c", std::move(polys)}});
  };
  const Value src = is_obj(gis_features) ? gis_features : Value::object();
  std::set<std::string> seen;
  const Value layers = get(gis, "layers");
  const Array& ls = arr(layers);
  for (std::size_t li = 0; li < ls.size() && li < 200; ++li) {
    const Value& l = ls[li];
    const std::string lid = base::js_string(get(l, "id"));
    bool lid_ok = lid.size() >= 2 && lid.size() <= 10 && lid[0] == 'g' &&
                  std::all_of(lid.begin() + 1, lid.end(), [](char c) { return c >= '0' && c <= '9'; });
    const Value gk = get(l, "geom");
    const bool geom_ok = gk.is_string() && (gk.as_string() == "pt" || gk.as_string() == "ln" || gk.as_string() == "pg");
    const Value idv = get(l, "id");
    if (!is_obj(l) || !lid_ok || seen.contains(map_key(idv)) || !geom_ok) {
      dropped++;
      continue;
    }
    seen.insert(map_key(idv));
    std::set<std::string> field_names;
    Array fields;
    const Array& fs_in = arr(get(l, "fields"));
    for (std::size_t i = 0; i < fs_in.size() && i < 500; ++i) {
      const Value& f = fs_in[i];
      const std::string name = tclean(get(f, "name"), 64);
      const Value ty = get(f, "type");
      const bool ty_ok = ty.is_string() && (ty.as_string() == "text" || ty.as_string() == "number" ||
                                            ty.as_string() == "date" || ty.as_string() == "bool");
      if (!name.empty() && !field_names.contains(name)) {
        field_names.insert(name);
        fields.push_back(make_object({{"name", name}, {"type", ty_ok ? ty : Value("text")}}));
      }
    }
    const Value srcv = get(l, "source");
    const Value color = get(l, "color");
    static constexpr std::array<std::string_view, 7> kTokens{"blue",  "orange", "magenta", "cyan",
                                                             "ochre", "green",  "red"};
    const bool color_ok =
        color.is_string() && std::ranges::find(kTokens, std::string_view(color.as_string())) != kTokens.end();
    const std::string lname = tclean(get(l, "name"), 120);
    Value layer = make_object(
        {{"id", idv},
         {"name", lname.empty() ? w.t("layers.default") : lname},
         {"geom", gk},
         {"hasZ", get(l, "hasZ").is_bool() && get(l, "hasZ").as_bool()},
         {"fields", fields},
         {"visible", !get(l, "visible").is_bool() || get(l, "visible").as_bool()},
         {"color", color_ok ? color : Value("blue")},
         {"source",
          make_object({{"file", tclean(get(srcv, "file"), 200)},
                       {"format", tclean(get(srcv, "format"), 40)},
                       {"crs", get(srcv, "crs").truthy() ? Value(tclean(get(srcv, "crs"), 200)) : Value(nullptr)}})}});
    const Value label = get(l, "label");
    if (is_obj(label) && field_names.contains(tclean(get(label, "field"), 64))) {
      layer.set("label", make_object({{"field", tclean(get(label, "field"), 64)}}));
    }
    const Value* rows_v = src.is_object() ? src.find(lid) : nullptr;
    const Array& rows = rows_v != nullptr && rows_v->is_array() ? rows_v->as_array() : arr(Value());
    Array fs;
    double bad = 0;
    for (const Value& r : rows) {
      if (total >= 2e6 || budget <= 0) {
        bad++;
        continue;
      }
      if (!r.is_array() || r.as_array().size() != 2) {
        bad++;
        continue;
      }
      const auto g = geom(r.as_array()[0], gk.as_string());
      if (!g) {
        bad++;
        continue;
      }
      Value p = Value::object();
      const Value& props = r.as_array()[1];
      if (props.truthy() && props.is_object()) {
        for (const Value& f : fields) {
          const std::string fname = f.get("name").as_string();
          const Value* v = props.find(fname);
          if (v == nullptr) {
            continue;
          }
          Value pv = nullptr;
          if (v->is_null() || fin(*v) || v->is_bool()) {
            pv = *v;
          } else if (v->is_string()) {
            std::string s = v->as_string();
            for (char& c : s) {
              const auto u = static_cast<unsigned char>(c);
              if ((u <= 0x08) || u == 0x0B || u == 0x0C || (u >= 0x0E && u <= 0x1F) || u == 0x7F) {
                c = ' ';
              }
            }
            pv = slice16(s, 10000);
          }
          p.set(fname, pv);
        }
      }
      fs.push_back(make_object({{"g", *g}, {"p", std::move(p)}}));
      total++;
    }
    if (bad != 0) {
      notes.emplace_back(
          w.tn("gstate.bad.features", bad, {{"n", appkit::num_str(bad)}, {"layer", layer.get("name").as_string()}}));
    }
    out.find("layers")->push(std::move(layer));
    features.set(lid, std::move(fs));
  }
  if (dropped != 0) {
    notes.emplace_back(w.tn("gstate.bad.layers", dropped, {{"n", appkit::num_str(dropped)}}));
  }
  const Value maps = get(gis, "maps");
  if (maps.is_array()) {
    const Array& ms = maps.as_array();
    for (std::size_t i = 0; i < ms.size() && i < 100; ++i) {
      const Value& m = ms[i];
      if (!m.truthy() || !m.is_object()) {
        continue;
      }
      Value o = Value::object();
      std::size_t taken = 0;
      for (const base::Member* mem : base::js_members(m.as_object())) {
        if (taken++ >= 50) {
          break;
        }
        if (mem->key == "__proto__" || base::utf16_length(mem->key) > 40) {
          continue;
        }
        const Value& v = mem->value;
        if (v.is_string()) {
          o.set(mem->key, base::utf16_length(v.as_string()) > 40000000 ? Value("") : v);
        } else if (v.is_bool() || fin(v) || v.is_null()) {
          o.set(mem->key, v);
        }
      }
      out.find("maps")->push(std::move(o));
    }
  }
  return {out, features};
}

Value clean_blk(const Value& raw) {
  if (!raw.truthy() || !raw.is_object() || !get(raw, "name").is_string()) {
    return {};
  }
  return make_object({{"name", tclean(get(raw, "name"), 255)},
                      {"layer", tclean(get(raw, "layer"), 255)},
                      {"handle", tclean(get(raw, "handle"), 40)}});
}

std::optional<std::vector<std::uint8_t>> atob(const std::string& v) {
  std::string s = v;
  while (!s.empty() && s.back() == '=') {
    s.pop_back();
  }
  if (s.size() % 4 == 1 || v.size() - s.size() > 2) {
    return std::nullopt;  // the JS's atob throws
  }
  return base::unbase64(v);
}

}  // namespace

Value display_defaults() {
  return make_object({{"surface", "eg"},
                      {"triangles", false},
                      {"contours", true},
                      {"interval", 1},
                      {"major", 5},
                      {"labels", true},
                      {"bands", false},
                      {"bandCount", 8},
                      {"slopes", false},
                      {"points", true},
                      {"pointLabels", false},
                      {"breaklines", true},
                      {"outline", true},
                      {"pipes", true},
                      {"profFeatures", true},
                      {"profWithin", 0},
                      {"gradeOverlay", false}});
}

Value empty_state(const i18n::Catalog& w) {
  return make_object({{"name", w.t("project.untitled")},
                      {"units", "ft"},
                      {"surface", make_object({{"name", w.t("surface.eg")},
                                               {"groups", Value::object()},
                                               {"breaklines", Array{}},
                                               {"boundary", nullptr},
                                               {"maxEdge", 0},
                                               {"tin", nullptr},
                                               {"descKeys", Array{}},
                                               {"lineCodes", clean_line_codes(Value())}})},
                      {"display", display_defaults()},
                      {"alignments", Array{}},
                      {"pads", Array{}},
                      {"network", nullptr},
                      {"seq", 1},
                      {"crs", nullptr},
                      {"features", Array{}},
                      {"parcels", Array{}},
                      {"gis", make_object({{"layers", Array{}}, {"maps", Array{}}})}});
}

Project::Project(const i18n::Catalog& words) : state(empty_state(words)), words_(&words) {}

template <class T, class F>
std::shared_ptr<T> Project::memo(const std::string& key, const std::string& deps, F f) {
  if (const auto it = cache_.find(key); it != cache_.end() && it->second.first == deps) {
    return std::static_pointer_cast<T>(it->second.second);
  }
  std::shared_ptr<T> v = f();
  cache_.insert_or_assign(key, std::pair{deps, std::static_pointer_cast<void>(v)});
  return v;
}

std::string Project::snapshot() const {
  Value s = Value::object();
  for (const auto& m : state.as_object()) {
    if (m.key == "display") {
      continue;
    }
    if (m.key == "surface" && m.value.is_object()) {
      Value surf = Value::object();
      for (const auto& sm : m.value.as_object()) {
        if (sm.key != "tin") {
          surf.append(sm.key, sm.value);
        }
      }
      s.append("surface", std::move(surf));
      continue;
    }
    s.append(m.key, m.value);
  }
  return base::js_stringify(s);
}

void Project::restore(const std::string& json) {
  auto parsed = base::parse_json(json);
  if (!parsed) {
    return;
  }
  Value s = std::move(*parsed);
  const Value tin = get(get(state, "surface"), "tin");
  if (Value* surf = s.find("surface"); surf != nullptr && surf->is_object()) {
    surf->set("tin", tin);
  }
  s.set("display", get(state, "display"));
  state = std::move(s);
}

void Project::set_display(const std::string& k, const Value& v) {
  Value* d = state.find("display");
  if (d == nullptr || !d->is_object()) {
    return;
  }
  if (strict_equal(d->get(k), v)) {
    return;
  }
  d->set(k, v);
  touch();
}

bool Project::change(const std::string& label, const std::function<void(Value&)>& fn) {
  const std::string before = snapshot();
  fn(state);
  if (snapshot() == before) {
    return false;
  }
  undo_stack.push_back({label, before, std::nullopt, std::nullopt, false, Value()});
  if (undo_stack.size() > kMaxUndo) {
    undo_stack.erase(undo_stack.begin());
  }
  redo_stack.clear();
  touch();
  return true;
}

bool Project::edit_points(const std::string& label, const std::function<std::optional<Value>(Value, Value&)>& fn) {
  const std::string before = snapshot();
  const Value was = points;
  auto next = fn(was, state);
  if (!next || !next->is_array()) {
    return false;
  }
  Value* g = state.find("surface") != nullptr ? state.find("surface")->find("groups") : nullptr;
  const Value keys = get(get(state, "surface"), "descKeys");
  std::vector<std::pair<std::string, double>> grp;
  {
    std::map<std::string, std::size_t> at;
    for (const Value& p : next->as_array()) {
      const std::string k = group_of(keys.truthy() ? keys : Value::array(), p);
      if (!at.contains(k)) {
        at.emplace(k, grp.size());
        grp.emplace_back(k, 0);
      }
    }
  }
  appkit::js_sort(
      grp, [](const auto& a, const auto& b) { return static_cast<double>(base::js_locale_compare(a.first, b.first)); });
  if (g != nullptr && g->is_object()) {
    for (const auto& [k, n] : grp) {
      if (!g->has(k)) {
        g->set(k, default_use(k));
      }
    }
  }
  points = std::move(*next);
  points_version++;
  undo_stack.push_back({label, before, was, std::nullopt, false, Value()});
  if (undo_stack.size() > kMaxUndo) {
    undo_stack.erase(undo_stack.begin());
  }
  redo_stack.clear();
  touch();
  return true;
}

bool Project::set_tin(const std::string& label, const Value& tin, const std::function<void(Value&)>* fn) {
  const std::string before = snapshot();
  const Value was = get(get(state, "surface"), "tin");
  if (fn != nullptr) {
    (*fn)(state);
  }
  if (Value* s = state.find("surface"); s != nullptr && s->is_object()) {
    s->set("tin", tin);
  }
  undo_stack.push_back({label, before, std::nullopt, std::nullopt, true, was});
  if (undo_stack.size() > kMaxUndo) {
    undo_stack.erase(undo_stack.begin());
  }
  redo_stack.clear();
  cache_.clear();
  touch();
  return true;
}

namespace {
std::optional<std::string> step(Project& p, std::vector<Project::Step>& from, std::vector<Project::Step>& to,
                                std::map<std::string, std::pair<std::string, std::shared_ptr<void>>>& cache) {
  if (from.empty()) {
    return std::nullopt;
  }
  Project::Step u = std::move(from.back());
  from.pop_back();
  Project::Step back{u.label,
                     p.snapshot(),
                     u.pts ? std::optional(p.points) : std::nullopt,
                     u.gis ? std::optional(p.gis_features) : std::nullopt,
                     u.tin_set,
                     u.tin_set ? get(get(p.state, "surface"), "tin") : Value()};
  to.push_back(std::move(back));
  p.restore(u.json);
  if (u.tin_set) {
    if (Value* s = p.state.find("surface"); s != nullptr && s->is_object()) {
      s->set("tin", u.tin);
    }
    cache.clear();
  }
  if (u.pts) {
    p.points = std::move(*u.pts);
    p.points_version++;
  }
  if (u.gis) {
    p.gis_features = std::move(*u.gis);
    p.gis_version++;
  }
  p.version++;
  return u.label;
}
}  // namespace

std::optional<std::string> Project::undo() {
  return step(*this, undo_stack, redo_stack, cache_);
}
std::optional<std::string> Project::redo() {
  return step(*this, redo_stack, undo_stack, cache_);
}

std::string Project::next_id(const std::string& prefix) {
  const double seq = num(state.get("seq"));
  state.set("seq", seq + 1);
  return prefix + appkit::num_str(seq);
}

Value Project::default_use(const std::string& group) const {
  for (const Value& x : arr(get(get(state, "surface"), "descKeys"))) {
    const Value g = get(x, "group");
    const Value name = g.truthy() ? g : get(x, "code");
    if (name.is_string() && name.as_string() == group && get(x, "use").truthy()) {
      return get(x, "use");
    }
  }
  return "points";
}

void Project::set_points(const Value& pts, const std::string& file) {
  points = pts.is_array() ? pts : Value::array();
  points_file = file;
  points_version++;
  Value* g = state.find("surface") != nullptr ? state.find("surface")->find("groups") : nullptr;
  const Value keys = get(get(state, "surface"), "descKeys");
  const Value k2 = keys.truthy() ? keys : Value::array();
  std::vector<std::string> now;
  std::set<std::string> now_set;
  for (const Value& p : points.as_array()) {
    const std::string k = group_of(k2, p);
    if (now_set.insert(k).second) {
      now.push_back(k);
    }
  }
  if (g != nullptr && g->is_object()) {
    std::vector<std::string> drop;
    for (const base::Member* m : base::js_members(g->as_object())) {
      if (!now_set.contains(m->key)) {
        drop.push_back(m->key);
      }
    }
    for (const auto& k : drop) {
      g->erase(k);
    }
    std::vector<std::string> sorted = now;
    appkit::js_sort(sorted, [](const std::string& a, const std::string& b) {
      return static_cast<double>(base::js_locale_compare(a, b));
    });
    for (const auto& k : sorted) {
      if (!g->has(k)) {
        g->set(k, default_use(k));
      }
    }
  }
  undo_stack.clear();
  redo_stack.clear();
  touch();
}

Value Project::groups() {
  const Value keys = get(get(state, "surface"), "descKeys");
  const Value k2 = keys.truthy() ? keys : Value::array();
  std::vector<std::pair<std::string, double>> grp;
  std::map<std::string, std::size_t> at;
  for (const Value& p : points.as_array()) {
    const std::string k = group_of(k2, p);
    if (const auto it = at.find(k); it != at.end()) {
      grp[it->second].second++;
    } else {
      at.emplace(k, grp.size());
      grp.emplace_back(k, 1);
    }
  }
  appkit::js_sort(
      grp, [](const auto& a, const auto& b) { return static_cast<double>(base::js_locale_compare(a.first, b.first)); });
  const Value g = get(get(state, "surface"), "groups");
  Array out;
  for (const auto& [k, n] : grp) {
    const Value u = get(g, k);
    out.push_back(make_object({{"key", k}, {"count", n}, {"use", u.truthy() ? u : Value("points")}}));
  }
  return out;
}

bool Project::set_coding(const Value& keys, const Value& line_codes, const std::string& label) {
  return change(label, [&](Value& s) {
    Value* surf = s.find("surface");
    if (surf == nullptr || !surf->is_object()) {
      return;
    }
    surf->set("descKeys", clean_desc_keys(keys));
    surf->set("lineCodes", clean_line_codes(line_codes));
    const Value dk = surf->get("descKeys");
    std::vector<std::string> now;
    std::set<std::string> now_set;
    for (const Value& p : points.as_array()) {
      const std::string k = group_of(dk, p);
      if (now_set.insert(k).second) {
        now.push_back(k);
      }
    }
    Value* g = surf->find("groups");
    if (g == nullptr || !g->is_object()) {
      return;
    }
    std::vector<std::string> drop;
    for (const base::Member* m : base::js_members(g->as_object())) {
      if (!now_set.contains(m->key)) {
        drop.push_back(m->key);
      }
    }
    for (const auto& k : drop) {
      g->erase(k);
    }
    for (const auto& k : now) {
      if (g->has(k)) {
        continue;
      }
      Value use = "points";
      for (const Value& x : arr(dk)) {
        const Value gg = get(x, "group");
        const Value name = gg.truthy() ? gg : get(x, "code");
        if (name.is_string() && name.as_string() == k && get(x, "use").truthy()) {
          use = get(x, "use");
          break;
        }
      }
      g->set(k, use);
    }
  });
}

Value Project::figures(const std::vector<std::string>* only) {
  Array pts;
  for (const Value& p : points.as_array()) {
    if (!get(p, "z").is_null()) {
      pts.push_back(p);
    }
  }
  const Value s = get(state, "surface");
  const Value keys = get(s, "descKeys");
  const Value lc = get(s, "lineCodes");
  return civil::figures(Value(std::move(pts)), keys.truthy() ? keys : Value::array(),
                        lc.truthy() ? lc : clean_line_codes(Value()), only);
}

std::string Project::surface_deps() const {
  const Value s = get(state, "surface");
  Value rest = Value::object();
  for (const auto& m : s.as_object()) {
    if (m.key != "tin") {
      rest.append(m.key, m.value);
    }
  }
  const Value tin = get(s, "tin");
  std::string t;
  if (tin.truthy()) {
    t = appkit::num_str(static_cast<double>(arr(get(tin, "pts")).size())) + ":" +
        appkit::num_str(static_cast<double>(arr(get(tin, "faces")).size()));
  }
  Array eg_features;
  for (const Value& f : arr(get(state, "features"))) {
    if (get(f, "use").is_string() && get(f, "use").as_string() == "eg") {
      eg_features.push_back(f);
    }
  }
  return appkit::num_str(points_version) + "|" + t + "|" + base::js_stringify(rest) + "|" +
         base::js_stringify(Value(std::move(eg_features)));
}

std::string Project::grade_deps() const {
  Array on;
  for (const Value& a : arr(get(state, "alignments"))) {
    if (get(get(a, "corridor"), "on").truthy()) {
      on.push_back(a);
    }
  }
  Array design;
  for (const Value& f : arr(get(state, "features"))) {
    if (get(f, "use").is_string() && get(f, "use").as_string() == "design") {
      design.push_back(f);
    }
  }
  return base::js_stringify(Value(Array{get(state, "pads"), Value(std::move(on)), Value(std::move(design))}));
}

Value Project::eg_breaklines() {
  Array out;
  std::vector<std::string> bl;
  for (const base::Member* m : base::js_members(get(get(state, "surface"), "groups").as_object())) {
    if (m->value.is_string() && m->value.as_string() == "breakline") {
      bl.push_back(m->key);
    }
  }
  if (!bl.empty()) {
    for (const Value& f : arr(figures(&bl))) {
      Array pts;
      const Array& fp = arr(f.get("pts"));
      for (const Value& p : fp) {
        pts.push_back(base::pt3(num(get(p, "x")), num(get(p, "y")), num(get(p, "z"))));
      }
      if (f.get("closed").truthy() && !fp.empty()) {
        pts.push_back(base::pt3(num(get(fp[0], "x")), num(get(fp[0], "y")), num(get(fp[0], "z"))));
      }
      out.push_back(make_object({{"name", f.get("code")}, {"pts", std::move(pts)}, {"fromGroup", true}}));
    }
  }
  for (const Value& b : arr(get(get(state, "surface"), "breaklines"))) {
    out.push_back(b);
  }
  for (const Value& b : arr(feature_breaklines(get(state, "features"), Value("eg")))) {
    out.push_back(b);
  }
  return out;
}

Value Project::eg_points() {
  const Value g = get(get(state, "surface"), "groups");
  const Value keys = get(get(state, "surface"), "descKeys");
  const Value k2 = keys.truthy() ? keys : Value::array();
  Array out;
  for (const Value& p : points.as_array()) {
    if (get(p, "z").is_null()) {
      continue;
    }
    const Value use = get(g, group_of(k2, p));
    const std::string u = use.truthy() ? base::js_string(use) : std::string("points");
    if (u == "points") {
      out.push_back(p);
    }
  }
  return out;
}

const Project::Eg& Project::eg() {
  return *memo<Eg>("eg", surface_deps(), [&] {
    auto r = std::make_shared<Eg>();
    const Value s = get(state, "surface");
    const Value tin = get(s, "tin");
    if (tin.truthy()) {
      FaceInput fi;
      for (const Value& p : arr(get(tin, "pts"))) {
        if (p.truthy()) {
          fi.pts.emplace_back(P3{num(get(p, "x")), num(get(p, "y")), num(get(p, "z"))});
        } else {
          fi.pts.emplace_back(std::nullopt);
        }
      }
      for (const Value& f : arr(get(tin, "faces"))) {
        std::vector<double> ids;
        for (const Value& i : arr(f)) {
          ids.push_back(i.is_number() ? i.as_number() : std::nan(""));
        }
        fi.faces.push_back(std::move(ids));
      }
      Built b = surface_from_faces(fi, *words_);
      r->surface = b.surface;
      r->report = report_value(b.report, true);
      return r;
    }
    const bool any_eg = std::ranges::any_of(arr(get(state, "features")), [](const Value& f) {
      return get(f, "use").is_string() && get(f, "use").as_string() == "eg";
    });
    if (points.as_array().empty() && arr(get(s, "breaklines")).empty() && !any_eg) {
      r->report = make_object({{"notes", Array{}}, {"errors", Array{}}});
      return r;
    }
    Value in =
        make_object({{"points", eg_points()}, {"breaklines", eg_breaklines()}, {"boundary", get(s, "boundary")}});
    BuildInput bi = build_input_from(in.get("points"), in.get("breaklines"));
    const Value bnd = get(s, "boundary");
    if (bnd.truthy()) {
      std::vector<P2> b;
      for (const Value& p : arr(bnd)) {
        b.push_back({num(get(p, "x")), num(get(p, "y"))});
      }
      bi.boundary = std::move(b);
    }
    const double me = num(get(s, "maxEdge"));
    bi.max_edge = (std::isnan(me) || me == 0) ? 0 : me;
    Built b = build_surface(bi, *words_);
    r->surface = b.surface;
    r->report = report_value(b.report, false);
    return r;
  });
}

Value Project::contours() {
  const auto& e = eg();
  if (!e.surface) {
    return Array{};
  }
  const double iv = num(get(get(state, "display"), "interval"));
  const auto c = e.surface->contours((std::isnan(iv) || iv == 0) ? 1 : iv);
  Array out;
  if (!c) {
    return out;
  }
  for (const Contour& k : *c) {
    Array pts;
    for (const P2& p : k.pts) {
      pts.push_back(base::pt2(p.x, p.y));
    }
    out.push_back(make_object({{"z", k.z}, {"pts", std::move(pts)}, {"closed", k.closed}}));
  }
  return out;
}

std::shared_ptr<Alignment> Project::alignment(const Value& a) {
  return memo<Alignment>("al:" + base::js_string(get(a, "id")),
                         base::js_stringify(Value(Array{get(a, "pis"), get(a, "start")})), [&] {
                           const double st = num(get(a, "start"));
                           return build_alignment(pis_of(get(a, "pis")), (std::isnan(st) || st == 0) ? 0 : st, *words_);
                         });
}

Value Project::eg_profile(const Value& a) {
  const auto al = alignment(a);
  const auto& e = eg();
  Array out;
  for (const auto& g : sample_surface(e.surface.get(), *al, std::nullopt)) {
    out.push_back(make_object({{"sta", g.sta}, {"z", g.z ? Value(*g.z) : Value(nullptr)}}));
  }
  return out;
}

std::shared_ptr<Profile> Project::design_profile(const Value& a) {
  return memo<Profile>("fg:" + base::js_string(get(a, "id")), base::js_stringify(get(a, "pvis")), [&] {
    const Value pv = get(a, "pvis");
    return build_profile(pv.truthy() ? pv : Value::array(), *words_);
  });
}

const Project::Graded& Project::graded() {
  return *memo<Graded>("graded", surface_deps() + "|" + grade_deps(), [&] {
    auto out = std::make_shared<Graded>();
    const auto eg_surface = eg().surface;
    // the items: corridors, closed design feature lines, pads
    struct Item {
      Value id, kind, name;
      GradeItem g;
    };
    std::vector<Item> items;
    const std::string units = base::js_string(get(state, "units"));
    for (const Value& a : arr(get(state, "alignments"))) {
      if (!get(get(a, "corridor"), "on").truthy()) {
        continue;
      }
      auto al = alignment(a);
      auto prof = design_profile(a);
      Value asm_ = default_assembly(units == "m" ? "m" : "ft");
      for (const base::Member* m : base::js_members(get(a, "corridor").as_object())) {
        asm_.set(m->key, m->value);
      }
      const std::string name = words_->t("project.corridor", {{"name", sv(get(a, "name"))}});
      const i18n::Catalog* w = words_;
      GradeItem g;
      g.name = name;
      g.patch = [al, prof, asm_, w](const Surface& sf) { return corridor_patch(&sf, al.get(), prof.get(), asm_, *w); };
      items.push_back({get(a, "id"), "corridor", name, std::move(g)});
    }
    for (const Value& f : arr(get(state, "features"))) {
      const Value it = feature_item(f);
      if (!it.truthy()) {
        continue;
      }
      if (it.has("patch")) {
        GradeItem g;
        g.name = sv(it.get("name"));
        g.patch = [patch = it.get("patch")](const Surface&) { return patch; };
        items.push_back({it.get("id"), "feature", it.get("name"), std::move(g)});
      } else {
        items.push_back({it.get("id"), "feature", it.get("name"), GradeItem{it, {}, {}}});
      }
    }
    for (const Value& p : arr(get(state, "pads"))) {
      if (arr(get(p, "pts")).size() < 3) {
        continue;
      }
      Value q = p;
      q.set("kind", "pad");
      q.set("z", num(get(p, "z")));
      q.set("cut", num(get(p, "cut")));
      q.set("fill", num(get(p, "fill")));
      items.push_back({get(p, "id"), "pad", get(p, "name"), GradeItem{q, {}, {}}});
    }
    if (!eg_surface || items.empty()) {
      out->report = make_object({{"notes", Array{}}, {"errors", Array{}}, {"items", Array{}}});
      out->regions = Value::array();
      out->items = Value::array();
      return out;
    }
    const Value s = get(state, "surface");
    const bool tin = get(s, "tin").truthy();
    Value base_pts;
    Value base_bl;
    if (tin) {
      Array pts;
      for (const P3& p : eg_surface->to_faces().pts) {
        pts.push_back(base::pt3(p.x, p.y, p.z));
      }
      base_pts = std::move(pts);
      base_bl = Value::array();
    } else {
      base_pts = eg_points();
      base_bl = eg_breaklines();
    }
    std::vector<GradeItem> gi;
    gi.reserve(items.size());
    for (const Item& it : items) {
      gi.push_back(it.g);
    }
    const i18n::Catalog* w = words_;
    auto build = [s, tin, w](const Value& pts, const Value& bls) {
      BuildInput bi = build_input_from(pts, bls);
      if (!tin && get(s, "boundary").truthy()) {
        std::vector<P2> b;
        for (const Value& p : arr(get(s, "boundary"))) {
          b.push_back({num(get(p, "x")), num(get(p, "y"))});
        }
        bi.boundary = std::move(b);
      }
      const double me = tin ? 0 : num(get(s, "maxEdge"));
      bi.max_edge = (std::isnan(me) || me == 0) ? 0 : me;
      return build_surface(bi, *w);
    };
    civil::Graded r = graded_surface(eg_surface, base_pts, base_bl, gi, *w, BuildFn(build));
    out->surface = r.surface;
    out->report = r.report;
    out->regions = r.regions;
    Array its;
    const Array& infos = arr(r.items);
    const Array& rep_items = arr(r.report.get("items"));
    for (std::size_t i = 0; i < infos.size(); ++i) {
      Value o = make_object({{"id", items[i].id},
                             {"kind", items[i].kind},
                             {"name", items[i].name},
                             {"problems", rep_items[i].get("problems")},
                             {"region", arr(r.regions)[i]}});
      if (infos[i].is_object() && !infos[i].is_point()) {
        for (const base::Member* m : base::js_members(infos[i].as_object())) {
          o.set(m->key, m->value);
        }
      }
      its.push_back(std::move(o));
    }
    out->items = std::move(its);
    return out;
  });
}

std::shared_ptr<Value> Project::sections(const Value& a) {
  if (!get(a, "sampling").truthy()) {
    return nullptr;
  }
  const auto al = alignment(a);
  if (!al->ok()) {
    return nullptr;
  }
  return memo<Value>("sec:" + base::js_string(get(a, "id")),
                     surface_deps() + "|" + grade_deps() + "|" +
                         base::js_stringify(Value(Array{get(a, "pis"), get(a, "start"), get(a, "sampling")})),
                     [&] {
                       const auto eg_surface = eg().surface;
                       const Graded& g = graded();
                       Value cor;
                       for (const Value& x : arr(g.items)) {
                         if (get(x, "kind").is_string() && get(x, "kind").as_string() == "corridor" &&
                             strict_equal(get(x, "id"), get(a, "id"))) {
                           cor = x;
                           break;
                         }
                       }
                       SectionInput in;
                       in.alignment = al.get();
                       in.ground = eg_surface.get();
                       in.design = g.surface ? g.surface.get() : eg_surface.get();
                       std::vector<std::vector<P2>> regions;
                       for (const Value& r : arr(g.regions)) {
                         if (!r.truthy()) {
                           continue;
                         }
                         std::vector<P2> poly;
                         for (const Value& p : arr(r)) {
                           poly.push_back({num(get(p, "x")), num(get(p, "y"))});
                         }
                         regions.push_back(std::move(poly));
                       }
                       in.regions = std::move(regions);
                       const Value lines = get(cor, "lines");
                       in.corridor = lines.truthy() ? lines : Value(nullptr);
                       in.sampling = get(a, "sampling");
                       return std::make_shared<Value>(build_sections(in));
                     });
}

Value Project::volumes(const Value& cell) {
  const Graded& g = graded();
  const auto eg_surface = eg().surface;
  if (!g.surface || !eg_surface) {
    return nullptr;
  }
  Array regions;
  for (const Value& r : arr(g.regions)) {
    if (r.truthy()) {
      regions.push_back(r);
    }
  }
  if (regions.empty()) {
    return nullptr;
  }
  auto gv = [&](const Value& region, const Value& c) {
    Value opts = make_object({{"region", region}});
    if (!c.is_undefined()) {
      opts.set("cell", c);
    }
    const GridVolume v = grid_volume(*eg_surface, *g.surface, opts);
    return make_object(
        {{"cut", v.cut}, {"fill", v.fill}, {"net", v.net}, {"cells", v.cells}, {"cell", v.cell}, {"area", v.area}});
  };
  Value total = gv(Value(regions), cell);
  Array items;
  for (const Value& it : arr(g.items)) {
    if (!get(it, "region").truthy()) {
      continue;
    }
    Value o = make_object({{"id", get(it, "id")}, {"name", get(it, "name")}});
    for (const auto& m : gv(get(it, "region"), total.get("cell")).as_object()) {
      o.set(m.key, m.value);
    }
    items.push_back(std::move(o));
  }
  total.set("items", std::move(items));
  return total;
}

Value Project::network_check() {
  const Value n = get(state, "network");
  if (!n.truthy()) {
    return nullptr;
  }
  const Graded& g = graded();
  const auto s = g.surface ? g.surface : eg().surface;
  const std::string units = base::js_string(get(state, "units"));
  return analyze_network(n, s.get(), units == "m" ? "m" : "ft", *words_);
}

Value Project::to_json() const {
  Array pts;
  for (const Value& p : points.as_array()) {
    const Value blk = get(p, "blk");
    Array row{get(p, "n"), get(p, "x"), get(p, "y"), get(p, "z"), get(p, "d")};
    if (blk.truthy()) {
      row.push_back(blk);
    }
    pts.emplace_back(std::move(row));
  }
  Value grids = Value::object();
  const Value grid = get(get(state, "crs"), "grid");
  if (grid.truthy()) {
    for (const auto& [n, b] : crs_grids) {
      if (n == base::js_string(grid)) {
        grids.set(n, base::base64(b));
      }
    }
  }
  std::set<std::string> used;
  for (const Value& m : arr(get(get(state, "gis"), "maps"))) {
    const Value im = get(m, "image");
    if (im.truthy()) {
      used.insert(base::js_string(im));
    }
  }
  Value images = Value::object();
  for (const auto& [k, v] : gis_images) {
    if (used.contains(k)) {
      images.set(k, v);
    }
  }
  Value gf = Value::object();
  for (const auto& m : gis_features.as_object()) {
    Array rows;
    for (const Value& f : arr(m.value)) {
      rows.emplace_back(Array{get(f, "g"), get(f, "p")});
    }
    gf.append(m.key, std::move(rows));
  }
  const Value features = get(state, "features");
  const Value parcels = get(state, "parcels");
  return make_object({{"format", "civilcraft-project"},
                      {"version", 1},
                      {"name", get(state, "name")},
                      {"units", get(state, "units")},
                      {"pointsFile", points_file},
                      {"points", std::move(pts)},
                      {"surface", get(state, "surface")},
                      {"display", get(state, "display")},
                      {"alignments", get(state, "alignments")},
                      {"pads", get(state, "pads")},
                      {"network", get(state, "network")},
                      {"seq", get(state, "seq")},
                      {"features", features.truthy() ? features : Value::array()},
                      {"parcels", parcels.truthy() ? parcels : Value::array()},
                      {"crs", get(state, "crs")},
                      {"gis", get(state, "gis")},
                      {"gisFeatures", std::move(gf)},
                      {"crsGrids", std::move(grids)},
                      {"gisImages", std::move(images)}});
}

Project::Loaded Project::from_json(const Value& obj, const i18n::Catalog& w) {
  Loaded L;
  Array notes;
  if (!obj.truthy() || !(get(obj, "format").is_string() && get(obj, "format").as_string() == "civilcraft-project")) {
    L.error = w.t("project.err.notcc");
    return L;
  }
  const bool v1 = get(obj, "version").is_number() && get(obj, "version").as_number() == 1;
  if (!v1) {
    L.error = w.t("project.err.newer", {{"version", slice16(base::js_string(get(obj, "version")), 10)}});
    return L;
  }
  auto p = std::make_shared<Project>(w);
  Value& s = p->state;
  {
    const std::string n = pstr(get(obj, "name"), 120);
    s.set("name", n.empty() ? w.t("project.untitled") : n);
  }
  s.set("units", get(obj, "units").is_string() && get(obj, "units").as_string() == "m" ? "m" : "ft");
  const std::string units = s.get("units").as_string();
  {
    const Value seq = get(obj, "seq");
    s.set("seq", fin(seq) && std::floor(seq.as_number()) == seq.as_number() && seq.as_number() > 0 ? seq : Value(1));
  }
  auto id = [&](const Value& v, const char* pre) {
    const std::string sv2 = base::js_string(v);
    return id_like(sv2) ? sv2 : p->next_id(pre);
  };
  const Value pts_in = get(obj, "points");
  Array pts = list(
      pts_in,
      [&](const Value& r) -> Value {
        if (!r.is_array()) {
          return nullptr;
        }
        const Array& c = r.as_array();
        auto at = [&](std::size_t k) { return k < c.size() ? c[k] : Value(); };
        if (!fin(at(1)) || !fin(at(2))) {
          return nullptr;
        }
        Value q = make_object({{"n", pstr(at(0), 40)},
                               {"x", at(1)},
                               {"y", at(2)},
                               {"z", fin(at(3)) ? at(3) : Value(nullptr)},
                               {"d", pstr(at(4))}});
        const Value blk = clean_blk(at(5));
        if (blk.truthy()) {
          q.set("blk", blk);
        }
        return q;
      },
      2e6);
  if (pts_in.is_array() && pts.size() != pts_in.as_array().size()) {
    const auto n = static_cast<double>(pts_in.as_array().size() - pts.size());
    notes.emplace_back(w.tn("project.bad.points", n, {{"n", appkit::num_str(n)}}));
  }
  const Value sf = get(obj, "surface").truthy() ? get(obj, "surface") : Value::object();
  Value* surf = s.find("surface");
  {
    const std::string n = pstr(get(sf, "name"), 120);
    surf->set("name", n.empty() ? w.t("surface.eg") : n);
  }
  {
    Value groups = Value::object();
    const Value g = get(sf, "groups");
    if (g.truthy() && is_obj(g) && g.is_object()) {
      std::size_t k = 0;
      for (const base::Member* m : base::js_members(g.as_object())) {
        if (k++ >= 10000) {
          break;
        }
        const Value& v = m->value;
        if (v.is_string() && (v.as_string() == "off" || v.as_string() == "points" || v.as_string() == "breakline")) {
          groups.set(pstr(Value(m->key), 200), v);
        }
      }
    }
    surf->set("groups", std::move(groups));
  }
  surf->set("breaklines", list(
                              get(sf, "breaklines"),
                              [&](const Value& b) -> Value {
                                Array q = list(get(b, "pts"), xyz, 1e6);
                                if (q.size() < 2) {
                                  return nullptr;
                                }
                                return make_object({{"name", pstr(get(b, "name"), 120)}, {"pts", std::move(q)}});
                              },
                              1e5));
  {
    Array bnd = list(get(sf, "boundary"), xy, 1e6);
    surf->set("boundary", bnd.size() >= 3 ? Value(std::move(bnd)) : Value(nullptr));
  }
  {
    const Value me = get(sf, "maxEdge");
    surf->set("maxEdge", fin(me) && me.as_number() >= 0 ? me : Value(0));
  }
  surf->set("descKeys", clean_desc_keys(get(sf, "descKeys")));
  surf->set("lineCodes", clean_line_codes(get(sf, "lineCodes")));
  {
    const Value tin = get(sf, "tin");
    if (tin.truthy() && get(tin, "pts").is_array() && get(tin, "faces").is_array()) {
      Array tp;
      const Array& tpi = get(tin, "pts").as_array();
      for (std::size_t i = 0; i < tpi.size() && i < 5000000; ++i) {
        tp.push_back(xyz(tpi[i]));
      }
      Array tf;
      const Array& tfi = get(tin, "faces").as_array();
      for (std::size_t i = 0; i < tfi.size() && i < 10000000; ++i) {
        const Value& f = tfi[i];
        if (f.is_array() && f.as_array().size() == 3 && std::ranges::all_of(f.as_array(), [&](const Value& v) {
              return fin(v) && std::floor(v.as_number()) == v.as_number() && v.as_number() >= 0 &&
                     v.as_number() < static_cast<double>(tp.size());
            })) {
          tf.push_back(f);
        }
      }
      Value t = make_object({{"pts", std::move(tp)}, {"faces", std::move(tf)}});
      if (get(tin, "from").is_string() && get(tin, "from").as_string() == "grid") {
        t.set("from", "grid");
      }
      surf->set("tin", std::move(t));
    }
  }
  {
    const Value d = get(obj, "display");
    if (d.truthy() && is_obj(d)) {
      Value* sd = s.find("display");
      for (const auto& m : display_defaults().as_object()) {
        const Value v = get(d, m.key);
        if (v.kind() != m.value.kind()) {
          continue;
        }
        const bool positive = fin(v) && v.as_number() > 0;
        if (v.is_number() && !positive) {
          continue;
        }
        if (m.key == "surface" && !(v.as_string() == "eg" || v.as_string() == "fg")) {
          continue;
        }
        sd->set(m.key, v);
      }
    }
  }
  s.set("alignments",
        list(
            get(obj, "alignments"),
            [&](const Value& a) -> Value {
              Array pis = list(
                  get(a, "pis"),
                  [&](const Value& q) -> Value {
                    const Value c = xy(q);
                    if (!c.truthy()) {
                      return nullptr;
                    }
                    const Value r = get(q, "r");
                    return make_object(
                        {{"x", c.get("x")}, {"y", c.get("y")}, {"r", fin(r) && r.as_number() > 0 ? r : Value(0)}});
                  },
                  1e4);
              if (pis.size() < 2) {
                return nullptr;
              }
              Array pvis = list(
                  get(a, "pvis"),
                  [&](const Value& v) -> Value {
                    if (!(v.truthy() && fin(get(v, "sta")) && fin(get(v, "z")))) {
                      return nullptr;
                    }
                    const Value lv = get(v, "L");
                    return make_object({{"sta", get(v, "sta")},
                                        {"z", get(v, "z")},
                                        {"L", fin(lv) && lv.as_number() > 0 ? lv : Value(0)}});
                  },
                  1e4);
              Value corridor = nullptr;
              const Value c = get(a, "corridor");
              if (c.truthy() && is_obj(c)) {
                const Value d = default_assembly(units);
                auto pos = [&](const char* k) {
                  const Value v = get(c, k);
                  return fin(v) && v.as_number() >= 0 ? v : d.get(k);
                };
                auto or_d = [&](const char* k) {
                  const Value v = pos(k);
                  return v.truthy() ? v : d.get(k);
                };
                corridor = make_object({{"on", get(c, "on").is_bool() && get(c, "on").as_bool()},
                                        {"lane", pos("lane")},
                                        {"laneSlope", pos("laneSlope")},
                                        {"shoulder", pos("shoulder")},
                                        {"shoulderSlope", pos("shoulderSlope")},
                                        {"cut", or_d("cut")},
                                        {"fill", or_d("fill")},
                                        {"step", or_d("step")}});
                const Value sup = clean_super(get(c, "super"), units);
                if (sup.truthy()) {
                  corridor.set("super", sup);
                }
              }
              const Value mr = get(a, "minR");
              const bool min_r = fin(mr) && mr.as_number() > 0 && mr.as_number() < 1e7;
              const std::string aid = id(get(a, "id"), "a");
              const std::string name = pstr(get(a, "name"), 120);
              const Value st = get(a, "start");
              Value out = make_object({{"id", aid},
                                       {"name", name.empty() ? w.t("scene.al.default") : name},
                                       {"start", fin(st) ? st : Value(0)},
                                       {"pis", std::move(pis)},
                                       {"pvis", std::move(pvis)},
                                       {"corridor", corridor},
                                       {"sampling", clean_sampling(get(a, "sampling"), units)}});
              if (min_r) {
                out.set("minR", mr);
              }
              return out;
            },
            1000));
  s.set("pads", list(
                    get(obj, "pads"),
                    [&](const Value& d) -> Value {
                      Array q = list(get(d, "pts"), xy, 1e4);
                      if (q.size() < 3 || !fin(get(d, "z"))) {
                        return nullptr;
                      }
                      const std::string pid = id(get(d, "id"), "p");
                      const std::string name = pstr(get(d, "name"), 120);
                      const Value cut = get(d, "cut");
                      const Value fill = get(d, "fill");
                      return make_object({{"id", pid},
                                          {"name", name.empty() ? w.t("scene.pad.default") : name},
                                          {"pts", std::move(q)},
                                          {"z", get(d, "z")},
                                          {"cut", fin(cut) && cut.as_number() > 0 ? cut : Value(2)},
                                          {"fill", fin(fill) && fill.as_number() > 0 ? fill : Value(2)}});
                    },
                    1000));
  s.set("features", list(
                        get(obj, "features"),
                        [&](const Value& f) -> Value {
                          Value c = clean_feature(f, units, w);
                          if (c.truthy()) {
                            c.set("id", id(get(f, "id"), "f"));
                          }
                          return c;
                        },
                        1000));
  s.set("parcels", list(
                       get(obj, "parcels"),
                       [&](const Value& f) -> Value {
                         Value c = clean_parcel(f);
                         if (c.truthy()) {
                           c.set("id", id(get(f, "id"), "r"));
                         }
                         return c;
                       },
                       2000));
  {
    const Value nw = get(obj, "network");
    if (nw.truthy() && is_obj(nw) && get(nw, "structures").is_array() && get(nw, "pipes").is_array()) {
      const Value r = get(nw, "rules").truthy() ? get(nw, "rules") : Value::object();
      auto pos = [](const Value& v, const Value& d) { return fin(v) && v.as_number() >= 0 ? v : d; };
      Array structures = list(
          get(nw, "structures"),
          [&](const Value& x) -> Value {
            const Value c = xy(x);
            if (!c.truthy()) {
              return nullptr;
            }
            const std::string sid = id(get(x, "id"), "s");
            const std::string name = pstr(get(x, "name"), 60);
            const Value rim = get(x, "rim");
            return make_object({{"id", sid},
                                {"name", name.empty() ? w.t("project.def.structure") : name},
                                {"x", c.get("x")},
                                {"y", c.get("y")},
                                {"rim", fin(rim) ? rim : Value(nullptr)},
                                {"sumpDepth", pos(get(x, "sumpDepth"), Value(0))},
                                {"dia", pos(get(x, "dia"), Value(48))}});
          },
          1e4);
      std::set<std::string> ids;
      for (const Value& x : structures) {
        ids.insert(x.get("id").as_string());
      }
      auto in_ids = [&](const Value& v) { return v.is_string() && ids.contains(v.as_string()); };
      Array pipes = list(
          get(nw, "pipes"),
          [&](const Value& x) -> Value {
            const bool joined = x.truthy() && in_ids(get(x, "from")) && in_ids(get(x, "to")) &&
                                !strict_equal(get(x, "from"), get(x, "to"));
            if (!joined) {
              return nullptr;
            }
            const std::string qid = id(get(x, "id"), "q");
            const std::string name = pstr(get(x, "name"), 60);
            const Value dia = pos(get(x, "dia"), Value(12));
            const Value iu = get(x, "invUp");
            const Value idn = get(x, "invDn");
            return make_object({{"id", qid},
                                {"name", name.empty() ? w.t("project.def.pipe") : name},
                                {"from", get(x, "from")},
                                {"to", get(x, "to")},
                                {"dia", dia.truthy() ? dia : Value(12)},
                                {"invUp", fin(iu) ? iu : Value(nullptr)},
                                {"invDn", fin(idn) ? idn : Value(nullptr)}});
          },
          1e4);
      if (pipes.size() != arr(get(nw, "pipes")).size()) {
        const auto n = static_cast<double>(arr(get(nw, "pipes")).size() - pipes.size());
        notes.emplace_back(w.tn("project.bad.pipes", n, {{"n", appkit::num_str(n)}}));
      }
      const std::string nname = pstr(get(nw, "name"), 60);
      Value net = make_object(
          {{"name", nname.empty() ? w.t("pipes.default") : nname},
           {"type", get(nw, "type").is_string() && get(nw, "type").as_string() == "sanitary" ? "sanitary" : "storm"},
           {"rules", make_object({{"minSlope", pos(get(r, "minSlope"), Value(0))},
                                  {"minCover", pos(get(r, "minCover"), Value(0))},
                                  {"minDrop", pos(get(r, "minDrop"), Value(0))}})},
           {"structures", std::move(structures)},
           {"pipes", std::move(pipes)}});
      Array cs = list(
          get(nw, "catchments"),
          [&](const Value& x) -> Value {
            Value c = clean_catchment(x);
            if (!c.truthy()) {
              return nullptr;
            }
            c.set("id", id(get(x, "id"), "k"));
            const Value to = c.get("to");
            if (to.truthy() && !in_ids(to)) {
              c.set("to", nullptr);
            }
            return c;
          },
          1e4);
      if (!cs.empty()) {
        net.set("catchments", std::move(cs));
      }
      const Value hy = get(nw, "hydro");
      if (hy.truthy() && is_obj(hy)) {
        net.set("hydro", clean_hydro(hy, units));
      }
      s.set("network", std::move(net));
    }
  }
  // crs (readCrs)
  {
    const Value c = get(obj, "crs");
    Value crs = nullptr;
    if (!(c.is_null() || c.is_undefined())) {
      bool ok = is_obj(c) && get(c, "proj4").is_string() && !base::js_blank(get(c, "proj4").as_string()) &&
                base::utf16_length(get(c, "proj4").as_string()) <= 20000 && get(c, "unit").is_string() &&
                (get(c, "unit").as_string() == "m" || get(c, "unit").as_string() == "ft" ||
                 get(c, "unit").as_string() == "ftUS");
      if (ok) {
        const Value code = get(c, "code");
        std::string proj4 = get(c, "proj4").as_string();
        for (char& ch : proj4) {
          const auto u = static_cast<unsigned char>(ch);
          if (u < 0x20 || u == 0x7F) {
            ch = ' ';
          }
        }
        const std::string name = pstr(get(c, "name"), 200);
        crs = make_object({{"code", fin(code) && std::floor(code.as_number()) == code.as_number() &&
                                            code.as_number() > 0 && code.as_number() < 1e6
                                        ? code
                                        : Value(nullptr)},
                           {"name", name.empty() ? w.t("project.def.crs") : name},
                           {"proj4", proj4},
                           {"unit", get(c, "unit")},
                           {"local", nullptr}});
        const Value grid = get(c, "grid");
        if (grid.is_string() && !grid.as_string().empty() && grid.as_string().size() <= 80 &&
            std::ranges::all_of(grid.as_string(), [](char ch) {
              return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' ||
                     ch == '_' || ch == '-';
            })) {
          crs.set("grid", grid);
        }
        const Value l = get(c, "local");
        if (!(l.is_null() || l.is_undefined())) {
          auto pt = [](const Value& q) {
            return q.is_array() && q.as_array().size() == 2 && std::ranges::all_of(q.as_array(), fin);
          };
          if (!is_obj(l) || !fin(get(l, "scale")) || !scale_ok(get(l, "scale")) || !fin(get(l, "rotation")) ||
              !pt(get(l, "from")) || !pt(get(l, "to"))) {
            ok = false;
          } else {
            crs.set("local", make_object({{"scale", get(l, "scale")},
                                          {"rotation", get(l, "rotation")},
                                          {"from", Array{get(l, "from").as_array()[0], get(l, "from").as_array()[1]}},
                                          {"to", Array{get(l, "to").as_array()[0], get(l, "to").as_array()[1]}}}));
          }
        }
      }
      if (!ok) {
        notes.emplace_back(w.t("project.bad.crs"));
        crs = nullptr;
      }
    }
    s.set("crs", crs);
  }
  {
    auto [gis, features] = read_gis_state(get(obj, "gis"), get(obj, "gisFeatures"), notes, w);
    s.set("gis", gis);
    p->gis_features = std::move(features);
    p->gis_version++;
  }
  {  // readCrsGrids
    const Value o = get(obj, "crsGrids");
    const Value grid = get(s.get("crs"), "grid");
    if (o.truthy() && o.is_object() && grid.truthy()) {
      const Value* v = o.find(grid.as_string());
      if (v == nullptr || !v->is_string()) {
        notes.emplace_back(w.t("grid.err.notinfile", {{"name", grid.as_string()}}));
      } else {
        const std::string& b = v->as_string();
        std::size_t body = b.size();
        while (body > 0 && b[body - 1] == '=') {
          --body;
        }
        const bool chars_ok =
            body > 0 && std::all_of(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(body), [](char ch) {
              return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' ||
                     ch == '/';
            });
        if (b.size() > 27962032 || !chars_ok) {
          notes.emplace_back(w.t("grid.err.badfile"));
        } else {
          const auto bytes = atob(b);
          if (!bytes) {
            L.error = "The string to be decoded is not correctly encoded.";
            return L;
          }
          p->crs_grids.emplace_back(grid.as_string(), *bytes);
        }
      }
    }
  }
  {  // readGisImages
    const Value o = get(obj, "gisImages");
    if (o.truthy() && o.is_object()) {
      std::set<std::string> used;
      for (const Value& m : arr(get(s.get("gis"), "maps"))) {
        const Value im = get(m, "image");
        if (im.is_string()) {
          used.insert(im.as_string());
        }
      }
      double bad = 0;
      std::size_t k = 0;
      for (const base::Member* m : base::js_members(o.as_object())) {
        if (k++ >= 200) {
          break;
        }
        const std::string& key = m->key;
        const bool key_ok = key.size() >= 4 && key.size() <= 12 && key.starts_with("img") &&
                            std::all_of(key.begin() + 3, key.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
        if (!used.contains(key) || !key_ok) {
          continue;
        }
        const Value& v = m->value;
        bool good = false;
        if (v.is_string() && v.as_string().size() <= 34952574) {
          const std::string& sv2 = v.as_string();
          for (const std::string_view pre : {"data:image/png;base64,", "data:image/jpeg;base64,"}) {
            if (sv2.starts_with(pre)) {
              std::size_t e = sv2.size();
              while (e > pre.size() && sv2[e - 1] == '=') {
                --e;
              }
              good = e > pre.size() && std::all_of(sv2.begin() + static_cast<std::ptrdiff_t>(pre.size()),
                                                   sv2.begin() + static_cast<std::ptrdiff_t>(e), [](char ch) {
                                                     return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                                            (ch >= '0' && ch <= '9') || ch == '+' || ch == '/';
                                                   });
            }
          }
        }
        if (good) {
          p->gis_images.emplace_back(key, v.as_string());
        } else {
          bad++;
        }
      }
      if (bad != 0) {
        notes.emplace_back(w.tn("project.bad.maps", bad, {{"n", appkit::num_str(bad)}}));
      }
    }
  }
  p->points = Value(std::move(pts));
  p->points_file = pstr(get(obj, "pointsFile"), 200);
  p->points_version++;
  {
    std::vector<Value> all;
    for (const char* k : {"alignments", "pads"}) {
      for (const Value& v : arr(s.get(k))) {
        all.push_back(v);
      }
    }
    const Value net = s.get("network");
    if (net.truthy()) {
      for (const char* k : {"structures", "pipes", "catchments"}) {
        for (const Value& v : arr(get(net, k))) {
          all.push_back(v);
        }
      }
    }
    for (const Value& v : arr(get(s.get("gis"), "layers"))) {
      all.push_back(v);
    }
    for (const char* k : {"features", "parcels"}) {
      for (const Value& v : arr(s.get(k))) {
        all.push_back(v);
      }
    }
    for (const Value& a : all) {
      std::string idv = base::js_string(get(a, "id"));
      std::size_t i = 0;
      while (i < idv.size() && (idv[i] < '0' || idv[i] > '9')) {
        ++i;
      }
      const double n = base::js_number(std::string_view(idv).substr(i));
      if (n >= num(s.get("seq"))) {
        s.set("seq", n + 1);
      }
    }
  }
  L.project = std::move(p);
  L.notes = std::move(notes);
  return L;
}

void apply_ops(Value& state, const Value& ops) {
  for (const Value& op : arr(ops)) {
    Value path = get(op, "set");
    int kind = 0;
    if (!path.truthy()) {
      path = get(op, "push");
      kind = 1;
    }
    if (!path.truthy()) {
      path = get(op, "del");
      kind = 2;
    }
    if (!path.truthy() || !path.is_array() || path.as_array().empty()) {
      continue;
    }
    const Array& P = path.as_array();
    Value* cur = &state;
    bool ok = true;
    for (std::size_t i = 0; i + 1 < P.size(); ++i) {
      const Value& k = P[i];
      if (cur->is_array()) {
        const double d = base::js_to_number(k);
        if ((k.is_number() || (k.is_string() && base::js_index_key(k.as_string()))) && d >= 0 && std::floor(d) == d &&
            d < static_cast<double>(cur->as_array().size())) {
          cur = &cur->as_array()[static_cast<std::size_t>(d)];
          continue;
        }
        ok = false;
        break;
      }
      if (cur->is_object()) {
        Value* next = cur->find(base::js_string(k));
        if (next == nullptr) {
          ok = false;
          break;
        }
        cur = next;
        continue;
      }
      ok = false;
      break;
    }
    if (!ok || !(cur->is_object() || cur->is_array())) {
      continue;
    }
    const Value& last = P.back();
    const Value v = get(op, "value");
    const Value val = (v.is_null() || v.is_undefined()) ? Value(nullptr) : v;
    if (kind == 0) {
      if (last.is_string() && !cur->is_array()) {
        cur->set(last.as_string(), val);
      } else if (last.is_number() && cur->is_array()) {
        const double d = last.as_number();
        if (d < static_cast<double>(cur->as_array().size()) && d >= 0 && std::floor(d) == d) {
          cur->as_array()[static_cast<std::size_t>(d)] = val;
        }
      }
    } else if (kind == 1) {
      if (last.is_string() && !cur->is_array()) {
        Value* a = cur->find(last.as_string());
        if (a != nullptr && a->is_array()) {
          a->push(val);
        }
      }
    } else {
      if (last.is_string() && !cur->is_array()) {
        cur->erase(last.as_string());
      } else if (last.is_number() && cur->is_array()) {
        const double d = last.as_number();
        const auto n = static_cast<double>(cur->as_array().size());
        if (d < n) {
          double st = std::trunc(d);
          if (st < 0) {
            st = std::max(n + st, 0.0);
          }
          if (st < n) {
            cur->as_array().erase(cur->as_array().begin() + static_cast<std::ptrdiff_t>(st));
          }
        }
      }
    }
  }
}

}  // namespace cc::civil
