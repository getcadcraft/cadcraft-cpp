// SPDX-License-Identifier: Apache-2.0
// model.js, the Model: definitions, walking, groups and components, the clipboard, guides, dimensions, tags,
// sections, axes, lights, scenes' visibility, undo, files.
#include "cc/m3d/model.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/json.hpp"
#include "cc/i18n/i18n.hpp"
#include "files_internal.hpp"
#include "geom_internal.hpp"

namespace cc::m3d {

using base::Array;
using base::Value;
using namespace detail;  // NOLINT(google-build-using-namespace): the kernel's own helpers

namespace {

// i18n.js keeps its catalog module-wide too; the app sets it once at start (and the tests)
const i18n::Catalog* g_words = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

std::string num_text(double x) {
  std::string s;
  base::append_number(s, x);
  return s;
}

bool same_ids(std::span<const Id> a, std::span<const Id> b) {
  return std::ranges::equal(a, b);
}

}  // namespace

void set_words(const i18n::Catalog* words) noexcept {
  g_words = words;
}

std::string tr(std::string_view key, std::initializer_list<std::pair<std::string_view, std::string>> fills) {
  if (g_words == nullptr) {
    return std::string(key);
  }
  return g_words->t(key, fills);
}

// ---------------------------------------------------------------- the model's tables

Model::Model() {
  reset();
}

Model::Model(const Model& o)
    : units(o.units),
      uid(o.uid),
      next_id(o.next_id),
      version(o.version),
      dirty(o.dirty),
      saved_view(o.saved_view),
      guides(o.guides),
      lights(o.lights),
      dims(o.dims),
      sections(o.sections),
      active_section(o.active_section),
      view_planes(o.view_planes),
      view_cuts(o.view_cuts),
      storeys(o.storeys),
      axes_m(o.axes_m),
      scenes(o.scenes),
      tags(o.tags),
      images(o.images),
      undo_(o.undo_),
      redo_(o.redo_),
      preview_(o.preview_) {
  for (const auto& [k, d] : o.defs_) {
    defs_.emplace_back(k, std::make_unique<Def>(*d));
  }
  reindex();
}

Model& Model::operator=(const Model& o) {
  if (this != &o) {
    Model copy(o);
    *this = std::move(copy);
  }
  return *this;
}

Model::Model(Model&&) noexcept = default;
Model& Model::operator=(Model&&) noexcept = default;
Model::~Model() = default;

void Model::reindex() {
  index_.clear();
  for (std::size_t i = 0; i < defs_.size(); ++i) {
    index_.emplace(defs_[i].first, i);
  }
}

namespace {

std::string new_uid() {
  // Date.now().toString(36) + '-' + random + '-' + counter in the JS: only its uniqueness matters
  static std::uint64_t n = 0;
  std::random_device rd;
  const std::uint64_t r = (static_cast<std::uint64_t>(rd()) << 32U) ^ rd();
  return std::to_string(r) + "-" + std::to_string(n++);
}

}  // namespace

void Model::reset() {
  uid = new_uid();
  defs_.clear();
  defs_.emplace_back("root", std::make_unique<Def>("root", "root", "Model"));
  reindex();
  guides.clear();
  lights.clear();
  dims.clear();
  sections.clear();
  active_section.reset();
  storeys.clear();
  axes_m.reset();
  view_planes = true;
  view_cuts = true;
  scenes = Array{};
  tags.clear();
  units = "ft-in";
  next_id = 1;
  version = 0;
  undo_.clear();
  redo_.clear();
  dirty = false;
  editing_ = 0;
  preview_.reset();
  saved_view = Value(nullptr);
}

const Def* Model::def(std::string_view id) const {
  const auto it = index_.find(std::string(id));
  return it == index_.end() ? nullptr : defs_[it->second].second.get();
}

Def* Model::def_mut(std::string_view id) {
  const auto it = index_.find(std::string(id));
  return it == index_.end() ? nullptr : defs_[it->second].second.get();
}

std::vector<std::string> Model::def_ids() const {
  std::vector<std::string> out;
  out.reserve(defs_.size());
  for (const auto& [k, d] : defs_) {
    out.push_back(k);
  }
  return out;
}

std::size_t Model::def_count() const noexcept {
  return defs_.size();
}

void Model::erase_def(const std::string& id) {
  const auto it = index_.find(id);
  if (it == index_.end()) {
    return;
  }
  defs_.erase(defs_.begin() + static_cast<std::ptrdiff_t>(it->second));
  reindex();
}

void Model::set_def(std::unique_ptr<Def> d) {
  const std::string id = d->id;
  const auto it = index_.find(id);
  if (it != index_.end()) {
    defs_[it->second].second = std::move(d);
    return;
  }
  index_.emplace(id, defs_.size());
  defs_.emplace_back(id, std::move(d));
}

std::string Model::unique_name(std::string_view base_name) const {
  std::unordered_set<std::string> names;
  for (const auto& [k, d] : defs_) {
    names.insert(d->name);
  }
  for (int i = 1;; ++i) {
    std::string n = std::string(base_name) + "#" + std::to_string(i);
    if (!names.contains(n)) {
      return n;
    }
  }
}

Def& Model::new_def(std::string_view kind, std::string_view name) {
  std::string id = "d" + std::to_string(next_id++);
  std::string nm = !name.empty() ? std::string(name) : unique_name(kind == "component" ? "Component" : "Group");
  auto d = std::make_unique<Def>(id, std::string(kind), std::move(nm));
  Def& ref = *d;
  set_def(std::move(d));
  return ref;
}

// ---------------------------------------------------------------- walking

// NOLINTNEXTLINE(misc-no-recursion): 32 levels at most, as model.js
void Model::walk(const WalkFn& cb, const Def* from, const Mat4& m, const std::vector<Id>& path, int depth,
                 const std::string& color) const {
  if (depth > 32) {
    return;
  }
  // a hostile file can place a definition twice inside each of 32 levels: 2^32 visits. The JS has no cap; here a walk
  // visits at most kMaxWalk placements (no real model comes near it: docs/gaps.md)
  if (depth == 0) {
    walk_budget_ = kMaxWalk;
  } else {
    if (walk_budget_ == 0) {
      return;
    }
    --walk_budget_;
  }
  const Def& d = from != nullptr ? *from : root();
  if (!cb(d, m, path, color)) {
    return;
  }
  for (std::size_t i = 0; i < d.insts.slots(); ++i) {
    const auto* kv = d.insts.at(i);
    if (kv == nullptr) {
      continue;
    }
    const Id iid = kv->first;
    const Inst inst = kv->second;
    if (const Def* c = def(inst.def)) {
      std::vector<Id> p = path;
      p.push_back(iid);
      walk(cb, c, mmul(m, inst.m), p, depth + 1, !inst.color.empty() ? inst.color : color);
    }
  }
}

void Model::walk(const std::function<void(const Def&, const Mat4&)>& cb) const {
  walk([&](const Def& d, const Mat4& m, const std::vector<Id>& /*path*/, const std::string& /*color*/) {
    cb(d, m);
    return true;
  });
}

void Model::walk_shown(const WalkFn& cb) const {
  std::unordered_set<std::string> off;
  for (const TagDef& g : tags) {
    if (g.hidden) {
      off.insert(g.name);
    }
  }
  std::uint64_t budget = kMaxWalk;  // as walk()
  // NOLINTNEXTLINE(misc-no-recursion): 32 levels at most, as model.js
  const std::function<void(const Def&, const Mat4&, std::vector<Id>, int, const std::string&)> go =
      [&](const Def& d, const Mat4& m, const std::vector<Id>& path, int depth, const std::string& color) {
        if (depth > 32 || budget == 0) {
          return;
        }
        --budget;
        if (!cb(d, m, path, color)) {
          return;
        }
        for (const auto& [iid, inst] : d.insts) {
          if (inst.hidden || (!inst.tag.empty() && off.contains(inst.tag))) {
            continue;
          }
          if (const Def* c = def(inst.def)) {
            std::vector<Id> p = path;
            p.push_back(iid);
            go(*c, mmul(m, inst.m), std::move(p), depth + 1, !inst.color.empty() ? inst.color : color);
          }
        }
      };
  go(root(), ident(), {}, 0, {});
}

std::optional<Model::Context> Model::context(std::span<const Id> path) {
  Def* d = &root();
  Mat4 m = ident();
  for (const Id iid : path) {
    const Inst* inst = d->insts.get(iid);
    if (inst == nullptr) {
      return std::nullopt;
    }
    m = mmul(m, inst->m);
    d = def_mut(inst->def);
    if (d == nullptr) {
      return std::nullopt;
    }
  }
  return Context{d, m, invert(m)};
}

std::optional<Box> Model::bbox(const Def* d, const Mat4& m) const {
  std::optional<Box> bb;
  walk(
      [&](const Def& x, const Mat4& wm, const std::vector<Id>& /*p*/, const std::string& /*c*/) {
        for (const auto& [v, p] : x.verts) {
          bbox_add(bb, m3d::apply(wm, p));
        }
        return true;
      },
      d != nullptr ? d : &root(), m);
  return bb;
}

std::map<std::string, std::size_t> Model::instance_counts() const {
  std::map<std::string, std::size_t> c;
  for (const auto& [k, d] : defs_) {
    for (const auto& [iid, inst] : d->insts) {
      ++c[inst.def];
    }
  }
  return c;
}

std::string Model::path_color(std::span<const Id> path) const {
  const Def* d = &root();
  std::string color;
  for (const Id iid : path) {
    const Inst* inst = d != nullptr ? d->insts.get(iid) : nullptr;
    if (inst == nullptr) {
      break;
    }
    if (!inst->color.empty()) {
      color = inst->color;
    }
    d = def(inst->def);
  }
  return color;
}

bool Model::empty() const {
  const Def& r = root();
  return r.faces.empty() && r.edges.empty() && r.insts.empty();
}

// ---------------------------------------------------------------- groups and components

std::optional<Id> Model::make_group(Def& d, const Sel& sel, std::string_view kind, std::string_view name) {
  std::vector<Id> fids;
  OrderedSet<Id> eids;
  std::vector<Id> iids;
  for (const Item& s : sel) {
    if (s.t == 'f' && d.faces.has(s.id)) {
      fids.push_back(s.id);
    }
  }
  for (const Item& s : sel) {
    if (s.t == 'e' && d.edges.has(s.id)) {
      eids.add(s.id);
    }
  }
  for (const Id fid : fids) {
    for (const auto& [a, b] : loop_segs(*d.faces.get(fid))) {
      if (const auto e = d.edge_between(a, b)) {
        eids.add(*e);
      }
    }
  }
  for (const Item& s : sel) {
    if (s.t == 'i' && d.insts.has(s.id)) {
      iids.push_back(s.id);
    }
  }
  if (fids.empty() && eids.empty() && iids.empty()) {
    return std::nullopt;
  }
  std::optional<Box> bb;
  for (const Id eid : eids) {
    const Edge& e = *d.edges.get(eid);
    bbox_add(bb, d.vert(e.a));
    bbox_add(bb, d.vert(e.b));
  }
  for (const Id iid : iids) {
    const Inst& inst = *d.insts.get(iid);
    if (const auto ib = bbox(def(inst.def), inst.m)) {
      bbox_add(bb, ib->min);
      bbox_add(bb, ib->max);
    }
  }
  const Vec3 origin = bb ? bb->min : Vec3{0, 0, 0};
  Def& nd = new_def(kind, name);
  std::unordered_map<Id, Id> vmap;
  const auto cv = [&](Id v) {
    const auto it = vmap.find(v);
    if (it != vmap.end()) {
      return it->second;
    }
    const Id n = v_add(nd, sub(d.vert(v), origin));
    vmap.emplace(v, n);
    return n;
  };
  for (const Id eid : eids) {
    const Edge e = *d.edges.get(eid);
    const Id a = cv(e.a);
    const Id b = cv(e.b);
    (void)e_add(nd, a, b, e.soft, e.curve, e.hidden);
  }
  OrderedSet<std::string> cids;
  for (const Id eid : eids) {
    const std::string& c = d.edges.get(eid)->curve;
    if (!c.empty() && d.splines.has(c)) {
      cids.add(c);
    }
  }
  for (const std::string& cid : cids) {
    Spline sp = *d.splines.get(cid);
    for (Vec3& p : sp.pts) {
      p = sub(p, origin);
    }
    nd.splines.set(cid, std::move(sp));
  }
  for (const Id fid : fids) {
    const Face f = *d.faces.get(fid);
    std::vector<Loop> loops;
    for (const Loop& l : f.loops) {
      Loop c;
      for (const Id v : l) {
        c.push_back(cv(v));
      }
      loops.push_back(std::move(c));
    }
    (void)f_add(nd, std::move(loops), f.color, std::nullopt, f.hidden);
  }
  const Mat4 back = translation(neg(origin));
  for (const Id iid : iids) {
    Inst inst = *d.insts.get(iid);
    inst.m = mmul(back, inst.m);
    nd.insts.set(nd.next++, std::move(inst));
    d.insts.erase(iid);
  }
  for (const Id fid : fids) {
    d.faces.erase(fid);
  }
  const auto still = edge_faces(d);
  for (const Id eid : eids) {
    const Edge* e = d.edges.get(eid);
    if (e != nullptr && !still.contains(edge_key(e->a, e->b))) {
      e_remove(d, eid);
    }
  }
  remove_loose_verts(d);
  prune_splines(d);
  const Id iid = d.next++;
  d.insts.set(
      iid,
      Inst{.def = nd.id, .m = translation(origin), .name = {}, .color = {}, .hidden = false, .tag = {}, .rule = {}});
  return iid;
}

Sel Model::explode(Def& d, Id iid) {
  const Inst* ip = d.insts.get(iid);
  if (ip == nullptr) {
    return {};
  }
  const Inst inst = *ip;
  const Def* src = def(inst.def);
  d.insts.erase(iid);
  Sel out;
  if (src != nullptr) {
    const Mat4& m = inst.m;
    const bool mirror = det3(m) < 0;
    std::unordered_map<Id, Id> vmap;
    const auto cv = [&](Id v) {
      const auto it = vmap.find(v);
      if (it != vmap.end()) {
        return it->second;
      }
      const Id n = vertex_at(d, m3d::apply(m, src->vert(v)));
      vmap.emplace(v, n);
      return n;
    };
    CurveRemapper rc(d);
    for (const auto& [eid, e] : src->edges) {
      const Id a = cv(e.a);
      const Id b = cv(e.b);
      const std::string c = rc(e.curve);
      const auto id = e_add(d, a, b, e.soft, c, e.hidden);
      out.push_back({'e', id.value_or(-1)});
    }
    for (const auto& [cid, sp] : src->splines) {
      Spline s = sp;
      for (Vec3& p : s.pts) {
        p = m3d::apply(m, p);
      }
      d.splines.set(rc(cid), std::move(s));
    }
    for (const auto& [fid, f] : src->faces) {
      std::vector<Loop> loops;
      for (const Loop& l : f.loops) {
        Loop c;
        for (const Id v : l) {
          c.push_back(cv(v));
        }
        if (mirror) {
          std::ranges::reverse(c);
        }
        loops.push_back(std::move(c));
      }
      out.push_back({'f', f_add(d, std::move(loops), f.color, std::nullopt, f.hidden)});
    }
    for (const auto& [cid, child] : src->insts) {
      const Id id = d.next++;
      Inst c = child;
      c.m = mmul(m, child.m);
      d.insts.set(id, std::move(c));
      out.push_back({'i', id});
    }
    (void)cleanup(d);
  }
  purge();
  Sel kept;
  for (const Item& s : out) {
    bool has = d.insts.has(s.id);
    if (s.t == 'e') {
      has = d.edges.has(s.id);
    } else if (s.t == 'f') {
      has = d.faces.has(s.id);
    }
    if (has) {
      kept.push_back(s);
    }
  }
  return kept;
}

std::optional<std::string> Model::make_unique(Def& d, Id iid) {
  const Inst* inst = d.insts.get(iid);
  if (inst == nullptr) {
    return std::nullopt;
  }
  const Def* src = def(inst->def);
  if (src == nullptr) {
    return std::nullopt;
  }
  const std::string kind = src->kind;
  std::string name;
  if (kind == "component") {
    std::string base_name = src->name;
    // .replace(/#\d+$/, '')
    std::size_t k = base_name.size();
    while (k > 0 && base_name[k - 1] >= '0' && base_name[k - 1] <= '9') {
      --k;
    }
    if (k < base_name.size() && k > 0 && base_name[k - 1] == '#') {
      base_name.resize(k - 1);
    }
    name = unique_name(base_name);
  }
  const Value j = def_to_json(*src);
  Def& nd = new_def(kind, name);
  if (!def_from_json(nd, j)) {
    return std::nullopt;  // its own JSON always reads back
  }
  d.insts.get(iid)->def = nd.id;
  return nd.id;
}

void Model::purge() {
  for (;;) {
    const auto used = instance_counts();
    std::unordered_set<std::string> origin;
    for (const auto& [k, d] : defs_) {
      if (d->dyn && used.contains(d->id)) {
        origin.insert(d->dyn->src);
      }
    }
    std::vector<std::string> dead;
    for (const auto& [k, d] : defs_) {
      if (k != "root" && !used.contains(k) && !origin.contains(k)) {
        dead.push_back(k);
      }
    }
    if (dead.empty()) {
      break;
    }
    for (const std::string& id : dead) {
      erase_def(id);
    }
  }
}

Id Model::place(Def& in, const std::string& def_id, const Mat4& m) {
  const Id iid = in.next++;
  in.insts.set(iid, Inst{.def = def_id, .m = m, .name = {}, .color = {}, .hidden = false, .tag = {}, .rule = {}});
  return iid;
}

base::Result<Id> Model::move_into(std::span<const Id> from, Id iid, std::span<const Id> to) {
  auto src = context(from);
  auto dst = context(to);
  const Inst* ip = src ? src->def->insts.get(iid) : nullptr;
  if (ip == nullptr || !dst) {
    return base::fail(base::Errc::invalid, tr("m3d.model.move_gone"));
  }
  const Inst inst = *ip;
  std::unordered_set<std::string> reach;
  // NOLINTNEXTLINE(misc-no-recursion): 64 levels at most, as model.js
  const std::function<void(const std::string&, int)> visit = [&](const std::string& id, int depth) {
    if (reach.contains(id) || depth > 64) {
      return;
    }
    reach.insert(id);
    if (const Def* x = def(id)) {
      for (const auto& [k, i] : x->insts) {
        visit(i.def, depth + 1);
      }
    }
  };
  visit(inst.def, 0);
  const Def* d = &root();
  for (const Id p : to) {
    const Inst* i = d->insts.get(p);
    if (i == nullptr) {
      break;
    }
    if (reach.contains(i->def)) {
      return base::fail(base::Errc::invalid, tr("m3d.model.move_into_itself"));
    }
    d = def(i->def);
    if (d == nullptr) {
      break;
    }
  }
  const Context& s = must(src);
  const Context& t = must(dst);
  if (s.def == t.def) {
    return iid;
  }
  const Mat4 m = mmul(t.inv, mmul(s.m, inst.m));
  s.def->insts.erase(iid);
  const Id nid = place(*t.def, inst.def, m);
  Inst& ni = *t.def->insts.get(nid);
  ni.name = inst.name;
  ni.color = inst.color;
  ni.hidden = inst.hidden;
  ni.tag = inst.tag;
  ni.rule = inst.rule;
  return nid;
}

// ---------------------------------------------------------------- guides, dimensions

Id Model::add_guide_line(const Vec3& p, const Vec3& d) {
  const Id id = new_guide_id();
  guides.set(id, Guide{.kind = "line", .p = p, .d = norm(d)});
  return id;
}

Id Model::add_guide_point(const Vec3& p) {
  const Id id = new_guide_id();
  guides.set(id, Guide{.kind = "point", .p = p, .d = {}});
  return id;
}

std::optional<DimEnd> Model::anchor_at(const Vec3& p, std::span<const Id> prefer) const {
  struct Best {
    std::size_t score;
    std::vector<Id> path;
    Id v;
  };
  std::optional<Best> best;
  walk([&](const Def& d, const Mat4& m, const std::vector<Id>& path, const std::string& /*c*/) {
    for (const auto& [v, q] : d.verts) {
      if (dist(m3d::apply(m, q), p) > kEps * 100) {
        continue;
      }
      const std::size_t score = (same_ids(path, prefer) ? 0 : 1000) + path.size();
      if (!best || score < best->score) {
        best = Best{score, path, v};
      }
    }
    return true;
  });
  if (!best) {
    return std::nullopt;
  }
  return DimEnd{.p = p, .path = best->path, .v = best->v};
}

namespace {

DimEnd end_of(const Model& m, const Vec3& p, std::span<const Id> prefer) {
  DimEnd e{.p = p, .path = std::nullopt, .v = 0};
  if (const auto a = m.anchor_at(p, prefer)) {
    e.path = a->path;
    e.v = a->v;
  }
  return e;
}

}  // namespace

Id Model::add_dim(const Vec3& a, const Vec3& b, const Vec3& off, std::span<const Id> prefer) {
  const Id id = new_guide_id();
  Dim dm;
  dm.a = end_of(*this, a, prefer);
  dm.b = end_of(*this, b, prefer);
  dm.off = off;
  dm.move = "b";
  dims.set(id, std::move(dm));
  return id;
}

Vec3 Model::end_at(DimEnd& e) {
  for (int guard = 0; guard < 64; ++guard) {  // the JS recurses once at most (the anchor it found is live)
    if (!e.path) {
      return e.p;
    }
    auto ctx = context(*e.path);
    const Vec3* q = ctx ? ctx->def->verts.get(e.v) : nullptr;
    if (q == nullptr && ctx) {
      if (const auto v = find_vert(*ctx->def, m3d::apply(ctx->inv, e.p), kEps * 100)) {
        e.v = *v;
        q = ctx->def->verts.get(*v);
      }
    }
    if (q != nullptr) {
      return m3d::apply(must(ctx).m, *q);
    }
    if (const auto a = anchor_at(e.p)) {
      e.path = a->path;
      e.v = a->v;
      continue;
    }
    e.path.reset();
    e.v = 0;
    return e.p;
  }
  return e.p;
}

base::Result<Id> Model::add_radial_dim(std::span<const Id> path, const std::string& cid, const Vec3& off,
                                       std::string_view kind) {
  DimCurve c;
  c.path = std::vector<Id>(path.begin(), path.end());
  c.curve = cid;
  const auto g = radial_at(c);
  if (!g || !g->live) {
    return base::fail(base::Errc::invalid, tr("m3d.model.dim_not_round"));
  }
  const Id id = new_guide_id();
  Dim dm;
  dm.kind = kind == "dia" ? "dia" : "rad";
  dm.c = c;
  dm.off = off;
  dims.set(id, std::move(dm));
  return id;
}

Id Model::add_angle_dim(const Vec3& o, const Vec3& a, const Vec3& b, const Vec3& off, std::span<const Id> prefer) {
  const Id id = new_guide_id();
  Dim dm;
  dm.kind = "ang";
  dm.o = end_of(*this, o, prefer);
  dm.a = end_of(*this, a, prefer);
  dm.b = end_of(*this, b, prefer);
  dm.off = off;
  dims.set(id, std::move(dm));
  return id;
}

std::optional<Model::Radial> Model::radial_at(DimCurve& c) {
  const auto at = [](const Def& d, const Mat4& m, const CurveInfo& ci) {
    const Vec3 C = m3d::apply(m, *ci.center);
    const double r = dist(C, m3d::apply(m, d.vert(ci.verts[0])));
    return Radial{C, r, norm(apply_dir(m, *ci.normal)), true, ci.flat, !ci.closed};
  };
  for (int guard = 0; guard < 64; ++guard) {
    if (!c.path) {
      break;
    }
    auto ctx = context(*c.path);
    const auto ci = ctx ? curve_info(*ctx->def, c.curve) : std::nullopt;
    if (ci && ci->round) {
      const Radial g = at(*ctx->def, ctx->m, *ci);
      c.p = g.c;
      c.r = g.r;
      c.n = g.n;
      return g;
    }
    // a curve moved into or out of a group is found again by its centre and radius (c.p is always set here)
    std::optional<std::pair<std::vector<Id>, std::string>> found;
    walk([&](const Def& d, const Mat4& m, const std::vector<Id>& path, const std::string& /*col*/) {
      if (found) {
        return true;
      }
      OrderedSet<std::string> cids;
      for (const auto& [eid, e] : d.edges) {
        if (!e.curve.empty()) {
          cids.add(e.curve);
        }
      }
      for (const std::string& cid : cids) {
        const auto ci2 = curve_info(d, cid);
        if (!ci2 || !ci2->round) {
          continue;
        }
        const Radial g = at(d, m, *ci2);
        if (dist(g.c, c.p) <= kEps * 100 && std::abs(g.r - c.r) <= kEps * 100) {
          found.emplace(path, cid);
          return true;
        }
      }
      return true;
    });
    if (found) {
      c.path = found->first;
      c.curve = found->second;
      continue;
    }
    c.path.reset();
    c.curve.clear();
    break;
  }
  return Radial{c.p, c.r, c.n, false, false, false};
}

base::Result<double> Model::set_dim_length(Id id, double L) {
  Dim* dm = dims.get(id);
  if (dm == nullptr) {
    return base::fail(base::Errc::invalid, tr("m3d.model.the_dimension_is_gone"));
  }
  if (dm->kind == "ang") {
    return base::fail(base::Errc::invalid, tr("m3d.model.dim_angle_only"));
  }
  if (dm->kind == "rad" || dm->kind == "dia") {
    const auto g = radial_at(dm->c);
    if (!g || !g->live) {
      return base::fail(base::Errc::invalid, tr("m3d.model.dim_curve_gone"));
    }
    const double want = dm->kind == "dia" ? L / 2 : L;
    if (!(want > kEps)) {
      return base::fail(base::Errc::invalid, tr("m3d.model.a_length_must_be_above_zero"));
    }
    auto ctx = context(dm->c.path.value_or(std::vector<Id>{}));
    const auto ci = ctx ? curve_info(*ctx->def, dm->c.curve) : std::nullopt;
    if (!ctx || !ci) {
      return base::fail(base::Errc::invalid, tr("m3d.model.dim_curve_gone"));
    }
    if (auto r = set_curve_radius(*ctx->def, dm->c.curve, ci->radius * want / g->r); !r) {
      return std::unexpected(r.error());
    }
    (void)radial_at(dims.get(id)->c);
    return want - g->r;
  }
  const Vec3 pa = end_at(dm->a);
  const Vec3 pb = end_at(dm->b);
  const DimEnd A = dm->a;
  const DimEnd B = dm->b;
  const std::string move = dm->move;
  auto r = resize_between(A.path ? &A : nullptr, B.path ? &B : nullptr, pa, pb, L, move);
  if (!r) {
    return r;
  }
  Dim& d2 = *dims.get(id);
  d2.a.p = end_at(d2.a);
  d2.b.p = end_at(d2.b);
  return r;
}

base::Result<double> Model::resize_between(const DimEnd* A, const DimEnd* B, const Vec3& pa, const Vec3& pb, double L,
                                           std::string_view move) {
  if (!(L > kEps)) {
    return base::fail(base::Errc::invalid, tr("m3d.model.a_length_must_be_above_zero"));
  }
  const double cur = dist(pa, pb);
  if (cur <= kEps) {
    return base::fail(base::Errc::invalid, tr("m3d.model.the_two_ends_are_at_the"));
  }
  const Vec3 dv = norm(sub(pb, pa));
  const double delta = L - cur;
  if (std::abs(delta) <= kEps) {
    return 0.0;
  }
  if (move == "both") {
    if (auto r = move_end(B, A, dv, delta / 2); !r) {
      return std::unexpected(r.error());
    }
    if (auto r = move_end(A, B, mul(dv, -1), delta / 2); !r) {
      return std::unexpected(r.error());
    }
  } else if (move == "a") {
    if (auto r = move_end(A, B, mul(dv, -1), delta); !r) {
      return std::unexpected(r.error());
    }
  } else if (auto r = move_end(B, A, dv, delta); !r) {
    return std::unexpected(r.error());
  }
  return delta;
}

base::Result<void> Model::move_end(const DimEnd* E, const DimEnd* F, const Vec3& dv, double delta) {
  if (E == nullptr || !E->path) {
    return base::fail(base::Errc::invalid, tr("m3d.model.the_end_that_moves_is_not"));
  }
  {
    auto c = context(*E->path);
    if (!c || !c->def->verts.has(E->v)) {
      return base::fail(base::Errc::invalid, tr("m3d.model.the_end_that_moves_is_not"));
    }
  }
  const std::vector<Id>& ep = *E->path;
  std::size_t k = 0;
  if (F != nullptr && F->path) {
    while (k < ep.size() && k < F->path->size() && ep[k] == (*F->path)[k]) {
      ++k;
    }
  }
  const std::vector<Id> P =
      F != nullptr ? std::vector<Id>(ep.begin(), ep.begin() + static_cast<std::ptrdiff_t>(k)) : ep;
  const Context ctx = must(context(P));  // a prefix of E's path, which is there
  const Vec3 local = apply_dir(ctx.inv, dv);
  const double s = len(local);
  const Vec3 dl = norm(local);
  if (ep.size() > P.size()) {
    Inst& inst = *ctx.def->insts.get(ep[P.size()]);
    inst.m = mmul(translation(mul(dl, delta * s)), inst.m);
    return {};
  }
  Def& d = *ctx.def;
  const OrderedSet<Id> piece = connected_verts(d, E->v);
  const bool fixed_here = F != nullptr && F->path && F->path->size() == P.size() && piece.has(F->v);
  const double cut = fixed_here ? dot(d.vert(E->v), dl) : -std::numeric_limits<double>::infinity();
  StretchOpts o;
  o.only = &piece;
  o.box_of = [this](const std::string& id) { return bbox(def(id)); };
  if (auto r = stretch(d, dl, cut, delta * s, o); !r) {
    return std::unexpected(r.error());
  }
  return {};
}

base::Result<double> Model::set_edge_length(std::span<const Id> path, Id eid, double L) {
  auto ctx = context(path);
  const Edge* e = ctx ? ctx->def->edges.get(eid) : nullptr;
  if (e == nullptr) {
    return base::fail(base::Errc::invalid, tr("m3d.model.the_edge_is_gone"));
  }
  const Context& cx = must(ctx);
  const Vec3 pa = m3d::apply(cx.m, cx.def->vert(e->a));
  const Vec3 pb = m3d::apply(cx.m, cx.def->vert(e->b));
  const Vec3 dv = sub(pb, pa);
  std::size_t i = 0;
  for (std::size_t j = 0; j < 3; ++j) {
    if (std::abs(dv.at(j)) > std::abs(dv.at(i)) + kEps) {
      i = j;
    }
  }
  const std::vector<Id> p(path.begin(), path.end());
  const bool fwd = dv.at(i) >= 0;
  const DimEnd A{.p = {}, .path = p, .v = fwd ? e->a : e->b};
  const DimEnd B{.p = {}, .path = p, .v = fwd ? e->b : e->a};
  return resize_between(&A, &B, fwd ? pa : pb, fwd ? pb : pa, L, "b");
}

base::Result<void> Model::set_def_size(const std::string& def_id, int axis, double size) {
  Def* d = def_mut(def_id);
  const auto bb = d != nullptr ? bbox(d) : std::nullopt;
  if (!bb) {
    return base::fail(base::Errc::invalid, tr("m3d.model.it_is_empty"));
  }
  if (!(size > kEps)) {
    return base::fail(base::Errc::invalid, tr("m3d.model.a_size_must_be_above_zero"));
  }
  const auto a = static_cast<std::size_t>(axis);
  const double cur = bb->max.at(a) - bb->min.at(a);
  if (cur <= kEps) {
    return base::fail(base::Errc::invalid, tr("m3d.model.it_is_flat_that_way_push"));
  }
  Vec3 dir{0, 0, 0};
  dir.at(a) = 1;
  StretchOpts o;
  o.box_of = [this](const std::string& id) { return bbox(def(id)); };
  if (auto r = stretch(*d, dir, bb->max.at(a), size - cur, o); !r) {
    return std::unexpected(r.error());
  }
  return {};
}

// ---------------------------------------------------------------- tags, sections, axes, lights, scenes

const TagDef* Model::tag(std::string_view name) const {
  const std::string k = base::js_to_lower(name);
  for (const TagDef& g : tags) {
    if (base::js_to_lower(g.name) == k) {
      return &g;
    }
  }
  return nullptr;
}

std::string Model::add_tag(std::string_view name) {
  std::string b = clean_text(name, 80);
  if (b.empty()) {
    b = tr("m3d.tag.default");
  }
  std::string nm = b;
  int k = 2;
  while (tag(nm) != nullptr) {
    nm = b + " " + std::to_string(k++);
  }
  tags.push_back({nm, false});
  return nm;
}

base::Result<std::string> Model::rename_tag(std::string_view old, std::string_view name) {
  const TagDef* g = tag(old);
  std::string nm = clean_text(name, 80);
  if (g == nullptr) {
    return base::fail(base::Errc::invalid, tr("m3d.tag.gone"));
  }
  if (nm.empty()) {
    return base::fail(base::Errc::invalid, tr("m3d.tag.empty"));
  }
  const TagDef* other = tag(nm);
  if (other != nullptr && other != g) {
    return base::fail(base::Errc::invalid, tr("m3d.tag.taken", {{"name", nm}}));
  }
  const std::string was = g->name;
  for (auto& [k, d] : defs_) {
    for (auto& [iid, i] : d->insts) {
      if (i.tag == was) {
        i.tag = nm;
      }
    }
  }
  const_cast<TagDef*>(g)->name = nm;  // NOLINT(cppcoreguidelines-pro-type-const-cast): it is in `tags`
  return nm;
}

int Model::delete_tag(std::string_view name) {
  const TagDef* g = tag(name);
  if (g == nullptr) {
    return 0;
  }
  const std::string was = g->name;
  int n = 0;
  for (auto& [k, d] : defs_) {
    for (auto& [iid, i] : d->insts) {
      if (i.tag == was) {
        i.tag.clear();
        ++n;
      }
    }
  }
  std::erase_if(tags, [&](const TagDef& t) { return &t == g; });
  return n;
}

void Model::set_tag_hidden(std::string_view name, bool hidden) {
  if (const TagDef* g = tag(name)) {
    const_cast<TagDef*>(g)->hidden = hidden;  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  }
}

base::Result<void> Model::set_tag(Def& d, std::span<const Id> iids, std::optional<std::string_view> name) const {
  const TagDef* g = name && !name->empty() ? tag(*name) : nullptr;
  if (name && !name->empty() && g == nullptr) {
    return base::fail(base::Errc::invalid, tr("m3d.tag.gone"));
  }
  for (const Id iid : iids) {
    if (Inst* i = d.insts.get(iid)) {
      i->tag = g != nullptr ? g->name : std::string();
    }
  }
  return {};
}

std::vector<std::pair<std::string, std::size_t>> Model::tag_counts() const {
  std::vector<std::pair<std::string, std::size_t>> out = {{"", 0}};
  for (const TagDef& g : tags) {
    out.emplace_back(g.name, 0);
  }
  for (const auto& [k, d] : defs_) {
    for (const auto& [iid, i] : d->insts) {
      auto it = std::ranges::find_if(out, [&](const auto& p) { return p.first == i.tag; });
      if (it == out.end()) {
        out.emplace_back(i.tag, 1);
      } else {
        ++it->second;
      }
    }
  }
  return out;
}

void Model::ensure_tags() {
  for (auto& [k, d] : defs_) {
    for (auto& [iid, i] : d->insts) {
      if (i.tag.empty()) {
        continue;
      }
      if (const TagDef* g = tag(i.tag)) {
        i.tag = g->name;
      } else if (tags.size() < 1000) {
        tags.push_back({i.tag, false});
      } else {
        i.tag.clear();
      }
    }
  }
}

Id Model::add_section(const Vec3& p, const Vec3& n, std::string_view name) {
  const Id id = new_guide_id();
  std::string nm = clean_text(name, 80);
  if (nm.empty()) {
    std::size_t k = sections.size() + 1;
    std::unordered_set<std::string> used;
    for (const auto& [sid, s] : sections) {
      used.insert(s.name);
    }
    // the JS loops until a free name; with no catalog every name is the key and it would never end (fuzz, 2026-10-22):
    // a free name is always found within one more than the planes there are
    while (used.contains(tr("m3d.section.name", {{"n", std::to_string(k)}})) && k <= sections.size() + 1) {
      ++k;
    }
    nm = tr("m3d.section.name", {{"n", std::to_string(k)}});
  }
  sections.set(id, Section{p, norm(n), nm});
  active_section = id;
  view_cuts = true;
  return id;
}

void Model::delete_section(Id id) {
  sections.erase(id);
  if (active_section == id) {
    active_section.reset();
  }
}

base::Result<void> Model::set_axes(const Vec3& o, const Vec3& x, const Vec3& y) {
  const Vec3 X = norm(x);
  const Vec3 Y0 = sub(y, mul(X, dot(y, X)));
  if (len(X) == 0 || len(Y0) < 1e-9) {
    return base::fail(base::Errc::invalid, tr("m3d.axes.bad"));
  }
  const Vec3 Y = norm(Y0);
  const Vec3 Z = cross(X, Y);
  const Mat4 m = {X[0], X[1], X[2], 0, Y[0], Y[1], Y[2], 0, Z[0], Z[1], Z[2], 0, o[0], o[1], o[2], 1};
  if (is_ident(m)) {
    axes_m.reset();
  } else {
    axes_m = m;
  }
  return {};
}

Mat4 Model::axes_frame(std::span<const Id> path) {
  if (!path.empty()) {
    auto c = context(path);
    if (!c) {
      c = context({});
    }
    return must(c).m;
  }
  return axes_m.value_or(ident());
}

base::Result<Id> Model::add_light(const Value& o) {
  const auto L = clean_light(o);
  if (!L) {
    return base::fail(base::Errc::invalid, tr("m3d.model.bad_light"));
  }
  const Id id = new_guide_id();
  lights.set(id, *L);
  return id;
}

base::Result<bool> Model::set_light(Id id, const Value& patch) {
  const Light* L = lights.get(id);
  if (L == nullptr) {
    return false;
  }
  Value merged = light_json(id, *L);
  merged.erase("id");
  if (patch.is_object()) {
    for (const auto& m : patch.as_object()) {
      merged.set(m.key, m.value);
    }
  }
  const auto n = clean_light(merged);
  if (!n) {
    return base::fail(base::Errc::invalid, tr("m3d.model.bad_light"));
  }
  lights.set(id, *n);
  return true;
}

Value Model::capture_vis() const {
  Array insts;
  // NOLINTNEXTLINE(misc-no-recursion): 32 levels at most, as model.js
  const std::function<void(const Def&, const std::vector<Id>&, int)> visit =
      [&](const Def& d, const std::vector<Id>& path, int depth) {
        if (depth > 32) {
          return;
        }
        for (const auto& [iid, inst] : d.insts) {
          std::vector<Id> p = path;
          p.push_back(iid);
          if (inst.hidden) {
            insts.push_back(ids_value(p));
          }
          if (const Def* c = def(inst.def)) {
            visit(*c, p, depth + 1);
          }
        }
      };
  visit(root(), {}, 0);
  Array hidden_tags;
  for (const TagDef& g : tags) {
    if (g.hidden) {
      hidden_tags.emplace_back(g.name);
    }
  }
  return base::make_object({{"insts", std::move(insts)},
                            {"tags", std::move(hidden_tags)},
                            {"section", active_section ? Value(*active_section) : Value(nullptr)},
                            {"cuts", view_cuts}});
}

void Model::apply_vis(const Value& vis) {
  if (!vis.truthy()) {
    return;
  }
  for (auto& [k, d] : defs_) {
    for (auto& [iid, i] : d->insts) {
      i.hidden = false;
    }
  }
  for (const Value& p : vis.get("insts").as_array()) {
    Def* d = &root();
    Inst* inst = nullptr;
    for (const Value& iid : p.as_array()) {
      inst = d != nullptr ? d->insts.get(to_id(iid.as_number())) : nullptr;
      if (inst == nullptr) {
        break;
      }
      d = def_mut(inst->def);
    }
    if (inst != nullptr) {
      inst->hidden = true;
    }
  }
  std::unordered_set<std::string> off;
  for (const Value& x : vis.get("tags").as_array()) {
    off.insert(base::js_to_lower(x.as_string()));
  }
  for (TagDef& g : tags) {
    g.hidden = off.contains(base::js_to_lower(g.name));
  }
  const Value sec = vis.get("section");
  active_section = !sec.is_null() && sec.is_number() && sections.has(to_id(sec.as_number()))
                       ? std::optional<Id>(to_id(sec.as_number()))
                       : std::nullopt;
  const Value cuts = vis.get("cuts");
  view_cuts = !cuts.is_bool() || cuts.as_bool();
  ++version;
  dirty = true;
}

// ---------------------------------------------------------------- undo

void Model::push_step(std::string label, Snap before, Snap after) {
  undo_.push_back(Step{std::move(label), std::move(before), std::move(after)});
  if (undo_.size() > kMaxUndo) {
    undo_.erase(undo_.begin());
  }
  redo_.clear();
  dirty = true;
}

std::optional<std::string> Model::undo() {
  if (undo_.empty()) {
    return std::nullopt;
  }
  Step s = std::move(undo_.back());
  undo_.pop_back();
  restore(s.b);
  std::string label = s.label;
  redo_.push_back(std::move(s));
  ++version;
  dirty = true;
  return label;
}

std::optional<std::string> Model::redo() {
  if (redo_.empty()) {
    return std::nullopt;
  }
  Step s = std::move(redo_.back());
  redo_.pop_back();
  restore(s.a);
  std::string label = s.label;
  undo_.push_back(std::move(s));
  ++version;
  dirty = true;
  return label;
}

std::vector<std::string> Model::undo_labels() const {
  std::vector<std::string> out;
  out.reserve(undo_.size());
  for (const Step& s : undo_) {
    out.push_back(s.label);
  }
  return out;
}

std::vector<std::string> Model::redo_labels() const {
  std::vector<std::string> out;
  out.reserve(redo_.size());
  for (const Step& s : redo_) {
    out.push_back(s.label);
  }
  return out;
}

void Model::begin() {
  preview_ = snap();
}

void Model::commit(std::string_view label) {
  if (preview_) {
    Snap before = std::move(*preview_);
    Snap after = snap();
    if (!(after == before)) {
      push_step(std::string(label), std::move(before), std::move(after));
    }
  }
  preview_.reset();
  ++version;
}

void Model::abort() {
  if (preview_) {
    restore(*preview_);
  }
  preview_.reset();
  ++version;
}

Snap Model::snap() {
  Snap s;
  s.head = base::to_json(to_json(true));
  for (const auto& [k, d] : defs_) {
    s.parts.emplace_back(k, base::to_json(def_to_json(*d)));
  }
  return s;
}

void Model::restore(const Snap& s) {
  std::vector<std::pair<std::string, std::unique_ptr<Def>>> made;
  for (const auto& [id, str] : s.parts) {
    const auto it = index_.find(id);
    if (it != index_.end() && base::to_json(def_to_json(*defs_[it->second].second)) == str) {
      // a copy: if the load below refused the snapshot, the model keeps every definition it had (fuzz, 2026-10-27)
      made.emplace_back(id, std::make_unique<Def>(*defs_[it->second].second));
      continue;
    }
    const auto j = base::parse_json(str);
    auto d = j ? def_of(*j) : base::Result<std::unique_ptr<Def>>(std::unexpected(j.error()));
    if (!d) {
      throw std::logic_error("m3d: an undo snapshot does not read back");
    }
    made.emplace_back(id, std::move(*d));
  }
  const auto head = base::parse_json(s.head);
  if (!head) {
    throw std::logic_error("m3d: an undo snapshot does not read back");
  }
  if (auto r = load_impl(*head, true, &made); !r) {
    throw std::logic_error("m3d: an undo snapshot does not load: " + r.error().message);
  }
}

// ---------------------------------------------------------------- files

Value Model::to_json(bool no_defs) {
  Value o = base::make_object(
      {{"format", std::string(kFormat)}, {"version", kVersion}, {"units", units}, {"nextId", Value(next_id)}});
  if (no_defs) {
    o.set("defs", 0);
  } else {
    Array defs;
    for (const auto& [k, d] : defs_) {
      defs.push_back(def_to_json(*d));
    }
    o.set("defs", std::move(defs));
  }
  Array gs;
  for (const auto& [id, g] : guides) {
    Value go = base::make_object({{"id", Value(id)}, {"kind", g.kind}, {"p", pt_value(g.p)}});
    if (g.kind == "line") {
      go.set("d", pt_value(g.d));
    }
    gs.push_back(std::move(go));
  }
  o.set("guides", std::move(gs));
  if (!lights.empty()) {
    Array ls;
    for (const auto& [id, L] : lights) {
      ls.push_back(light_json(id, L));
    }
    o.set("lights", std::move(ls));
  }
  if (!sections.empty()) {
    Array ss;
    for (const auto& [id, s] : sections) {
      ss.push_back(
          base::make_object({{"id", Value(id)}, {"p", pt_value(s.p)}, {"n", pt_value(s.n)}, {"name", s.name}}));
    }
    o.set("sections", std::move(ss));
    o.set("activeSection", active_section ? Value(*active_section) : Value(nullptr));
  }
  if (!(view_planes && view_cuts)) {
    o.set("sectionView", base::make_object({{"planes", view_planes}, {"cuts", view_cuts}}));
  }
  if (axes_m) {
    o.set("axes", mat_value(*axes_m));
  }
  if (!storeys.empty()) {
    Array st;
    for (const Storey& s : storeys) {
      st.push_back(base::make_object({{"name", s.name}, {"elevation", s.elevation}}));
    }
    o.set("storeys", std::move(st));
  }
  Array ds;
  for (const Id id : dims.keys()) {
    const auto end = [&](DimEnd& e) {
      const Vec3 p = end_at(e);
      e.p = p;
      Value v = base::make_object({{"p", pt_value(p)}});
      if (e.path) {
        v.set("path", ids_value(*e.path));
        v.set("v", Value(e.v));
      }
      return v;
    };
    Dim* dm = dims.get(id);
    if (dm->kind == "ang") {
      Value vo = end(dims.get(id)->o);
      Value va = end(dims.get(id)->a);
      Value vb = end(dims.get(id)->b);
      ds.push_back(base::make_object({{"id", Value(id)},
                                      {"kind", "ang"},
                                      {"o", std::move(vo)},
                                      {"a", std::move(va)},
                                      {"b", std::move(vb)},
                                      {"off", pt_value(dims.get(id)->off)}}));
    } else if (dm->kind == "rad" || dm->kind == "dia") {
      const auto g = radial_at(dims.get(id)->c);
      dm = dims.get(id);
      Value c = base::make_object({{"p", pt_value(g ? g->c : Vec3{0, 0, 0})},
                                   {"r", g ? g->r : 0.0},
                                   {"n", pt_value(g ? g->n : Vec3{0, 0, 1})}});
      if (dm->c.path) {
        c.set("path", ids_value(*dm->c.path));
        c.set("curve", dm->c.curve);
      }
      ds.push_back(
          base::make_object({{"id", Value(id)}, {"kind", dm->kind}, {"c", std::move(c)}, {"off", pt_value(dm->off)}}));
    } else {
      Value va = end(dims.get(id)->a);
      Value vb = end(dims.get(id)->b);
      dm = dims.get(id);
      ds.push_back(base::make_object({{"id", Value(id)},
                                      {"a", std::move(va)},
                                      {"b", std::move(vb)},
                                      {"off", pt_value(dm->off)},
                                      {"move", dm->move}}));
    }
  }
  o.set("dims", std::move(ds));
  o.set("scenes", scenes);
  if (!tags.empty()) {
    Array ts;
    for (const TagDef& g : tags) {
      ts.push_back(g.hidden ? base::make_object({{"name", g.name}, {"hidden", true}})
                            : base::make_object({{"name", g.name}}));
    }
    o.set("tags", std::move(ts));
  }
  return o;
}

Value Model::to_json() const {
  Model copy(*this);
  return copy.to_json(false);
}

base::Result<void> Model::load(const Value& j, bool keep_history) {
  return load_impl(j, keep_history, nullptr);
}

base::Result<void> Model::load_impl(const Value& j, bool keep_history,
                                    std::vector<std::pair<std::string, std::unique_ptr<Def>>>* made) {
  const auto fail = [](std::string_view why_key) {
    return base::fail(base::Errc::invalid, tr("m3d.model.file_bad", {{"why", tr(why_key)}}));
  };
  const auto fail_msg = [](const std::string& why) {
    return base::fail(base::Errc::invalid, tr("m3d.model.file_bad", {{"why", why}}));
  };
  if (!j.is_object() || !j.get("format").is_string() || j.get("format").as_string() != kFormat) {
    return fail("m3d.model.the_format_mark_is_missing");
  }
  const Value ver = j.get("version");
  if (!is_int(ver) || ver.as_number() < 1) {
    return fail("m3d.model.the_version_is_missing");
  }
  if (ver.as_number() > kVersion) {
    return base::fail(base::Errc::unsupported, tr("m3d.model.file_newer", {{"v", num_text(ver.as_number())}}));
  }
  const Value jdefs = j.get("defs");
  if (made == nullptr && (!jdefs.is_array() || jdefs.as_array().empty())) {
    return fail("m3d.model.it_has_no_model");
  }
  std::vector<std::pair<std::string, std::unique_ptr<Def>>> defs;
  std::unordered_map<std::string, std::size_t> dindex;
  if (made != nullptr) {
    for (auto& [id, d] : *made) {
      dindex.emplace(id, defs.size());
      defs.emplace_back(id, std::move(d));
    }
  } else {
    std::size_t k = 0;
    for (const Value& dj : jdefs.as_array()) {
      if (k++ >= 100000) {
        break;
      }
      const Value id = dj.get("id");
      const auto good_id = [](const std::string& s) {
        if (s == "root") {
          return true;
        }
        return s.size() >= 2 && s.size() <= 10 && s[0] == 'd' &&
               std::all_of(s.begin() + 1, s.end(), [](char c) { return c >= '0' && c <= '9'; });
      };
      if (!dj.is_object() || !id.is_string() || !good_id(id.as_string())) {
        return fail("m3d.model.a_definition_id_is_wrong");
      }
      if (dindex.contains(id.as_string())) {
        return fail("m3d.model.a_definition_appears_twice");
      }
      auto d = def_of(dj);
      if (!d) {
        return fail_msg(d.error().message);
      }
      dindex.emplace(id.as_string(), defs.size());
      defs.emplace_back(id.as_string(), std::move(*d));
    }
  }
  if (!dindex.contains("root")) {
    return fail("m3d.model.it_has_no_root");
  }
  for (const auto& [id, d] : defs) {
    for (const auto& [iid, inst] : d->insts) {
      if (!dindex.contains(inst.def)) {
        return fail("m3d.model.an_instance_points_at_a_missing");
      }
    }
  }
  // no definition may contain itself (iteratively: a file may nest a hundred thousand definitions)
  {
    std::unordered_map<std::string, int> state;
    for (const auto& [id0, d0] : defs) {
      if (state[id0] == 2) {
        continue;
      }
      struct Frame {
        std::string id;
        std::vector<std::string> kids;
        std::size_t next = 0;
      };
      std::vector<Frame> stack;
      const auto enter = [&](const std::string& id) -> bool {
        if (state[id] == 1) {
          return false;
        }
        if (state[id] == 2) {
          return true;
        }
        state[id] = 1;
        Frame f{id, {}, 0};
        for (const auto& [iid, inst] : defs[dindex.at(id)].second->insts) {
          f.kids.push_back(inst.def);
        }
        stack.push_back(std::move(f));
        return true;
      };
      if (!enter(id0)) {
        return fail("m3d.model.a_group_contains_itself");
      }
      while (!stack.empty()) {
        Frame& top = stack.back();
        if (top.next < top.kids.size()) {
          const std::string kid = top.kids[top.next++];
          if (!enter(kid)) {
            return fail("m3d.model.a_group_contains_itself");
          }
        } else {
          state[top.id] = 2;
          stack.pop_back();
        }
      }
    }
  }
  IdMap<Guide> gs;
  if (const Value jg = j.get("guides"); jg.is_array()) {
    std::size_t k = 0;
    for (const Value& g : jg.as_array()) {
      if (k++ >= 10000) {
        break;
      }
      if (!g.is_object() || !is_int(g.get("id")) || !is_pt(g.get("p"))) {
        continue;
      }
      const Id id = to_id(g.get("id").as_number());
      const Value kind = g.get("kind");
      if (kind.is_string() && kind.as_string() == "line" && is_pt(g.get("d")) && len(pt_of(g.get("d"))) > 1e-9) {
        gs.set(id, Guide{"line", pt_of(g.get("p")), norm(pt_of(g.get("d")))});
      } else if (kind.is_string() && kind.as_string() == "point") {
        gs.set(id, Guide{"point", pt_of(g.get("p")), {}});
      }
    }
  }
  IdMap<Dim> ds;
  const auto dim_end = [](const Value& e) -> std::optional<DimEnd> {
    if (!e.is_object() || !is_pt(e.get("p"))) {
      return std::nullopt;
    }
    DimEnd out{.p = pt_of(e.get("p")), .path = std::nullopt, .v = 0};
    const Value path = e.get("path");
    if (path.is_array() && path.as_array().size() <= 32 && std::ranges::all_of(path.as_array(), is_int) &&
        is_int(e.get("v"))) {
      std::vector<Id> p;
      for (const Value& x : path.as_array()) {
        p.push_back(to_id(x.as_number()));
      }
      out.path = std::move(p);
      out.v = to_id(e.get("v").as_number());
    }
    return out;
  };
  if (const Value jd = j.get("dims"); jd.is_array()) {
    std::size_t k = 0;
    for (const Value& dm : jd.as_array()) {
      if (k++ >= 10000) {
        break;
      }
      if (!dm.is_object() || !is_int(dm.get("id")) || gs.has(to_id(dm.get("id").as_number())) ||
          !is_pt(dm.get("off"))) {
        continue;
      }
      const Id id = to_id(dm.get("id").as_number());
      const Value kind = dm.get("kind");
      const std::string kd = kind.is_string() ? kind.as_string() : std::string();
      if (kd == "ang") {
        const auto o = dim_end(dm.get("o"));
        const auto a = dim_end(dm.get("a"));
        const auto b = dim_end(dm.get("b"));
        if (o && a && b) {
          Dim d;
          d.kind = "ang";
          d.o = *o;
          d.a = *a;
          d.b = *b;
          d.off = pt_of(dm.get("off"));
          ds.set(id, std::move(d));
        }
        continue;
      }
      if (kd == "rad" || kd == "dia") {
        const Value c = dm.get("c");
        const Value r = c.get("r");
        if (!c.is_object() || !is_pt(c.get("p")) || !is_pt(c.get("n")) || !r.is_number() ||
            !std::isfinite(r.as_number()) || r.as_number() < 0) {
          continue;
        }
        DimCurve out{.p = pt_of(c.get("p")), .r = r.as_number(), .n = pt_of(c.get("n")), .path = {}, .curve = {}};
        const Value path = c.get("path");
        const Value curve = c.get("curve");
        if (path.is_array() && path.as_array().size() <= 32 && std::ranges::all_of(path.as_array(), is_int) &&
            curve.is_string() && is_curve_id(curve.as_string())) {
          std::vector<Id> p;
          for (const Value& x : path.as_array()) {
            p.push_back(to_id(x.as_number()));
          }
          out.path = std::move(p);
          out.curve = curve.as_string();
        }
        Dim d;
        d.kind = kd;
        d.c = std::move(out);
        d.off = pt_of(dm.get("off"));
        ds.set(id, std::move(d));
        continue;
      }
      const auto a = dim_end(dm.get("a"));
      const auto b = dim_end(dm.get("b"));
      if (a && b) {
        Dim d;
        d.a = *a;
        d.b = *b;
        d.off = pt_of(dm.get("off"));
        const Value mv = dm.get("move");
        d.move = mv.is_string() && (mv.as_string() == "a" || mv.as_string() == "b" || mv.as_string() == "both")
                     ? mv.as_string()
                     : "b";
        ds.set(id, std::move(d));
      }
    }
  }
  Array sc;
  if (const Value js = j.get("scenes"); js.is_array()) {
    std::size_t k = 0;
    for (const Value& s : js.as_array()) {
      if (k++ >= 500) {
        break;
      }
      Value c = clean_scene(s);
      if (!c.is_null()) {
        sc.push_back(std::move(c));
      }
    }
  }
  double max_id = 0;
  for (const auto& [id, d] : defs) {
    if (id != "root") {
      max_id = std::max(max_id, base::js_number(std::string_view(id).substr(1)));
    }
  }
  for (const auto& [id, g] : gs) {
    max_id = std::max(max_id, static_cast<double>(id));
  }
  for (const auto& [id, d] : ds) {
    max_id = std::max(max_id, static_cast<double>(id));
  }
  IdMap<Light> ls;
  if (const Value jl = j.get("lights"); jl.is_array()) {
    std::size_t k = 0;
    for (const Value& o : jl.as_array()) {
      if (k++ >= 1000) {
        break;
      }
      if (!o.is_object() || !is_int(o.get("id")) || o.get("id").as_number() < 1) {
        continue;
      }
      const Id id = to_id(o.get("id").as_number());
      if (gs.has(id) || ds.has(id) || ls.has(id)) {
        continue;
      }
      if (const auto L = clean_light(o)) {
        ls.set(id, *L);
        max_id = std::max(max_id, static_cast<double>(id));
      }
    }
  }
  IdMap<Section> ss;
  if (const Value jsec = j.get("sections"); jsec.is_array()) {
    std::size_t k = 0;
    for (const Value& s : jsec.as_array()) {
      if (k++ >= 1000) {
        break;
      }
      if (!s.is_object() || !is_int(s.get("id"))) {
        continue;
      }
      const Id id = to_id(s.get("id").as_number());
      if (gs.has(id) || ds.has(id) || ls.has(id) || ss.has(id) || !is_pt(s.get("p")) || !is_pt(s.get("n")) ||
          !(len(pt_of(s.get("n"))) > 1e-9)) {
        continue;
      }
      std::string nm = clean_text(s.get("name"), 80);
      if (nm.empty()) {
        nm = tr("m3d.section.name", {{"n", std::to_string(ss.size() + 1)}});
      }
      ss.set(id, Section{pt_of(s.get("p")), norm(pt_of(s.get("n"))), nm});
      max_id = std::max(max_id, static_cast<double>(id));
    }
  }
  // every check passed: the model takes the file
  defs_ = std::move(defs);
  reindex();
  const Value act = j.get("activeSection");
  active_section = act.is_number() && is_int(act) && ss.has(to_id(act.as_number()))
                       ? std::optional<Id>(to_id(act.as_number()))
                       : std::nullopt;
  sections = std::move(ss);
  const Value sv = j.get("sectionView");
  const auto flag_not_false = [](const Value& v) { return !v.is_bool() || v.as_bool(); };
  view_planes = !sv.is_object() || flag_not_false(sv.get("planes"));
  view_cuts = !sv.is_object() || flag_not_false(sv.get("cuts"));
  storeys = clean_storeys(j.get("storeys"));
  axes_m.reset();
  if (const Value a = j.get("axes");
      a.is_array() && a.as_array().size() == 16 && std::ranges::all_of(a.as_array(), is_num)) {
    Mat4 m{};
    for (std::size_t i = 0; i < 16; ++i) {
      m.at(i) = a.as_array()[i].as_number();
    }
    const Vec3 X{m[0], m[1], m[2]};
    const Vec3 Y{m[4], m[5], m[6]};
    const Vec3 Z{m[8], m[9], m[10]};
    if (std::abs(len(X) - 1) < 1e-6 && std::abs(len(Y) - 1) < 1e-6 && std::abs(len(Z) - 1) < 1e-6 &&
        std::abs(dot(X, Y)) < 1e-6 && dot(cross(X, Y), Z) > 0.999) {
      axes_m = m;
    }
  }
  guides = std::move(gs);
  lights = std::move(ls);
  dims = std::move(ds);
  scenes = std::move(sc);
  tags.clear();
  if (const Value jt = j.get("tags"); jt.is_array()) {
    std::size_t k = 0;
    for (const Value& g : jt.as_array()) {
      if (k++ >= 1000) {
        break;
      }
      const std::string nm = clean_text(g.get("name"), 80);
      if (!nm.empty() && tag(nm) == nullptr) {
        const Value h = g.get("hidden");
        tags.push_back({nm, h.is_bool() && h.as_bool()});
      }
    }
  }
  ensure_tags();
  const Value ju = j.get("units");
  units = "ft-in";
  if (ju.is_string()) {
    for (const char* u : {"ft-in", "in", "m", "cm", "mm"}) {
      if (ju.as_string() == u) {
        units = u;
      }
    }
  }
  const Value jn = j.get("nextId");
  next_id = to_id(std::max(is_int(jn) ? jn.as_number() : 1, max_id + 1));
  if (!keep_history) {
    undo_.clear();
    redo_.clear();
    dirty = false;
    preview_.reset();
    uid = new_uid();
  }
  ++version;
  return {};
}

// ---------------------------------------------------------------- images

namespace {

/// /^[A-Za-z0-9+/]+=*$/
bool is_base64(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size() && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
                          (s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '/')) {
    ++i;
  }
  if (i == 0) {
    return false;
  }
  while (i < s.size() && s[i] == '=') {
    ++i;
  }
  return i == s.size();
}

}  // namespace

base::Result<std::string> Model::add_image(std::string_view name, std::string_view mime, std::string_view data,
                                           std::string_view avg) {
  const bool mime_ok = mime == "image/png" || mime == "image/jpeg" || mime == "image/webp";
  if (!mime_ok || !is_base64(data) || static_cast<double>(data.size()) > 24e6) {
    return base::fail(base::Errc::invalid, tr("m3d.model.bad_image"));
  }
  for (const auto& [id, im] : images) {
    if (im.data == data) {
      return id;
    }
  }
  std::uint32_t h = 0;
  for (std::size_t i = 0; i < data.size(); i += 97) {
    h = h * 31U + static_cast<unsigned char>(data[i]);
  }
  std::string id = "t";
  {
    std::string digits;
    std::uint32_t x = h;
    do {
      digits +=
          "0123456789abcdefghijklmnopqrstuvwxyz"[x % 36U];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      x /= 36U;
    } while (x != 0);
    std::ranges::reverse(digits);
    id += digits;
  }
  while (images.has(id)) {
    id += 'x';
  }
  images.set(id, Image{clean_text(name, 120), std::string(mime), std::string(data),
                       is_color(avg) ? base::js_to_lower(avg) : std::string()});
  return id;
}

Value Model::images_json() const {
  std::unordered_set<std::string> used;
  for (const auto& [k, d] : defs_) {
    for (const auto& [fid, f] : d->faces) {
      if (is_image_paint(f.color)) {
        used.insert(image_id(f.color));
      }
    }
    for (const auto& [iid, i] : d->insts) {
      if (is_image_paint(i.color)) {
        used.insert(image_id(i.color));
      }
    }
  }
  Array out;
  for (const auto& [id, im] : images) {
    if (used.contains(id)) {
      out.push_back(base::make_object({{"id", id},
                                       {"name", im.name},
                                       {"mime", im.mime},
                                       {"data", im.data},
                                       {"avg", im.avg.empty() ? Value(nullptr) : Value(im.avg)}}));
    }
  }
  return out;
}

void Model::set_images(const Value& list) {
  images.clear();
  if (!list.is_array()) {
    return;
  }
  double total = 0;
  std::size_t k = 0;
  for (const Value& im : list.as_array()) {
    if (k++ >= 256) {
      break;
    }
    const Value id = im.get("id");
    const Value mime = im.get("mime");
    const Value data = im.get("data");
    const auto id_ok = [](const std::string& s) {
      return !s.empty() && s.size() <= 24 &&
             std::ranges::all_of(s, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
    };
    if (!im.is_object() || !id.is_string() || !id_ok(id.as_string()) || !mime.is_string() ||
        (mime.as_string() != "image/png" && mime.as_string() != "image/jpeg" && mime.as_string() != "image/webp") ||
        !data.is_string() || !is_base64(data.as_string())) {
      continue;
    }
    total += static_cast<double>(data.as_string().size());
    if (total > 96e6) {
      break;
    }
    const Value avg = im.get("avg");
    images.set(id.as_string(), Image{clean_text(im.get("name"), 120), mime.as_string(), data.as_string(),
                                     avg.is_string() && is_color(avg.as_string()) ? base::js_to_lower(avg.as_string())
                                                                                  : std::string()});
  }
}

}  // namespace cc::m3d
