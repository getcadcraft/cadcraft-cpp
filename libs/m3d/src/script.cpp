// SPDX-License-Identifier: Apache-2.0
// The model scripts of tools/parity/m3d.mjs, run through cc::m3d: each op makes the kernel call the JS harness makes
// (its OPS table), with the same selectors, and reports what it returned (in the JSON shape the JS value has) or the
// words of the error, and the model in its file form after it.
#include "cc/m3d/script.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <map>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "cc/m3d/model.hpp"
#include "files_internal.hpp"

namespace cc::m3d {

using base::Array;
using base::Value;
using namespace detail;  // NOLINT(google-build-using-namespace): the kernel's own helpers

namespace {

constexpr Id kNone = -1;  // a selector that finds nothing (null in the JS): no table has this id

/// A point of the script. One outside the native file's range (|x| ≥ 1e7 m, isNum) is taken as 0: the parity scripts
/// never give one, and the kernel's searches slow to minutes on them (fuzz, 2026-10-22).
Vec3 vec(const Value& v, const Vec3& d = {0, 0, 0}) {
  if (!v.is_array() || v.as_array().size() < 3) {
    return d;
  }
  if (!std::ranges::all_of(v.as_array() | std::views::take(3),
                           [](const Value& x) { return is_num(x) || !x.is_number(); })) {
    return d;
  }
  const Array& a = v.as_array();
  return {a[0].num_or(0), a[1].num_or(0), a[2].num_or(0)};
}
std::optional<Vec3> opt_vec(const Value& v) {
  if (!v.is_array()) {
    return std::nullopt;
  }
  return vec(v);
}
std::vector<Vec3> vecs(const Value& v) {
  std::vector<Vec3> out;
  if (v.is_array()) {
    for (const Value& p : v.as_array()) {
      out.push_back(vec(p));
    }
  }
  return out;
}
std::vector<Id> idlist(const Value& v) {
  std::vector<Id> out;
  if (v.is_array()) {
    for (const Value& x : v.as_array()) {
      out.push_back(to_id(x.num_or(-1)));
    }
  }
  return out;
}
Mat4 mat(const Value& v) {
  Mat4 m = ident();
  if (v.is_array() && v.as_array().size() == 16) {
    for (std::size_t i = 0; i < 16; ++i) {
      m.at(i) = v.as_array()[i].num_or(0);
    }
  }
  return m;
}
double rad(double deg) {
  return (deg * kPi) / 180;
}

// ---- selectors
std::vector<Id> facing(const Def& d, const Vec3& n) {
  std::vector<Id> out;
  const Vec3 u = norm(n);
  for (const auto& [id, f] : d.faces) {
    if (dot(face_normal(d, f), u) > 1 - 1e-9) {
      out.push_back(id);
    }
  }
  return out;
}
Id face(const Def& d, const Value& s) {
  if (s.is_number()) {
    return to_id(s.as_number());
  }
  if (!s.is_object()) {
    return kNone;
  }
  if (s.get("last").truthy()) {
    const auto k = d.faces.keys();
    return k.empty() ? kNone : k.back();
  }
  std::vector<Id> ids = s.has("facing") ? facing(d, vec(s.get("facing"))) : d.faces.keys();
  if (s.has("area")) {
    const double A = s.get("area").num_or(0);
    std::erase_if(ids, [&](Id id) { return !(std::abs(face_area(d, *d.faces.get(id)) - A) <= 1e-6); });
  }
  if (s.has("loops")) {
    const double L = s.get("loops").num_or(0);
    std::erase_if(ids, [&](Id id) { return static_cast<double>(d.faces.get(id)->loops.size()) != L; });
  }
  const auto nth = static_cast<std::size_t>(to_count(s.get("nth").num_or(0), 0, 1 << 30));
  return nth < ids.size() ? ids[nth] : kNone;
}
Id edge(const Def& d, const Value& s) {
  if (s.is_number()) {
    return to_id(s.as_number());
  }
  const Value b = s.get("between");
  if (b.is_array() && b.as_array().size() == 2) {
    const Vec3 p = vec(b.as_array()[0]);
    const Vec3 q = vec(b.as_array()[1]);
    for (const auto& [id, e] : d.edges) {
      const Vec3& A = d.vert(e.a);
      const Vec3& B = d.vert(e.b);
      if ((dist(A, p) <= 1e-6 && dist(B, q) <= 1e-6) || (dist(A, q) <= 1e-6 && dist(B, p) <= 1e-6)) {
        return id;
      }
    }
  }
  return kNone;
}
std::vector<Id> edges(const Def& d, const Value& l) {
  if (l.is_array()) {
    std::vector<Id> out;
    for (const Value& x : l.as_array()) {
      out.push_back(edge(d, x));
    }
    return out;
  }
  return l.get("all").truthy() ? d.edges.keys() : std::vector<Id>{};
}
std::vector<Id> faces(const Def& d, const Value& l) {
  if (l.is_array()) {
    std::vector<Id> out;
    for (const Value& x : l.as_array()) {
      out.push_back(face(d, x));
    }
    return out;
  }
  return l.get("all").truthy() ? d.faces.keys() : std::vector<Id>{};
}
Sel sel(const Def& d, const Value& l) {
  Sel out;
  if (l.is_array()) {
    for (const Value& x : l.as_array()) {
      const std::string t = x.get("t").is_string() ? x.get("t").as_string() : std::string("?");
      const char c = t.empty() ? '?' : t[0];
      Id id = to_id(x.get("id").num_or(-1));
      if (c == 'f') {
        id = face(d, x.get("id"));
      } else if (c == 'e') {
        id = edge(d, x.get("id"));
      }
      out.push_back({c, id});
    }
    return out;
  }
  const Value all = l.get("all");
  if (!all.is_string()) {
    return out;
  }
  const std::string& a = all.as_string();
  if (a == "faces" || a == "everything") {
    for (const Id id : d.faces.keys()) {
      out.push_back({'f', id});
    }
  }
  if (a == "edges" || a == "everything") {
    for (const Id id : d.edges.keys()) {
      out.push_back({'e', id});
    }
  }
  if (a == "insts" || a == "everything") {
    for (const Id id : d.insts.keys()) {
      out.push_back({'i', id});
    }
  }
  return out;
}

// ---- results in the shape of the JS values
Value ids_v(std::span<const Id> ids) {
  return ids_value(ids);
}
Value opt_id(std::optional<Id> id) {
  return id ? Value(*id) : Value(nullptr);
}
Value sel_v(const Sel& s) {
  Array a;
  for (const Item& x : s) {
    a.push_back(base::make_object({{"t", std::string(1, x.t)}, {"id", x.id < 0 ? Value(nullptr) : Value(x.id)}}));
  }
  return a;
}
Value info_v(const SolidInfo& s) {
  return base::make_object({{"closed", s.closed},
                            {"volume", s.volume ? Value(*s.volume) : Value(nullptr)},
                            {"area", s.area},
                            {"faces", Value(s.faces)},
                            {"edges", Value(s.edges)},
                            {"verts", Value(s.verts)}});
}
Value curve_v(const std::optional<CurveInfo>& c) {
  if (!c) {
    return {nullptr};
  }
  return base::make_object({{"id", c->id},
                            {"kind", c->kind},
                            {"edges", ids_v(c->edges)},
                            {"verts", ids_v(c->verts)},
                            {"closed", c->closed},
                            {"length", c->length},
                            {"segs", Value(c->segs)},
                            {"round", c->round},
                            {"center", c->center ? pt_value(*c->center) : Value(nullptr)},
                            {"normal", c->normal ? pt_value(*c->normal) : Value(nullptr)},
                            {"radius", c->radius},
                            {"flat", c->flat},
                            {"sweep", c->sweep}});
}
Value pp_v(const PushPullResult& r) {
  if (!r.ok) {
    return base::make_object({{"ok", false}});
  }
  return base::make_object({{"ok", true}, {"mode", r.mode}, {"distance", r.distance}, {"face", opt_id(r.face)}});
}

using Out = base::Result<Value>;
Out ok(Value v) {
  return v;
}
Out none() {
  return Value(nullptr);
}
template <class T>
Out err_of(const base::Result<T>& r) {
  return std::unexpected(r.error());
}

/// Every number of a step inside the native file's range (|x| < 1e7, isNum; a lamp's numbers are clamped by cleanLight
/// and may be anything) and nested at most 64 deep: the parity scripts never go past it, and the
/// kernel's searches slow to hours on such numbers (fuzz, 2026-10-27).
// NOLINTNEXTLINE(misc-no-recursion): 64 levels at most
bool sane(const Value& v, int depth) {
  if (depth > 64) {
    return false;
  }
  if (v.is_number()) {
    return std::abs(v.as_number()) < 1e7;  // NaN fails too
  }
  if (v.is_array()) {
    for (const Value& x : v.as_array()) {
      if (!sane(x, depth + 1)) {
        return false;
      }
    }
  } else if (v.is_object() && !v.is_point()) {
    for (const auto& m : v.as_object()) {
      if (m.key != "light" && m.key != "patch" && !sane(m.value, depth + 1)) {
        return false;
      }
    }
  }
  return true;
}

struct Runner {
  Model m;
  std::optional<Value> clip;

  Out op(const Value& s, Def& d) {
    const std::string name = s.get("op").is_string() ? s.get("op").as_string() : std::string();
    const auto path = idlist(s.get("path"));
    if (name == "rect") {
      const auto pts = rect_pts(vec(s.get("p")), vec(s.get("u"), {1, 0, 0}), vec(s.get("v"), {0, 1, 0}),
                                s.get("w").num_or(0), s.get("h").num_or(0));
      return ok(ids_v(add_polygon(d, pts, opt_vec(s.get("hint")))));
    }
    if (name == "polygon") {
      return ok(ids_v(add_polygon(d, vecs(s.get("pts")), opt_vec(s.get("hint")))));
    }
    if (name == "ngon") {
      const auto pts = ngon_pts(vec(s.get("c")), vec(s.get("n"), {0, 0, 1}), vec(s.get("u"), {1, 0, 0}),
                                s.get("r").num_or(0), to_count(s.get("sides").num_or(0), 0, 1000));
      const auto ids = add_polygon(d, pts, opt_vec(s.get("hint")));
      if (s.get("curve").is_string()) {
        const std::string c = tag_curve(d, pts, s.get("curve").as_string(), true);
        return ok(base::make_object({{"edges", ids_v(ids)}, {"curve", c}}));
      }
      return ok(ids_v(ids));
    }
    if (name == "edge") {
      AddEdgeOpts o;
      o.hint = opt_vec(s.get("hint"));
      o.faces = !s.get("faces").is_bool() || s.get("faces").as_bool();
      return ok(ids_v(add_edge(d, vec(s.get("p")), vec(s.get("q")), o)));
    }
    if (name == "arc") {
      const auto pts =
          arc_pts(vec(s.get("c")), vec(s.get("n"), {0, 0, 1}), vec(s.get("u"), {1, 0, 0}), s.get("r").num_or(0),
                  rad(s.get("sweep").num_or(0)), to_count(s.get("segs").num_or(0), 0, 1000));
      for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        (void)add_edge(d, pts[i], pts[i + 1]);
      }
      return ok(tag_curve(d, pts, "arc"));
    }
    if (name == "pushPull") {
      return ok(pp_v(push_pull(d, face(d, s.get("face")), s.get("d").num_or(0))));
    }
    if (name == "erase") {
      erase_sel(d, sel(d, s.get("sel")));
      return none();
    }
    if (name == "eraseEdges") {
      erase_edges(d, edges(d, s.get("edges")));
      return none();
    }
    if (name == "move" || name == "rotate" || name == "scale") {
      const Sel x = sel(d, s.get("sel"));
      Mat4 t = scaling(s.get("s").num_or(1), vec(s.get("about")));
      if (name == "move") {
        t = translation(vec(s.get("by")));
      } else if (name == "rotate") {
        t = rotation(vec(s.get("axis")), rad(s.get("deg").num_or(0)), vec(s.get("about")));
      }
      transform_sel(d, x, t);
      return none();
    }
    if (name == "copy") {
      const Sel out = copy_sel(d, sel(d, s.get("sel")));
      if (s.get("by").is_array()) {
        transform_sel(d, out, translation(vec(s.get("by"))));
      }
      return ok(sel_v(out));
    }
    if (name == "reverse") {
      reverse_faces(d, faces(d, s.get("faces")));
      return none();
    }
    if (name == "connected") {
      return ok(sel_v(connected(d, sel(d, s.get("sel")))));
    }
    if (name == "cleanup") {
      return ok(cleanup(d));
    }
    if (name == "tagCurve") {
      return ok(tag_curve(d, vecs(s.get("pts")), s.get("kind").as_string(), s.get("closed").truthy()));
    }
    if (name == "curveInfo") {
      return ok(curve_v(curve_info(d, s.get("curve").as_string())));
    }
    if (name == "curveRadius") {
      const auto r = set_curve_radius(d, s.get("curve").as_string(), s.get("r").num_or(0));
      return r ? ok(curve_v(*r)) : err_of(r);
    }
    if (name == "spline") {
      const int segs = s.get("segs").truthy() ? to_count(s.get("segs").as_number(), 1, 64) : kSplineSegs;
      const auto r = add_spline(d, vecs(s.get("pts")), s.get("closed").truthy(), segs);
      return r ? ok(*r) : err_of(r);
    }
    if (name == "editSpline") {
      std::optional<bool> closed;
      if (s.has("closed") && !s.get("closed").is_undefined()) {
        closed = s.get("closed").truthy();
      }
      std::optional<int> segs;
      if (s.get("segs").truthy()) {
        segs = to_count(s.get("segs").as_number(), 1, 64);
      }
      const auto r = edit_spline(d, s.get("curve").as_string(), vecs(s.get("pts")), closed, segs);
      return r ? ok(*r) : err_of(r);
    }
    if (name == "hide") {
      hide_sel(d, sel(d, s.get("sel")), !s.get("on").is_bool() || s.get("on").as_bool());
      return none();
    }
    if (name == "isolate") {
      return ok(Value(isolate_sel(d, sel(d, s.get("sel")))));
    }
    if (name == "unhideAll") {
      int n = 0;
      for (const std::string& id : m.def_ids()) {
        Def& x = *m.def_mut(id);
        for (auto& [k, e] : x.edges) {
          n += e.hidden ? 1 : 0;
          e.hidden = false;
        }
        for (auto& [k, f] : x.faces) {
          n += f.hidden ? 1 : 0;
          f.hidden = false;
        }
        for (auto& [k, i] : x.insts) {
          n += i.hidden ? 1 : 0;
          i.hidden = false;
        }
      }
      return ok(Value(n));
    }
    if (name == "flip") {
      return ok(sel_v(flip_sel(d, sel(d, s.get("sel")), vec(s.get("c")), vec(s.get("n")), s.get("copy").truthy())));
    }
    if (name == "follow") {
      const auto r = follow_me(d, face(d, s.get("face")), edges(d, s.get("along")));
      return r ? ok(base::make_object({{"faces", ids_v(r->faces)}, {"closed", r->closed}})) : err_of(r);
    }
    if (name == "chainThrough") {
      return ok(ids_v(chain_through(d, edge(d, s.get("edge")))));
    }
    if (name == "paint" || name == "paintInst") {
      std::optional<std::string> c;
      if (s.get("c").is_string()) {
        c = s.get("c").as_string();
      }
      const auto r = name == "paint" ? paint_faces(d, faces(d, s.get("faces")), c)
                                     : paint_instance(d, to_id(s.get("inst").num_or(-1)), c);
      return r ? none() : err_of(r);
    }
    if (name == "offsetFace") {
      const auto r = offset_face(d, face(d, s.get("face")), s.get("d").num_or(0));
      return r ? ok(base::make_object({{"face", opt_id(*r)}})) : err_of(r);
    }
    if (name == "offsetChain") {
      const auto r = offset_chain_pts(d, edges(d, s.get("edges")), s.get("d").num_or(0), vec(s.get("n"), {0, 0, 1}));
      if (!r) {
        return err_of(r);
      }
      Array pts;
      for (const Vec3& p : r->pts) {
        pts.push_back(pt_value(p));
      }
      return ok(base::make_object({{"pts", std::move(pts)}, {"closed", r->closed}, {"chain", ids_v(r->chain)}}));
    }
    if (name == "solidInfo") {
      return ok(info_v(solid_info(d)));
    }
    if (name == "solidProblems") {
      const SolidProblems p = solid_problems(d);
      return ok(base::make_object({{"holes", ids_v(p.holes)},
                                   {"extra", ids_v(p.extra)},
                                   {"inner", ids_v(p.inner)},
                                   {"stray", ids_v(p.stray)},
                                   {"flipped", ids_v(p.flipped)}}));
    }
    if (name == "fixSolid") {
      const FixSolid f = fix_solid(d);
      return ok(base::make_object({{"stray", Value(f.stray)},
                                   {"inner", Value(f.inner)},
                                   {"flipped", Value(f.flipped)},
                                   {"holes", Value(f.holes)}}));
    }
    if (name == "soften") {
      return ok(Value(soften_edges(d, edges(d, s.get("edges")), s.get("deg").num_or(0))));
    }
    if (name == "intersect") {
      const std::string others = s.get("others").is_string() ? s.get("others").as_string() : "model";
      return ok(Value(intersect_faces(m, path, sel(d, s.get("sel")), others)));
    }
    if (name == "group") {
      const std::string kind = s.get("kind").is_string() ? s.get("kind").as_string() : "group";
      const std::string nm = s.get("name").is_string() ? s.get("name").as_string() : std::string();
      return ok(opt_id(m.make_group(d, sel(d, s.get("sel")), kind, nm)));
    }
    if (name == "explode") {
      return ok(sel_v(m.explode(d, to_id(s.get("inst").num_or(-1)))));
    }
    if (name == "makeUnique") {
      const auto id = m.make_unique(d, to_id(s.get("inst").num_or(-1)));
      return ok(id ? Value(*id) : Value(nullptr));
    }
    if (name == "purge") {
      m.purge();
      return none();
    }
    if (name == "place") {
      return ok(Value(m.place(d, s.get("def").as_string(), s.has("m") ? mat(s.get("m")) : ident())));
    }
    if (name == "moveInto") {
      const auto r = m.move_into(idlist(s.get("from")), to_id(s.get("inst").num_or(-1)), idlist(s.get("to")));
      return r ? ok(Value(*r)) : err_of(r);
    }
    if (name == "copyClip") {
      clip = m.copy_clip(path, sel(d, s.get("sel")));
      if (!clip) {
        return none();
      }
      Value c = *clip;
      c.set("source", true);
      return ok(std::move(c));
    }
    if (name == "pasteClip") {
      Value c(nullptr);
      if (s.has("clip")) {
        c = s["clip"];
      } else if (clip) {
        c = *clip;
      }
      const Mat4 I = ident();
      std::vector<double> mm(I.begin(), I.end());
      if (s.has("m")) {  // as given: pasteClip checks it
        mm.clear();
        for (const Value& x : s.get("m").is_array() ? s.get("m").as_array() : Array{}) {
          mm.push_back(x.is_number() ? x.as_number() : std::nan(""));
        }
      }
      const auto r = m.paste_clip(path, c, mm);
      return r ? ok(sel_v(*r)) : err_of(r);
    }
    if (name == "guideLine") {
      return ok(Value(m.add_guide_line(vec(s.get("p")), vec(s.get("d")))));
    }
    if (name == "guidePoint") {
      return ok(Value(m.add_guide_point(vec(s.get("p")))));
    }
    if (name == "dim") {
      return ok(Value(m.add_dim(vec(s.get("a")), vec(s.get("b")), vec(s.get("off")), idlist(s.get("prefer")))));
    }
    if (name == "dimLength") {
      const auto r = m.set_dim_length(to_id(s.get("dim").num_or(-1)), s.get("L").num_or(0));
      return r ? ok(*r) : err_of(r);
    }
    if (name == "edgeLength") {
      const auto r = m.set_edge_length(path, edge(d, s.get("edge")), s.get("L").num_or(0));
      return r ? ok(*r) : err_of(r);
    }
    if (name == "defSize") {
      const auto r =
          m.set_def_size(s.get("def").as_string(), to_count(s.get("axis").num_or(0), 0, 2), s.get("size").num_or(0));
      return r ? none() : err_of(r);
    }
    if (name == "radialDim") {
      const std::string kind = s.get("kind").is_string() ? s.get("kind").as_string() : "rad";
      const auto r = m.add_radial_dim(path, s.get("curve").as_string(), vec(s.get("off")), kind);
      return r ? ok(Value(*r)) : err_of(r);
    }
    if (name == "angleDim") {
      return ok(Value(m.add_angle_dim(vec(s.get("o")), vec(s.get("a")), vec(s.get("b")), vec(s.get("off")),
                                      idlist(s.get("prefer")))));
    }
    if (name == "addTag") {
      return ok(m.add_tag(s.get("name").is_string() ? s.get("name").as_string() : std::string()));
    }
    if (name == "renameTag") {
      const auto r = m.rename_tag(s.get("old").as_string(), s.get("name").is_string() ? s.get("name").as_string() : "");
      return r ? ok(*r) : err_of(r);
    }
    if (name == "deleteTag") {
      return ok(Value(m.delete_tag(s.get("name").as_string())));
    }
    if (name == "setTag") {
      std::optional<std::string_view> t;
      std::string tn;
      if (s.get("name").is_string()) {
        tn = s.get("name").as_string();
        t = tn;
      }
      const auto r = m.set_tag(d, idlist(s.get("insts")), t);
      return r ? none() : err_of(r);
    }
    if (name == "tagHidden") {
      m.set_tag_hidden(s.get("name").as_string(), s.get("hidden").truthy());
      return none();
    }
    if (name == "tagCounts") {
      Array a;
      for (const auto& [k, n] : m.tag_counts()) {
        a.emplace_back(Array{Value(k), Value(n)});
      }
      return ok(std::move(a));
    }
    if (name == "section") {
      return ok(Value(m.add_section(vec(s.get("p")), vec(s.get("n")),
                                    s.get("name").is_string() ? s.get("name").as_string() : std::string())));
    }
    if (name == "deleteSection") {
      m.delete_section(to_id(s.get("id").num_or(-1)));
      return none();
    }
    if (name == "axes") {
      const auto r = m.set_axes(vec(s.get("o")), vec(s.get("x")), vec(s.get("y")));
      return r ? none() : err_of(r);
    }
    if (name == "resetAxes") {
      m.axes_m.reset();
      return none();
    }
    if (name == "light") {
      const auto r = m.add_light(s.get("light"));
      return r ? ok(Value(*r)) : err_of(r);
    }
    if (name == "setLight") {
      const auto r = m.set_light(to_id(s.get("id").num_or(-1)), s.get("patch"));
      return r ? ok(Value(*r)) : err_of(r);
    }
    if (name == "deleteLight") {
      return ok(Value(m.delete_light(to_id(s.get("id").num_or(-1)))));
    }
    if (name == "captureVis") {
      return ok(m.capture_vis());
    }
    if (name == "applyVis") {
      m.apply_vis(s.get("vis"));
      return none();
    }
    if (name == "undo" || name == "redo") {
      const auto label = name == "undo" ? m.undo() : m.redo();
      return ok(label ? Value(*label) : Value(nullptr));
    }
    if (name == "begin") {
      m.begin();
      return none();
    }
    if (name == "commit") {
      m.commit(s.get("label").is_string() ? s.get("label").as_string() : std::string());
      return none();
    }
    if (name == "abort") {
      m.abort();
      return none();
    }
    if (name == "load") {
      const auto r = m.load(s.get("json"));
      return r ? none() : err_of(r);
    }
    if (name == "meshDef" || name == "edgesDef") {
      const std::string nm = s.get("name").is_string() ? s.get("name").as_string() : std::string();
      std::vector<std::pair<Vec3, Vec3>> segs;
      for (const Value& sg : s.get("segs").is_array() ? s.get("segs").as_array() : Array{}) {
        if (sg.is_array() && sg.as_array().size() == 2) {
          segs.emplace_back(vec(sg.as_array()[0]), vec(sg.as_array()[1]));
        }
      }
      std::string id;
      if (name == "meshDef") {
        std::vector<MeshPoly> polys;
        for (const Value& p : s.get("polys").is_array() ? s.get("polys").as_array() : Array{}) {
          MeshPoly mp;
          if (p.is_array()) {
            mp.loops.push_back(vecs(p));
          } else {
            mp.object = true;
            for (const Value& ring : p.get("loops").is_array() ? p.get("loops").as_array() : Array{}) {
              mp.loops.push_back(vecs(ring));
            }
            if (p.get("color").is_string()) {
              mp.color = p.get("color").as_string();
            }
          }
          polys.push_back(std::move(mp));
        }
        const std::string kind = s.get("kind").is_string() ? s.get("kind").as_string() : "group";
        id = mesh_def(m, polys, nm, kind, segs).id;
      } else {
        id = edges_def(m, segs, nm).id;
      }
      auto cx = m.context(path);
      if (!cx) {
        return base::fail(base::Errc::invalid, "no such context");
      }
      Def& dd = *cx->def;
      return s.get("place").truthy() ? ok(Value(m.place(dd, id, ident()))) : ok(id);
    }
    if (name == "worldTriangles") {
      Array a;
      const std::vector<WorldTri> tris = world_triangles(m);
      if (tris.size() > 200000) {  // a result this big is a hostile script's (fuzz): only its size
        return ok(base::make_object({{"triangles", Value(tris.size())}}));
      }
      for (const WorldTri& t : tris) {
        Value o = base::make_object(
            {{"p", Array{pt_value(t.p[0]), pt_value(t.p[1]), pt_value(t.p[2])}}, {"n", pt_value(t.n)}});
        if (!t.color.empty()) {
          o.set("color", t.color);
        }
        a.push_back(std::move(o));
      }
      return ok(std::move(a));
    }
    return base::fail(base::Errc::invalid, "unknown op " + name);
  }

  Out run_one(const Value& s) {
    auto c = m.context(idlist(s.get("path")));
    if (!c) {
      return base::fail(base::Errc::invalid, "no such context");
    }
    return op(s, *c->def);
  }

  Out step(const Value& s) {
    if (!sane(s, 0)) {
      return base::fail(base::Errc::invalid, "a number in this step is out of range");
    }
    const std::string name = s.get("op").is_string() ? s.get("op").as_string() : std::string();
    if (name == "change") {
      const Array steps = s.get("steps").is_array() ? s.get("steps").as_array() : Array{};
      const auto r = m.change(s.get("label").is_string() ? s.get("label").as_string() : "", [&](Model& /*mm*/) {
        for (const Value& t : steps) {
          if (auto x = run_one(t); !x) {
            return base::Result<void>(std::unexpected(x.error()));
          }
        }
        return base::Result<void>();
      });
      return r ? none() : err_of(r);
    }
    if (name == "preview") {
      const Array steps = s.get("steps").is_array() ? s.get("steps").as_array() : Array{};
      (void)m.preview([&](Model& /*mm*/) {
        for (const Value& t : steps) {
          if (auto x = run_one(t); !x) {
            return base::Result<void>(std::unexpected(x.error()));
          }
        }
        return base::Result<void>();
      });
      return none();
    }
    return run_one(s);
  }
};

}  // namespace

Value run_script(const Value& script, std::size_t max_elements) {
  Runner r;
  if (script.get("units").is_string()) {
    r.m.units = script.get("units").as_string();
  }
  Array log;
  const Array steps = script.get("steps").is_array() ? script.get("steps").as_array() : Array{};
  for (const Value& s : steps) {
    Value e = base::make_object({{"op", s.get("op")}});
    try {
      auto out = r.step(s);
      if (out) {
        e.set("r", std::move(*out));
      } else {
        e.set("err", out.error().message);
      }
    } catch (const std::exception& ex) {  // the edit guard reports a kernel bug as the step's error
      e.set("err", std::string("internal: ") + ex.what());
    }
    e.set("model", r.m.to_json(false));
    log.push_back(std::move(e));
    if (max_elements != 0) {
      std::size_t n = 0;
      bool finite = true;
      for (const std::string& id : r.m.def_ids()) {
        const Def& d = *r.m.def(id);
        n += d.verts.size() + d.edges.size() + d.faces.size() + d.insts.size();
        for (const auto& [v, p] : d.verts) {
          finite = finite && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
        }
      }
      // the JS kernel makes NaN points from degenerate edits as this one does (checked against it), and its searches
      // then slow to minutes: a budgeted run stops there
      if (n > max_elements || !finite) {
        break;
      }
    }
  }
  Array undo;
  for (const std::string& l : r.m.undo_labels()) {
    undo.emplace_back(l);
  }
  Array redo;
  for (const std::string& l : r.m.redo_labels()) {
    redo.emplace_back(l);
  }
  return base::make_object(
      {{"name", script.get("name")}, {"steps", std::move(log)}, {"undo", std::move(undo)}, {"redo", std::move(redo)}});
}

}  // namespace cc::m3d
