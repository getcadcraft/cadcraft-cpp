// SPDX-License-Identifier: Apache-2.0
// model.js, files: a definition's native form (defToJSON / defOf / defFromJSON), the checks on untrusted files, and
// the scene, storey and light cleaners.
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/value.hpp"
#include "cc/m3d/model.hpp"
#include "files_internal.hpp"

namespace cc::m3d {

using base::Array;
using base::Value;

namespace detail {

bool is_int(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number()) && std::floor(v.as_number()) == v.as_number();
}
bool is_num(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number()) && std::abs(v.as_number()) < 1e7;
}
bool is_pt(const Value& p) {
  return p.is_array() && p.as_array().size() == 3 && std::ranges::all_of(p.as_array(), is_num);
}
Vec3 pt_of(const Value& p) {
  const Array& a = p.as_array();
  return {a[0].as_number(), a[1].as_number(), a[2].as_number()};
}
Value pt_value(const Vec3& p) {
  return Array{p[0], p[1], p[2]};
}
Value ids_value(std::span<const Id> ids) {
  Array a;
  a.reserve(ids.size());
  for (const Id id : ids) {
    a.emplace_back(id);
  }
  return a;
}
Value mat_value(const Mat4& m) {
  Array a;
  a.reserve(16);
  for (const double x : m) {
    a.emplace_back(x);
  }
  return a;
}
bool truthy(const Value* v) {
  return v != nullptr && v->truthy();
}

}  // namespace detail

using namespace detail;  // NOLINT(google-build-using-namespace): this file's own helpers

namespace {

bool int_id(const Value& x) {
  return is_int(x) && x.as_number() > 0 && x.as_number() < 1e9;
}

bool matches_starter(const std::string& s) {
  return !s.empty() && s.size() <= 32 &&
         std::ranges::all_of(s, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool matches_def_ref(const std::string& s) {  // /^d\d{1,9}$/
  return s.size() >= 2 && s.size() <= 10 && s[0] == 'd' &&
         std::all_of(s.begin() + 1, s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

Id id_of(const Value& v) {
  return to_id(v.as_number());
}

/// Map.has(x) on an id table: x a number naming a key.
template <class T>
bool has_id(const IdMap<T>& m, const Value& x) {
  return is_int(x) && std::abs(x.as_number()) < 9e15 && m.has(id_of(x));
}

}  // namespace

std::string clean_text(std::string_view s, std::size_t max) {
  std::string t;
  t.reserve(s.size());
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    t += (u < 0x20 || u == 0x7f) ? ' ' : c;
  }
  const std::string_view tr0 = base::js_trim(t);
  // .slice(0, max) counts UTF-16 code units; a character the cut would split is left out whole
  std::string out;
  std::size_t units = 0;
  std::size_t i = 0;
  while (i < tr0.size()) {
    const auto b = static_cast<unsigned char>(tr0[i]);
    std::size_t n = 4;
    if (b < 0x80) {
      n = 1;
    } else if (b < 0xE0) {
      n = 2;
    } else if (b < 0xF0) {
      n = 3;
    }
    const std::size_t u = n == 4 ? 2 : 1;
    if (units + u > max) {
      break;
    }
    out.append(tr0.substr(i, n));
    units += u;
    i += n;
  }
  return out;
}

std::string clean_text(const Value& s, std::size_t max) {
  return s.is_string() ? clean_text(std::string_view(s.as_string()), max) : std::string();
}

Value def_to_json(const Def& d) {
  Value o = base::make_object({{"id", d.id}, {"kind", d.kind}, {"name", d.name}, {"next", Value(d.next)}});
  if (!d.starter.empty()) {
    o.set("starter", d.starter);
  }
  if (d.dyn) {
    o.set("dyn", base::make_object({{"src", d.dyn->src}, {"size", pt_value(d.dyn->size)}}));
  }
  Array verts;
  verts.reserve(d.verts.size());
  for (const auto& [id, p] : d.verts) {
    verts.emplace_back(Array{Value(id), p[0], p[1], p[2]});
  }
  Array edges;
  edges.reserve(d.edges.size());
  for (const auto& [id, e] : d.edges) {
    Array row = {Value(id), Value(e.a), Value(e.b)};
    if (e.hidden) {
      row.emplace_back(e.soft ? 1 : 0);
      row.emplace_back(e.curve.empty() ? Value(0) : Value(e.curve));
      row.emplace_back(1);
    } else if (!e.curve.empty()) {
      row.emplace_back(e.soft ? 1 : 0);
      row.emplace_back(e.curve);
    } else if (e.soft) {
      row.emplace_back(1);
    }
    edges.emplace_back(std::move(row));
  }
  Array faces;
  faces.reserve(d.faces.size());
  for (const auto& [id, f] : d.faces) {
    Array loops;
    for (const Loop& l : f.loops) {
      loops.push_back(ids_value(l));
    }
    Value fo = base::make_object({{"id", Value(id)}, {"loops", std::move(loops)}});
    if (!f.color.empty()) {
      fo.set("color", f.color);
    }
    if (f.hidden) {
      fo.set("hidden", true);
    }
    faces.push_back(std::move(fo));
  }
  Array insts;
  for (const auto& [id, i] : d.insts) {
    Value io = base::make_object({{"id", Value(id)}, {"def", i.def}, {"m", mat_value(i.m)}});
    if (!i.name.empty()) {
      io.set("name", i.name);
    }
    if (!i.color.empty()) {
      io.set("color", i.color);
    }
    if (i.hidden) {
      io.set("hidden", true);
    }
    if (!i.tag.empty()) {
      io.set("tag", i.tag);
    }
    if (i.rule) {
      Value r = Value::object();
      const InstRule& ru = *i.rule;
      if (ru.x) {
        r.set("x", *ru.x);
      }
      if (ru.y) {
        r.set("y", *ru.y);
      }
      if (ru.z) {
        r.set("z", *ru.z);
      }
      if (ru.sx) {
        r.set("sx", *ru.sx);
      }
      if (ru.sy) {
        r.set("sy", *ru.sy);
      }
      if (ru.sz) {
        r.set("sz", *ru.sz);
      }
      io.set("rule", std::move(r));
    }
    insts.push_back(std::move(io));
  }
  o.set("verts", std::move(verts));
  o.set("edges", std::move(edges));
  o.set("faces", std::move(faces));
  o.set("insts", std::move(insts));
  if (!d.splines.empty()) {
    Array sp;
    for (const auto& [id, s] : d.splines) {
      Array pts;
      for (const Vec3& p : s.pts) {
        pts.push_back(pt_value(p));
      }
      sp.push_back(base::make_object(
          {{"id", id}, {"pts", std::move(pts)}, {"closed", s.closed}, {"segs", Value(static_cast<double>(s.segs))}}));
    }
    o.set("splines", std::move(sp));
  }
  return o;
}

base::Result<void> def_from_json(Def& d, const Value& j) {
  const auto fail = [](std::string_view key) { return base::fail(base::Errc::invalid, tr(key)); };
  if (const Value* s = j.find("starter"); s != nullptr && s->is_string() && matches_starter(s->as_string())) {
    d.starter = s->as_string();
  }
  if (const Value* dy = j.find("dyn"); truthy(dy) && dy->is_object()) {
    const Value src = dy->get("src");
    const Value size = dy->get("size");
    if (src.is_string() && matches_def_ref(src.as_string()) && size.is_array() && size.as_array().size() == 3 &&
        std::ranges::all_of(size.as_array(), [](const Value& x) {
          return x.is_number() && std::isfinite(x.as_number()) && x.as_number() >= 0;
        })) {
      d.dyn = Dyn{src.as_string(), pt_of(size)};
    }
  }
  double max = 0;
  const Value verts = j.get("verts");
  if (verts.is_array()) {
    for (const Value& v : verts.as_array()) {
      if (!v.is_array() || v.as_array().size() != 4 || !int_id(v.as_array()[0])) {
        return fail("m3d.model.a_vertex_is_wrong");
      }
      const Array& a = v.as_array();
      if (!is_num(a[1]) || !is_num(a[2]) || !is_num(a[3])) {
        return fail("m3d.model.a_vertex_is_wrong");
      }
      d.verts.set(id_of(a[0]), {a[1].as_number(), a[2].as_number(), a[3].as_number()});
      max = std::max(max, a[0].as_number());
    }
  }
  const Value edges = j.get("edges");
  if (edges.is_array()) {
    for (const Value& e : edges.as_array()) {
      if (!e.is_array() || e.as_array().empty() || !int_id(e.as_array()[0])) {
        return fail("m3d.model.an_edge_is_wrong");
      }
      const Array& a = e.as_array();
      const Value e1 = a.size() > 1 ? a[1] : Value();
      const Value e2 = a.size() > 2 ? a[2] : Value();
      if (!has_id(d.verts, e1) || !has_id(d.verts, e2) || e1.as_number() == e2.as_number()) {
        return fail("m3d.model.an_edge_is_wrong");
      }
      const Id va = id_of(e1);
      const Id vb = id_of(e2);
      if (d.edge_between(va, vb)) {
        continue;
      }
      Edge ed{.a = va, .b = vb, .soft = false, .curve = {}, .hidden = false};
      if (a.size() > 3 && a[3].truthy()) {
        ed.soft = true;
      }
      if (a.size() > 4 && a[4].is_string() && is_curve_id(a[4].as_string())) {
        ed.curve = a[4].as_string();
      }
      if (a.size() > 5 && a[5].is_number() && a[5].as_number() == 1) {
        ed.hidden = true;
      }
      d.edges.set(id_of(a[0]), ed);
      d.ek().insert_or_assign(edge_key(va, vb), id_of(a[0]));
      max = std::max(max, a[0].as_number());
    }
  }
  const Value faces = j.get("faces");
  if (faces.is_array()) {
    for (const Value& f : faces.as_array()) {
      const Value fl = f.get("loops");
      if (!f.is_object() || !int_id(f.get("id")) || !fl.is_array() || fl.as_array().empty()) {
        return fail("m3d.model.a_face_is_wrong");
      }
      std::vector<Loop> loops;
      for (const Value& l : fl.as_array()) {
        Loop loop;
        if (l.is_array()) {
          for (const Value& v : l.as_array()) {
            if (!has_id(d.verts, v)) {
              return fail("m3d.model.a_face_is_wrong");
            }
            loop.push_back(id_of(v));
          }
        }
        if (loop.size() < 3) {
          return fail("m3d.model.a_face_is_wrong");
        }
        loops.push_back(std::move(loop));
      }
      max = std::max(max, f.get("id").as_number());
      d.next = std::max(d.next, to_id(max) + 1);
      const Value color = f.get("color");
      const std::string c =
          color.is_string() && is_paint(color.as_string()) ? base::js_to_lower(color.as_string()) : std::string();
      const Value hidden = f.get("hidden");
      (void)f_add(d, std::move(loops), c, id_of(f.get("id")), hidden.is_bool() && hidden.as_bool());
    }
  }
  const Value insts = j.get("insts");
  if (insts.is_array()) {
    for (const Value& i : insts.as_array()) {
      const Value m = i.get("m");
      if (!i.is_object() || !int_id(i.get("id")) || !i.get("def").is_string() || !m.is_array() ||
          m.as_array().size() != 16 || !std::ranges::all_of(m.as_array(), is_num)) {
        return fail("m3d.model.an_instance_is_wrong");
      }
      Inst ins;
      ins.def = i.get("def").as_string();
      for (std::size_t k = 0; k < 16; ++k) {
        ins.m.at(k) = m.as_array()[k].as_number();
      }
      if (i.get("name").truthy()) {
        ins.name = clean_text(i.get("name"), 120);
      }
      if (const Value c = i.get("color"); c.is_string() && is_paint(c.as_string())) {
        ins.color = base::js_to_lower(c.as_string());
      }
      if (const Value h = i.get("hidden"); h.is_bool() && h.as_bool()) {
        ins.hidden = true;
      }
      if (const Value t = i.get("tag"); t.is_string() && !clean_text(t, 80).empty()) {
        ins.tag = clean_text(t, 80);
      }
      if (const Value r = i.get("rule"); r.truthy() && r.is_object()) {
        InstRule ru;
        bool any = false;
        const auto rule_word = [&](std::string_view k, std::optional<std::string>& out) {
          const Value v = r.get(k);
          if (v.is_string()) {
            for (const char* w : {"auto", "stretch", "start", "end", "move", "scale", "repeat"}) {
              if (v.as_string() == w) {
                out = v.as_string();
                any = true;
              }
            }
          }
        };
        const auto rule_num = [&](std::string_view k, std::optional<double>& out) {
          const Value v = r.get(k);
          if (v.is_number() && std::isfinite(v.as_number()) && v.as_number() > 0 && v.as_number() < 1e6) {
            out = v.as_number();
            any = true;
          }
        };
        rule_word("x", ru.x);
        rule_word("y", ru.y);
        rule_word("z", ru.z);
        rule_num("sx", ru.sx);
        rule_num("sy", ru.sy);
        rule_num("sz", ru.sz);
        if (any) {
          ins.rule = ru;
        }
      }
      d.insts.set(id_of(i.get("id")), std::move(ins));
      max = std::max(max, i.get("id").as_number());
    }
  }
  const Value splines = j.get("splines");
  if (splines.is_array()) {
    for (const Value& sp : splines.as_array()) {
      const Value id = sp.get("id");
      const Value pts = sp.get("pts");
      if (!sp.is_object() || !id.is_string() || !is_curve_id(id.as_string()) || !pts.is_array() ||
          pts.as_array().size() < 2 || pts.as_array().size() > 10000 || !std::ranges::all_of(pts.as_array(), is_pt)) {
        continue;
      }
      Spline s;
      for (const Value& p : pts.as_array()) {
        s.pts.push_back(pt_of(p));
      }
      s.closed = sp.get("closed").truthy();
      const Value segs = sp.get("segs");
      s.segs = is_int(segs) && segs.as_number() >= 1 && segs.as_number() <= 64 ? static_cast<int>(segs.as_number())
                                                                               : kSplineSegs;
      d.splines.set(id.as_string(), std::move(s));
    }
  }
  const Value next = j.get("next");
  const double jn = is_int(next) ? next.as_number() : 1;
  d.next = to_id(std::max({jn, max + 1, static_cast<double>(d.next)}));
  return {};
}

base::Result<std::unique_ptr<Def>> def_of(const Value& dj) {
  const std::string id = dj.get("id").as_string();
  std::string kind = "group";
  if (id == "root") {
    kind = "root";
  } else if (dj.get("kind").is_string() && dj.get("kind").as_string() == "component") {
    kind = "component";
  }
  std::string name = clean_text(dj.get("name"), 120);
  if (name.empty() && kind == "root") {
    name = "Model";
  } else if (name.empty()) {
    name = kind == "component" ? "Component" : "Group";
  }
  auto d = std::make_unique<Def>(id, kind, name);
  if (auto r = def_from_json(*d, dj); !r) {
    return std::unexpected(r.error());
  }
  return d;
}

void copy_own_geometry(const Def& src, Def& dst) {
  Value j = def_to_json(src);
  j.set("insts", Array{});
  if (!def_from_json(dst, j)) {
    return;  // its own geometry always reads back
  }
}

// ---------------------------------------------------------------- cleaners

std::vector<Storey> clean_storeys(const Value& list) {
  std::vector<Storey> out;
  if (list.is_array()) {
    std::size_t k = 0;
    for (const Value& s : list.as_array()) {
      if (k++ >= 200) {
        break;
      }
      const std::string name = clean_text(s.get("name"), 80);
      const Value ev = s.get("elevation");
      double e = std::nan("");
      if (ev.is_number()) {
        e = ev.as_number();
      } else if (ev.is_string()) {
        e = base::js_number(ev.as_string());
      } else if (ev.is_null()) {
        e = 0;
      } else if (ev.is_bool()) {
        e = ev.as_bool() ? 1 : 0;
      }
      if (name.empty() || !std::isfinite(e) || std::abs(e) > 1e6 ||
          std::ranges::any_of(out, [&](const Storey& o) { return std::abs(o.elevation - e) < 1e-6; })) {
        continue;
      }
      out.push_back({name, e});
    }
  }
  std::ranges::stable_sort(out, [](const Storey& a, const Storey& b) { return a.elevation < b.elevation; });
  return out;
}

Value clean_scene(const Value& s) {
  if (!s.is_object() || s.is_null()) {
    return {nullptr};
  }
  const Value c = s.get("cam").is_object() ? s.get("cam") : Value::object();
  if (!is_pt(c.get("eye")) || !is_pt(c.get("target")) || dist(pt_of(c.get("eye")), pt_of(c.get("target"))) < 1e-9) {
    return {nullptr};
  }
  const Value st = s.get("style").truthy() && s.get("style").is_object() ? s.get("style") : Value::object();
  const Value sun = st.get("sun").truthy() && st.get("sun").is_object() ? st.get("sun") : Value::object();
  const auto num = [](const Value& x, double lo, double hi, double d) {
    return x.is_number() && std::isfinite(x.as_number()) ? std::min(hi, std::max(lo, x.as_number())) : d;
  };
  Value out = Value::object();
  const Value vis = s.get("vis");
  if (vis.truthy() && vis.is_object()) {
    Array paths;
    const Value insts = vis.get("insts");
    if (insts.is_array()) {
      std::size_t k = 0;
      for (const Value& p : insts.as_array()) {
        if (k++ >= 20000) {
          break;
        }
        if (p.is_array() && !p.as_array().empty() && p.as_array().size() <= 32 &&
            std::ranges::all_of(p.as_array(), is_int)) {
          paths.push_back(p);
        }
      }
    }
    Array tags;
    const Value tv = vis.get("tags");
    if (tv.is_array()) {
      std::size_t k = 0;
      for (const Value& x : tv.as_array()) {
        if (k++ >= 1000) {
          break;
        }
        std::string t = clean_text(x, 80);
        if (!t.empty()) {
          tags.emplace_back(std::move(t));
        }
      }
    }
    const Value sec = vis.get("section");
    const Value cuts = vis.get("cuts");
    out.set("vis", base::make_object({{"insts", std::move(paths)},
                                      {"tags", std::move(tags)},
                                      {"section", is_int(sec) ? sec : Value(nullptr)},
                                      {"cuts", !cuts.is_bool() || cuts.as_bool()}}));
  }
  std::string name = clean_text(s.get("name"), 80);
  if (name.empty()) {
    name = "Scene";
  }
  out.set("name", name);
  const Value persp = c.get("persp");
  out.set("cam", base::make_object({{"eye", c.get("eye")},
                                    {"target", c.get("target")},
                                    {"fov", num(c.get("fov"), 10, 120, 35)},
                                    {"persp", !persp.is_bool() || persp.as_bool()},
                                    {"height", num(c.get("height"), 1e-6, 1e7, 10)}}));
  const Value mode = st.get("mode");
  std::string m = "shaded";
  if (mode.is_string()) {
    for (const char* k : {"shaded", "hidden", "wire", "xray"}) {
      if (mode.as_string() == k) {
        m = k;
      }
    }
  }
  const Value edges = st.get("edges");
  const Value shadows = st.get("shadows");
  out.set("style",
          base::make_object({{"mode", m},
                             {"edges", !edges.is_bool() || edges.as_bool()},
                             {"shadows", shadows.is_bool() && shadows.as_bool()},
                             {"sun", base::make_object({{"month", base::js_round(num(sun.get("month"), 1, 12, 6))},
                                                        {"day", base::js_round(num(sun.get("day"), 1, 31, 21))},
                                                        {"hour", num(sun.get("hour"), 0, 23.99, 15)},
                                                        {"lat", num(sun.get("lat"), -90, 90, 40)},
                                                        {"lon", num(sun.get("lon"), -180, 180, -105)},
                                                        {"tz", num(sun.get("tz"), -14, 14, -7)}})}}));
  return out;
}

std::optional<Light> clean_light(const Value& o) {
  if (!o.is_object()) {
    return std::nullopt;
  }
  const Value kind = o.get("kind");
  if (!kind.is_string() || (kind.as_string() != "point" && kind.as_string() != "spot" && kind.as_string() != "area") ||
      !is_pt(o.get("p"))) {
    return std::nullopt;
  }
  const auto clamp = [](const Value& x, double lo, double hi, double d) {
    return is_num(x) ? std::min(hi, std::max(lo, x.as_number())) : d;
  };
  Light L;
  L.kind = kind.as_string();
  L.p = pt_of(o.get("p"));
  const Value dir = o.get("dir");
  L.dir = is_pt(dir) && len(pt_of(dir)) > 1e-9 ? norm(pt_of(dir)) : Vec3{0, 0, -1};
  const Value size = o.get("size");
  if (size.is_array() && size.as_array().size() == 2) {
    L.size = {clamp(size.as_array()[0], 0.02, 20, 0.6), clamp(size.as_array()[1], 0.02, 20, 0.6)};
  } else {
    L.size = L.kind == "area" ? std::array<double, 2>{0.6, 0.6} : std::array<double, 2>{0.06, 0.06};
  }
  const Value color = o.get("color");
  L.color = color.is_string() && is_color(color.as_string()) ? base::js_to_lower(color.as_string()) : "#ffe7c4";
  L.lumens = base::js_round(clamp(o.get("lumens"), 1, 200000, 800));
  L.angle = base::js_round(clamp(o.get("angle"), 5, 170, 60));
  const Value on = o.get("on");
  L.on = !on.is_bool() || on.as_bool();
  L.name = clean_text(o.get("name"), 80);
  return L;
}

Value light_json(Id id, const Light& L) {
  return base::make_object({{"id", Value(id)},
                            {"kind", L.kind},
                            {"p", pt_value(L.p)},
                            {"dir", pt_value(L.dir)},
                            {"color", L.color},
                            {"lumens", L.lumens},
                            {"angle", L.angle},
                            {"size", Array{L.size[0], L.size[1]}},
                            {"on", L.on},
                            {"name", L.name}});
}

}  // namespace cc::m3d
