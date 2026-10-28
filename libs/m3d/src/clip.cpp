// SPDX-License-Identifier: Apache-2.0
// model.js: the clipboard (copyClip / pasteClip), intersectFaces, meshDef / edgesDef, worldTriangles.
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/json.hpp"
#include "cc/m3d/model.hpp"
#include "files_internal.hpp"
#include "geom_internal.hpp"

namespace cc::m3d {

using base::Array;
using base::Value;
using namespace detail;  // NOLINT(google-build-using-namespace): the kernel's own helpers

std::optional<Value> Model::copy_clip(std::span<const Id> path, const Sel& sel) {
  Model scratch;
  const auto j = base::parse_json(base::to_json(to_json(false)));
  if (!j || !scratch.load(*j, true)) {
    return std::nullopt;
  }
  auto ctx = scratch.context(path);
  if (!ctx) {
    return std::nullopt;
  }
  Sel s;
  for (const Item& x : sel) {
    if (x.t != 'g') {
      s.push_back(x);
    }
  }
  const auto iid = scratch.make_group(*ctx->def, s, "group");
  if (!iid) {
    return std::nullopt;
  }
  ctx = scratch.context(path);
  const Model::Context& cx = must(ctx);
  const Inst inst = *cx.def->insts.get(must(iid));
  const Def* top = scratch.def(inst.def);
  OrderedMap<std::string, const Def*> need;
  // NOLINTNEXTLINE(misc-no-recursion): 32 levels at most, as model.js
  const std::function<void(const Def&, int)> visit = [&](const Def& d, int depth) {
    if (depth > 32) {
      return;
    }
    for (const auto& [k, i] : d.insts) {
      const Def* c = scratch.def(i.def);
      if (!need.has(i.def) && c != nullptr) {
        need.set(i.def, c);
        visit(*c, depth + 1);
      }
    }
  };
  visit(*top, 0);
  const auto box = scratch.bbox(top, ident());
  Array defs;
  for (const auto& [k, d] : need) {
    defs.push_back(def_to_json(*d));
  }
  return base::make_object(
      {{"format", "modelcraft-clip"},
       {"version", 1},
       {"source", uid},
       {"units", units},
       {"m", mat_value(mmul(cx.m, inst.m))},
       {"box", box ? base::make_object({{"min", pt_value(box->min)}, {"max", pt_value(box->max)}}) : Value(nullptr)},
       {"top", def_to_json(*top)},
       {"defs", std::move(defs)}});
}

base::Result<Sel> Model::paste_clip(std::span<const Id> path, const Value& clip, std::span<const double> m) {
  const auto fail = [](std::string_view key) { return base::fail(base::Errc::invalid, tr(key)); };
  const Value cdefs = clip.get("defs");
  if (!clip.is_object() || clip.get("format") != Value("modelcraft-clip") || clip.get("version") != Value(1) ||
      !clip.get("top").truthy() || !cdefs.is_array()) {
    return fail("m3d.model.the_clipboard_does_not_hold_a");
  }
  if (m.size() != 16 || !std::ranges::all_of(m, [](double x) { return std::isfinite(x) && std::abs(x) < 1e7; })) {
    return fail("m3d.model.the_place_is_wrong");
  }
  if (cdefs.as_array().size() > 10000) {
    return fail("m3d.model.the_copy_is_too_big");
  }
  if (!context(path)) {
    return fail("m3d.model.the_open_group_is_gone");
  }
  const Value src = clip.get("source");
  const bool same = src.is_string() && src.as_string() == uid;
  std::unordered_set<std::string> ids;
  for (const Value& d : cdefs.as_array()) {
    if (d.truthy() && d.get("id").is_string()) {
      ids.insert(d.get("id").as_string());
    }
  }
  OrderedMap<std::string, std::string> map;
  for (const Value& dj : cdefs.as_array()) {
    const Value id = dj.get("id");
    const auto good = [](const std::string& s) {
      return s.size() >= 2 && s.size() <= 10 && s[0] == 'd' &&
             std::all_of(s.begin() + 1, s.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    if (!dj.truthy() || !id.is_string() || !good(id.as_string())) {
      return fail("m3d.model.a_definition_id_is_wrong");
    }
    const Def* have = def(id.as_string());
    if (same && dj.get("kind") == Value("component") && have != nullptr && have->kind == "component") {
      map.set(id.as_string(), id.as_string());
    }
  }
  std::unordered_set<std::string> open;
  {
    const Def* d = &root();
    for (const Id iid : path) {
      d = def(d->insts.get(iid)->def);
      open.insert(d->id);
    }
  }
  std::unordered_set<std::string> reach;
  // NOLINTNEXTLINE(misc-no-recursion): 64 levels at most, as model.js
  const std::function<void(const std::string&, int)> walk_to = [&](const std::string& id, int depth) {
    if (reach.contains(id) || depth > 64) {
      return;
    }
    reach.insert(id);
    if (const Def* x = def(id)) {
      for (const auto& [k, i] : x->insts) {
        walk_to(i.def, depth + 1);
      }
    }
  };
  {
    OrderedSet<std::string> reused;
    for (const auto& [k, v] : map) {
      reused.add(v);
    }
    for (const std::string& id : reused) {
      walk_to(id, 0);
    }
  }
  for (const std::string& id : open) {
    if (reach.contains(id)) {
      return fail("m3d.model.a_component_cannot_be_pasted_inside");
    }
  }
  std::unordered_set<std::string> names;
  for (const auto& [k, d] : defs_) {
    names.insert(d->name);
  }
  for (const Value& dj : cdefs.as_array()) {
    const std::string id = dj.get("id").as_string();
    if (map.has(id)) {
      continue;
    }
    const std::string kind = dj.get("kind") == Value("component") ? "component" : "group";
    const std::string nm = clean_text(dj.get("name"), 120);
    map.set(id, new_def(kind, !nm.empty() && !names.contains(nm) ? nm : std::string()).id);
  }
  const auto remap = [&](const Value& j) -> base::Result<Value> {
    Value out = j;
    Array insts;
    const Value ji = j.get("insts");
    if (ji.is_array()) {
      for (const Value& i : ji.as_array()) {
        if (!i.truthy() || !i.get("def").is_string() || !ids.contains(i.get("def").as_string())) {
          return base::fail(base::Errc::invalid, tr("m3d.model.an_instance_points_at_a_missing"));
        }
        Value c = i;
        c.set("def", *map.get(i.get("def").as_string()));
        insts.push_back(std::move(c));
      }
    }
    out.set("insts", std::move(insts));
    return out;
  };
  for (const Value& dj : cdefs.as_array()) {
    const std::string id = dj.get("id").as_string();
    const std::string to = *map.get(id);
    if (to == id) {
      continue;
    }
    auto r = remap(dj);
    if (!r) {
      return std::unexpected(r.error());
    }
    if (auto ok = def_from_json(*def_mut(to), *r); !ok) {
      return std::unexpected(ok.error());
    }
  }
  const Def& tmp = new_def("group", "Paste");
  const std::string tmp_id = tmp.id;
  auto top = remap(clip.get("top"));
  if (!top) {
    return std::unexpected(top.error());
  }
  if (auto ok = def_from_json(*def_mut(tmp_id), *top); !ok) {
    return std::unexpected(ok.error());
  }
  Model::Context ctx = must(context(path));
  Mat4 mm{};
  std::ranges::copy(m, mm.begin());
  const Id iid = place(*ctx.def, tmp_id, mmul(ctx.inv, mm));
  ensure_tags();
  ctx = must(context(path));
  return explode(*ctx.def, iid);
}

// ---------------------------------------------------------------- intersect faces (M19)

std::size_t intersect_faces(Model& model, std::span<const Id> path, const Sel& sel, std::string_view others) {
  auto ctx = model.context(path);
  if (!ctx) {
    return 0;
  }
  struct Poly {
    std::string key;
    std::vector<std::vector<Vec3>> loops;
    Vec3 n{};
    double d = 0;
    Vec3 lo{}, hi{};
  };
  const auto polys = [](const Def& def, const Mat4& m, const std::function<bool(Id)>* filter, std::vector<Poly>& out) {
    std::string mk;
    for (const double x : m) {
      if (!mk.empty()) {
        mk += ',';
      }
      base::append_number(mk, x);
    }
    for (const auto& [fid, f] : def.faces) {
      if (f.hidden || (filter != nullptr && !(*filter)(fid))) {
        continue;
      }
      Poly p;
      for (const Loop& l : f.loops) {
        std::vector<Vec3> L;
        for (const Id v : l) {
          L.push_back(m3d::apply(m, def.vert(v)));
        }
        p.loops.push_back(std::move(L));
      }
      p.n = norm(newell(p.loops[0]));
      if (len(p.n) == 0) {
        continue;
      }
      p.lo = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
              std::numeric_limits<double>::infinity()};
      p.hi = neg(p.lo);
      for (const Vec3& q : p.loops[0]) {
        for (std::size_t k = 0; k < 3; ++k) {
          p.lo.at(k) = std::min(p.lo.at(k), q.at(k));
          p.hi.at(k) = std::max(p.hi.at(k), q.at(k));
        }
      }
      p.key = def.id + ":" + std::to_string(fid) + ":" + mk;
      p.d = dot(p.n, p.loops[0][0]);
      out.push_back(std::move(p));
    }
  };
  std::vector<Poly> A;
  std::unordered_set<Id> fids;
  for (const Item& s : sel) {
    if (s.t == 'f') {
      fids.insert(s.id);
    }
  }
  if (!fids.empty()) {
    const std::function<bool(Id)> f = [&](Id id) { return fids.contains(id); };
    polys(*ctx->def, ctx->m, &f, A);
  }
  for (const Item& s : sel) {
    if (s.t != 'i') {
      continue;
    }
    const Inst* inst = ctx->def->insts.get(s.id);
    if (inst == nullptr) {
      continue;
    }
    model.walk(
        [&](const Def& d, const Mat4& m, const std::vector<Id>& /*p*/, const std::string& /*c*/) {
          polys(d, m, nullptr, A);
          return true;
        },
        model.def(inst->def), mmul(ctx->m, inst->m));
  }
  std::vector<Poly> Bown;
  const std::vector<Poly>* B = &A;
  if (others == "model") {
    model.walk_shown([&](const Def& d, const Mat4& m, const std::vector<Id>& /*p*/, const std::string& /*c*/) {
      polys(d, m, nullptr, Bown);
      return true;
    });
    B = &Bown;
  }
  const auto spans = [](const Poly& F, const Vec3& x0, const Vec3& u) {
    const Vec3 w = cross(F.n, u);
    std::vector<double> ts;
    for (const auto& loop : F.loops) {
      for (std::size_t i = 0; i < loop.size(); ++i) {
        const Vec3& P = loop[i];
        const Vec3& Q = loop[(i + 1) % loop.size()];
        const double sp = dot(sub(P, x0), w);
        const double sq = dot(sub(Q, x0), w);
        if ((sp > 0) == (sq > 0)) {
          continue;
        }
        const Vec3 c = add(P, mul(sub(Q, P), sp / (sp - sq)));
        ts.push_back(dot(sub(c, x0), u));
      }
    }
    std::ranges::stable_sort(ts);
    std::vector<std::pair<double, double>> out;
    for (std::size_t i = 0; i + 1 < ts.size(); i += 2) {
      if (ts[i + 1] - ts[i] > kEps) {
        out.emplace_back(ts[i], ts[i + 1]);
      }
    }
    return out;
  };
  std::vector<std::pair<Vec3, Vec3>> segs;
  std::unordered_set<std::string> seen;
  for (const Poly& a : A) {
    for (const Poly& b : *B) {
      if (a.key == b.key) {
        continue;
      }
      const std::string pk = a.key < b.key ? a.key + "|" + b.key : b.key + "|" + a.key;
      if (seen.contains(pk)) {
        continue;
      }
      seen.insert(pk);
      bool apart = false;
      for (std::size_t k = 0; k < 3; ++k) {
        apart = apart || a.hi.at(k) < b.lo.at(k) - kEps || b.hi.at(k) < a.lo.at(k) - kEps;
      }
      if (apart) {
        continue;
      }
      const Vec3 dir = cross(a.n, b.n);
      const double L = len(dir);
      if (L < 1e-9) {
        continue;
      }
      const Vec3 u = mul(dir, 1 / L);
      const Vec3 x0 = mul(cross(sub(mul(b.n, a.d), mul(a.n, b.d)), dir), 1 / (L * L));
      const auto sa = spans(a, x0, u);
      const auto sb = spans(b, x0, u);
      for (const auto& [a0, a1] : sa) {
        for (const auto& [b0, b1] : sb) {
          const double t0 = std::max(a0, b0);
          const double t1 = std::min(a1, b1);
          if (t1 - t0 > kEps) {
            segs.emplace_back(add(x0, mul(u, t0)), add(x0, mul(u, t1)));
          }
        }
      }
    }
  }
  const std::size_t before = ctx->def->edges.size();
  for (const auto& [p, q] : segs) {
    (void)add_edge(*ctx->def, m3d::apply(ctx->inv, p), m3d::apply(ctx->inv, q));
  }
  return ctx->def->edges.size() - before;
}

// ---------------------------------------------------------------- imports: meshDef, edgesDef

namespace {

std::string grid_key(const Vec3& p) {
  std::string k;
  for (std::size_t i = 0; i < 3; ++i) {
    if (i != 0) {
      k += ',';
    }
    const double r = base::js_round(p.at(i) * 1e5);
    base::append_number(k, r == 0 ? 0.0 : r);  // String(-0) is "0"
  }
  return k;
}

}  // namespace

Def& mesh_def(Model& model, std::span<const MeshPoly> polys, std::string_view name, std::string_view kind,
              std::span<const std::pair<Vec3, Vec3>> segs, Kernel /*k*/) {
  Def& d = model.new_def(kind, name);
  std::unordered_map<std::string, Id> grid;
  const auto vid = [&](const Vec3& p) {
    const std::string key = grid_key(p);
    const auto it = grid.find(key);
    if (it != grid.end()) {
      return it->second;
    }
    const Id id = v_add(d, p);
    grid.emplace(key, id);
    return id;
  };
  const auto ring = [&](const std::vector<Vec3>& pts) -> std::optional<Loop> {
    Loop loop;
    for (const Vec3& p : pts) {
      const Id v = vid(p);
      if (loop.empty() || loop.back() != v) {
        loop.push_back(v);
      }
    }
    while (loop.size() > 1 && loop.front() == loop.back()) {
      loop.pop_back();
    }
    if (loop.size() < 3 || std::unordered_set<Id>(loop.begin(), loop.end()).size() != loop.size()) {
      return std::nullopt;
    }
    if (poly_area3(d.points(loop)) <= kEps2) {
      return std::nullopt;
    }
    return loop;
  };
  for (const MeshPoly& poly : polys) {
    if (poly.loops.empty()) {
      continue;
    }
    const auto outer = ring(poly.loops[0]);
    if (!outer) {
      continue;
    }
    const std::vector<Vec3> P = d.points(*outer);
    const Vec3 n = newell(P);
    double size = 0;
    for (const Vec3& p : P) {
      size = std::max(size, dist(p, P[0]));
    }
    const double tol = std::max(kEps, size * 1e-6);
    if (std::ranges::any_of(P, [&](const Vec3& p) { return std::abs(dot(n, sub(p, P[0]))) > tol; })) {
      for (std::size_t i = 1; i + 1 < outer->size(); ++i) {
        Loop t = {(*outer)[0], (*outer)[i], (*outer)[i + 1]};
        if (poly_area3(d.points(t)) > kEps2) {
          (void)f_add(d, {std::move(t)}, poly.color);
        }
      }
      continue;
    }
    std::vector<Loop> loops = {*outer};
    if (poly.object) {
      for (std::size_t r = 1; r < poly.loops.size(); ++r) {
        auto h = ring(poly.loops[r]);
        if (!h || std::ranges::any_of(*h, [&](Id v) { return std::ranges::find(*outer, v) != outer->end(); })) {
          continue;
        }
        if (dot(newell(d.points(*h)), n) > 0) {
          std::ranges::reverse(*h);
        }
        loops.push_back(std::move(*h));
      }
    }
    (void)f_add(d, std::move(loops), poly.color);
  }
  for (const auto& [p, q] : segs) {
    const Id a = vid(p);
    const Id b = vid(q);
    if (a != b) {
      (void)e_add(d, a, b);
    }
  }
  const auto ef = edge_faces(d);
  for (auto& [eid, e] : d.edges) {
    const auto it = ef.find(edge_key(e.a, e.b));
    if (it == ef.end() || it->second.size() != 2) {
      continue;
    }
    if (dot(face_normal(d, *d.faces.get(it->second[0])), face_normal(d, *d.faces.get(it->second[1]))) > 1 - 1e-6) {
      e.soft = true;
    }
  }
  return d;
}

Def& mesh_def(Model& model, const std::vector<std::vector<Vec3>>& polys, std::string_view name) {
  std::vector<MeshPoly> mp;
  mp.reserve(polys.size());
  for (const auto& p : polys) {
    mp.push_back(MeshPoly{{p}, {}, false});
  }
  return mesh_def(model, mp, name, "group", {}, Kernel::cadcraft);
}

Def& edges_def(Model& model, std::span<const std::pair<Vec3, Vec3>> segs, std::string_view name) {
  Def& d = model.new_def("group", name);
  std::unordered_map<std::string, Id> grid;
  const auto vid = [&](const Vec3& p) {
    const std::string key = grid_key(p);
    const auto it = grid.find(key);
    if (it != grid.end()) {
      return it->second;
    }
    const Id id = v_add(d, p);
    grid.emplace(key, id);
    return id;
  };
  for (const auto& [p, q] : segs) {
    const Id a = vid(p);
    const Id b = vid(q);
    if (a != b) {
      (void)e_add(d, a, b);
    }
  }
  return d;
}

std::vector<WorldTri> world_triangles(const Model& model, const Def* from, const Mat4& m0) {
  std::vector<WorldTri> out;
  model.walk(
      [&](const Def& d, const Mat4& wm, const std::vector<Id>& /*p*/, const std::string& pc) {
        const bool mirror = det3(wm) < 0;
        for (const auto& [fid, f] : d.faces) {
          const Vec3 n = norm(apply_dir(wm, face_normal(d, f)));
          for (const auto& t : face_tris(d, f)) {
            std::array<Vec3, 3> ps = {m3d::apply(wm, d.vert(t[0])), m3d::apply(wm, d.vert(t[1])),
                                      m3d::apply(wm, d.vert(t[2]))};
            if (mirror) {
              std::swap(ps[1], ps[2]);
            }
            const Vec3 fn = norm(cross(sub(ps[1], ps[0]), sub(ps[2], ps[0])));
            out.push_back({ps, len(fn) != 0 ? fn : n, !f.color.empty() ? f.color : pc});
          }
        }
        return true;
      },
      from != nullptr ? from : &model.root(), m0);
  return out;
}

}  // namespace cc::m3d
