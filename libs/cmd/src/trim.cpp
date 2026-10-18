// SPDX-License-Identifier: Apache-2.0
// commands.js's OFFSET, TRIM and EXTEND (CadCraft 0.5, 0.4 part 2 T.1–T.3) in C++: Quick mode (every object round it
// is a cutting edge or boundary) or Standard (the edges chosen), a pick, a fence (F, or two clicks on nothing), a
// crossing window (C), EDGEMODE (E), erase (R), undo (U). Constraints kept through a trim (ckeep.js) are not ported:
// the fork keeps none yet (docs/gaps.md).
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/cad/edit.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

std::string type_of(const Value& e) {
  return base::js_string(e.get("type"));
}
bool unbounded(const std::string& t) {
  return t == "xline" || t == "ray";
}
bool trimmable(const std::string& t) {
  return t == "line" || t == "arc" || t == "circle" || t == "poly" || t == "xline" || t == "ray";
}
cad::P2 p2(Pt p) {
  return {p.x, p.y};
}
cad::BlockLookup lookup_of(const Ctx& c) {
  return [&c](const std::string& n) { return c.doc.blocks().get(n); };
}
/// doc.all(): the open space's entities, in order
std::vector<const Value*> all(const Ctx& c) {
  std::vector<const Value*> out;
  c.doc.entities().each([&](const Id&, const Value& e) { out.push_back(&e); });
  return out;
}
bool edge_ok(const Ctx& c, const Value& o) {
  const std::string t = type_of(o);
  return c.doc.visible(o) && t != "text" && t != "point";
}

/// commands.js near(c, e, pad): what could cut or bound e (bounds overlapping e's, padded 1 %)
std::vector<const Value*> near(const Ctx& c, const Value& e) {
  if (unbounded(type_of(e))) {  // a construction line reaches everything (DR.6)
    std::vector<const Value*> out;
    for (const Value* o : all(c)) {
      if (!(o->get("id") == e.get("id")) && edge_ok(c, *o)) {
        out.push_back(o);
      }
    }
    return out;
  }
  const cad::Bounds b = cad::entity_bounds(e, lookup_of(c));
  if (!b.valid()) {
    return {};
  }
  const double p = std::max(b.max_x - b.min_x, b.max_y - b.min_y) * 0.01;
  std::vector<const Value*> out;
  for (const Value* o : all(c)) {
    if (!edge_ok(c, *o)) {
      continue;
    }
    if (unbounded(type_of(*o))) {
      out.push_back(o);
      continue;
    }
    const cad::Bounds ob = cad::entity_bounds(*o, lookup_of(c));
    if (ob.max_x >= b.min_x - p && ob.min_x <= b.max_x + p && ob.max_y >= b.min_y - p && ob.min_y <= b.max_y + p) {
      out.push_back(o);
    }
  }
  return out;
}

struct TrimState {
  bool standard = false;  // trimMode 'standard' (else 'quick')
  int edge_mode = 0;      // EDGEMODE
};
TrimState trim_state(const Ctx& c) {
  TrimState s;
  s.standard = c.engine.state.get("trimMode") == Value("standard");
  s.edge_mode = base::js_to_int32(base::js_to_number(c.engine.state.get("edgeMode")));
  return s;
}
void save_state(Ctx& c, const TrimState& s) {
  c.engine.state.set("trimMode", s.standard ? "standard" : "quick");
  c.engine.state.set("edgeMode", s.edge_mode);
}

/// commands.js trimEdgesFor(c, edges, e, all)
std::vector<const Value*> edges_for(const Ctx& c, const std::optional<IdSet>& edges, const Value& e, bool every) {
  const Value eid = e.get("id");
  if (edges) {
    std::vector<const Value*> out;
    for (const Id id : *edges) {
      const Value* o = c.doc.entities().get(id);
      if (o != nullptr && !(o->get("id") == eid) && edge_ok(c, *o)) {
        out.push_back(o);
      }
    }
    return out;
  }
  if (every) {
    std::vector<const Value*> out;
    for (const Value* o : all(c)) {
      if (!(o->get("id") == eid) && edge_ok(c, *o)) {
        out.push_back(o);
      }
    }
    return out;
  }
  return near(c, e);
}

struct How {
  std::optional<std::pair<Id, Pt>> pick;
  std::vector<Pt> path;
  std::optional<Rect> window;
};

/// commands.js trimCandidates(c, how)
std::vector<const Value*> candidates(Ctx& c, const How& how) {
  if (how.window) {
    std::vector<const Value*> out;
    for (const Id id : c.host.pick_window(*how.window, true)) {
      if (const Value* e = c.doc.entities().get(id)) {
        out.push_back(e);
      }
    }
    return out;
  }
  cad::Bounds b;
  for (const Pt p : how.path) {
    b.min_x = std::min(b.min_x, p.x);
    b.min_y = std::min(b.min_y, p.y);
    b.max_x = std::max(b.max_x, p.x);
    b.max_y = std::max(b.max_y, p.y);
  }
  std::vector<const Value*> out;
  for (const Value* e : all(c)) {
    const std::string t = type_of(*e);
    if (!c.doc.editable(*e) || !trimmable(t)) {
      continue;
    }
    if (unbounded(t)) {  // crossings decide (DR.6)
      out.push_back(e);
      continue;
    }
    const cad::Bounds eb = cad::entity_bounds(*e, lookup_of(c));
    if (eb.valid() && eb.max_x >= b.min_x && eb.min_x <= b.max_x && eb.max_y >= b.min_y && eb.min_y <= b.max_y) {
      out.push_back(e);
    }
  }
  return out;
}

struct TrimItem {
  Id id = 0;
  cad::TrimSplit split;
  std::set<std::size_t> gone;
};
struct TrimPlan {
  bool refused = false;
  std::vector<TrimItem> list;
};

std::vector<cad::P2> p2s(const std::vector<Pt>& v) {
  std::vector<cad::P2> out;
  out.reserve(v.size());
  for (const Pt p : v) {
    out.push_back(p2(p));
  }
  return out;
}

/// commands.js trimPlan(c, s, edges, how)
TrimPlan trim_plan(Ctx& c, const TrimState& s, const std::optional<IdSet>& edges, const How& how) {
  const bool edge_extend = s.edge_mode == 1;
  const bool quick = !s.standard;
  const auto usable = [quick](const std::optional<cad::TrimSplit>& sp) {
    return sp && (quick || sp->pieces.size() > 1 || (sp->closed && !sp->pieces[0].whole));
  };
  TrimPlan plan;
  if (how.pick) {
    const Value* e = c.doc.entities().get(how.pick->first);
    if (e == nullptr || !trimmable(type_of(*e)) || !c.doc.editable(*e)) {
      plan.refused = true;
      return plan;
    }
    auto sp = cad::trim_split(*e, edges_for(c, edges, *e, edge_extend), lookup_of(c), edge_extend);
    if (sp && usable(sp)) {
      TrimItem it{.id = id_of(*e), .split = std::move(*sp), .gone = {}};
      it.gone.insert(cad::piece_at(it.split, p2(how.pick->second)));
      plan.list.push_back(std::move(it));
    }
    return plan;
  }
  for (const Value* e : candidates(c, how)) {
    if (!trimmable(type_of(*e))) {
      continue;
    }
    std::optional<std::vector<cad::P2>> hits;
    if (!how.path.empty()) {
      hits = cad::path_crossings(*e, p2s(how.path), lookup_of(c));
      if (hits->empty()) {
        continue;
      }
    }
    auto sp = cad::trim_split(*e, edges_for(c, edges, *e, edge_extend), lookup_of(c), edge_extend);
    if (!sp || !usable(sp)) {
      continue;
    }
    TrimItem it{.id = id_of(*e), .split = std::move(*sp), .gone = {}};
    if (hits) {
      for (const cad::P2 x : *hits) {
        it.gone.insert(cad::piece_at(it.split, x));
      }
    } else if (how.window) {
      const cad::Rect2 w{.min_x = how.window->min_x,
                         .min_y = how.window->min_y,
                         .max_x = how.window->max_x,
                         .max_y = how.window->max_y};
      for (std::size_t i = 0; i < it.split.pieces.size(); ++i) {
        const Value pc = cad::piece_entity(it.split, i);
        if (pc.truthy() && cad::in_window(pc, w, true, lookup_of(c))) {
          it.gone.insert(i);
        }
      }
    }
    if (!it.gone.empty()) {
      plan.list.push_back(std::move(it));
    }
  }
  return plan;
}

struct TrimDone {
  int n = 0, erased = 0;
};
/// commands.js applyTrim(c, list): one undo step
TrimDone apply_trim(Ctx& c, const std::vector<TrimItem>& list) {
  TrimDone r;
  if (list.empty()) {
    return r;
  }
  c.doc.change("TRIM", [&] {
    for (const TrimItem& it : list) {
      if (c.doc.entities().get(it.id) == nullptr) {
        continue;
      }
      const auto out = cad::trim_result(it.split, it.gone);
      if (!out) {
        continue;
      }
      ++r.n;
      // the first piece keeps the object's id (so its constraints could stay with it)
      for (std::size_t i = 0; i < out->size(); ++i) {
        Value p = (*out)[i];
        p.erase("id");
        if (i == 0) {
          p.set("id", static_cast<double>(it.id));
          (void)c.doc.update(it.id, p);
        } else {
          (void)c.doc.add(p);
        }
      }
      if (out->empty()) {
        c.doc.remove(it.id);
        ++r.erased;
      }
    }
  });
  return r;
}

struct ExtendItem {
  Id id = 0;
  Value nu;
};
/// commands.js extendPlan(c, s, edges, how)
std::vector<ExtendItem> extend_plan(Ctx& c, const TrimState& s, const std::optional<IdSet>& edges, const How& how) {
  const bool edge_extend = s.edge_mode == 1;
  const auto one = [&](const Value* e, Pt at) -> std::optional<ExtendItem> {
    if (e == nullptr || !c.doc.editable(*e)) {
      return std::nullopt;
    }
    const std::string t = type_of(*e);
    if (t != "line" && t != "arc" && t != "poly") {
      return std::nullopt;
    }
    auto nu = cad::extend_entity(*e, edges_for(c, edges, *e, true), p2(at), lookup_of(c), edge_extend);
    if (!nu) {
      return std::nullopt;
    }
    return ExtendItem{.id = id_of(*e), .nu = std::move(*nu)};
  };
  std::vector<ExtendItem> out;
  if (how.pick) {
    if (auto r = one(c.doc.entities().get(how.pick->first), how.pick->second)) {
      out.push_back(std::move(*r));
    }
    return out;
  }
  for (const Value* e : candidates(c, how)) {
    Pt at;
    if (!how.path.empty()) {
      const auto hits = cad::path_crossings(*e, p2s(how.path), lookup_of(c));
      if (hits.empty()) {
        continue;
      }
      at = {hits[0].x, hits[0].y};
    } else if (how.window) {
      at = {(how.window->min_x + how.window->max_x) / 2, (how.window->min_y + how.window->max_y) / 2};
    }
    if (auto r = one(e, at)) {
      out.push_back(std::move(*r));
    }
  }
  return out;
}

std::size_t apply_extend(Ctx& c, const std::vector<ExtendItem>& list) {
  if (!list.empty()) {
    c.doc.change("EXTEND", [&] {
      for (const ExtendItem& it : list) {
        if (c.doc.entities().get(it.id) != nullptr) {
          (void)c.doc.update(it.id, it.nu);
        }
      }
    });
  }
  return list.size();
}

void undo_inside(Ctx& c) {
  if (c.doc.mark() > c.engine.cmd_mark) {
    (void)c.doc.undo();
    c.doc.clear_redo();
  } else {
    c.print(c.t("cmd.extend.nothing_undo_command"));
  }
}

/// The pieces a trim would remove, or the parts an extend would add, greyed (preview entities with `ghost`).
base::Array ghosts(Ctx& c, const TrimState& s, const std::optional<IdSet>& edges, Pt cur, bool extend,
                   const std::vector<Pt>& path) {
  How how;
  if (path.size() > 1) {
    how.path = path;
  } else {
    const auto id = c.host.pick_at(cur);
    if (!id || *id == 0) {
      return {};
    }
    how.pick = std::pair{*id, cur};
  }
  base::Array out;
  const auto ghost = [&out](Value e) {
    if (e.truthy()) {
      e.set("ghost", true);
      out.push_back(std::move(e));
    }
  };
  if (extend) {
    for (const ExtendItem& it : extend_plan(c, s, edges, how)) {
      if (const Value* e = c.doc.entities().get(it.id)) {
        ghost(cad::extension_part(*e, it.nu));
      }
    }
    return out;
  }
  for (const TrimItem& it : trim_plan(c, s, edges, how).list) {
    for (const std::size_t i : it.gone) {
      ghost(cad::piece_entity(it.split, i));
    }
  }
  return out;
}

/// trimOrExtend's chooseEdges: the cutting edges (or boundaries) to use; none chosen = every object
Task<void> choose_edges(Ctx& c, bool is_trim, std::optional<IdSet>& edges) {
  const Answer ids = co_await c.select(c.t(is_trim ? "cmd.trim.edges" : "cmd.extend.edges"));
  if (ids.kind == Answer::Kind::set && !ids.set.empty()) {
    edges = ids.set;
  } else {
    edges.reset();
  }
  c.host.set_selection({});
  c.print(edges ? c.words.tn("cmd.trim.edges.n", static_cast<double>(edges->size())) : c.t("cmd.trim.edges.all"));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) — the JS's trimOrExtend
Task<void> trim_or_extend(Ctx& c, bool is_trim) {
  TrimState s = trim_state(c);
  save_state(c, s);
  std::optional<IdSet> edges;  // Standard mode: the chosen cutting edges / boundaries (none = every object)
  const auto settings_line = [&] {
    c.print(c.t("cmd.trim.settings", {{"mode", c.t(s.standard ? "opt.standard" : "opt.quick")},
                                      {"edge", c.t(s.edge_mode == 1 ? "opt.extend" : "opt.noextend")}}));
  };
  settings_line();
  if (s.standard) {
    co_await choose_edges(c, is_trim, edges);
  } else {
    c.host.set_selection({});
  }
  const auto run = [&](const How& how, bool do_trim) {
    if (do_trim) {
      const TrimPlan plan = trim_plan(c, s, edges, how);
      if (plan.refused) {
        c.print(c.t("cmd.trim.cannot"));
        return;
      }
      if (plan.list.empty()) {
        c.print(c.t(how.pick ? "cmd.trim.nothing" : "cmd.trim.nothing.path"));
        return;
      }
      const TrimDone r = apply_trim(c, plan.list);
      if (!how.pick) {
        c.print(c.words.tn("cmd.trim.done", static_cast<double>(r.n)));
      }
      const bool one_piece =
          std::ranges::any_of(plan.list, [](const TrimItem& it) { return it.split.pieces.size() == 1; });
      if (r.erased != 0 && !s.standard && one_piece) {
        c.print(c.words.tn("cmd.trim.erased", static_cast<double>(r.erased)));
      }
    } else {
      const std::vector<ExtendItem> list = extend_plan(c, s, edges, how);
      if (list.empty()) {
        c.print(c.t(how.pick ? "cmd.extend.nothing" : "cmd.extend.nothing.path"));
        return;
      }
      const std::size_t n = apply_extend(c, list);
      if (!how.pick) {
        c.print(c.words.tn("cmd.extend.done", static_cast<double>(n)));
      }
    }
  };
  std::vector<Option> options;
  if (is_trim) {
    options = {c.opt("T", c.t("opt.cuttingedges")), c.opt("F", c.t("sel.opt.fence")),
               c.opt("C", c.t("sel.opt.crossing")), c.opt("O", c.t("opt.mode")),
               c.opt("E", c.t("opt.edge")),         c.opt("R", c.t("opt.erase")),
               c.opt("U", c.t("opt.undo"))};
  } else {
    options = {c.opt("B", c.t("opt.boundaryedges")), c.opt("F", c.t("sel.opt.fence")),
               c.opt("C", c.t("sel.opt.crossing")),  c.opt("O", c.t("opt.mode")),
               c.opt("E", c.t("opt.edge")),          c.opt("U", c.t("opt.undo"))};
  }
  const auto flips = [is_trim](bool shift) { return is_trim != shift; };  // true = this one trims
  for (;;) {
    c.preview([&](Pt cur) { return ghosts(c, s, edges, cur, !flips(c.host.shift()), {}); });
    Ask a;
    a.none = true;
    a.path = true;
    a.empty = true;
    a.options = options;
    const Answer r = co_await c.entity(c.t(is_trim ? "cmd.trim.pick" : "cmd.extend.pick"), std::move(a));
    if (!r.truthy()) {
      co_return;
    }
    if (r.kind == Answer::Kind::option) {
      if (r.option == "U") {
        undo_inside(c);
      } else if (r.option == "T" || r.option == "B") {
        s.standard = true;
        save_state(c, s);
        c.preview(nullptr);
        co_await choose_edges(c, is_trim, edges);
      } else if (r.option == "O") {
        c.preview(nullptr);
        Ask k;
        k.options = {c.opt("Q", c.t("opt.quick")), c.opt("S", c.t("opt.standard"))};
        k.def = s.standard ? "S" : "Q";
        const Answer ans = co_await c.keyword(c.t("cmd.trim.mode.ask"), std::move(k));
        const std::string v = base::js_to_upper(ans.key());
        if (v == "S") {
          s.standard = true;
          save_state(c, s);
          co_await choose_edges(c, is_trim, edges);
        } else if (v == "Q") {
          s.standard = false;
          save_state(c, s);
          edges.reset();
        }
        settings_line();
      } else if (r.option == "E") {
        c.preview(nullptr);
        Ask k;
        k.options = {c.opt("E", c.t("opt.extend")), c.opt("N", c.t("opt.noextend"))};
        k.def = s.edge_mode == 1 ? "E" : "N";
        const Answer ans = co_await c.keyword(c.t("cmd.trim.edge.ask"), std::move(k));
        const std::string v = base::js_to_upper(ans.key());
        if (v == "E") {
          s.edge_mode = 1;
        } else if (v == "N") {
          s.edge_mode = 0;
        }
        save_state(c, s);
        settings_line();
      } else if (r.option == "R") {
        c.preview(nullptr);
        Ask sel;
        sel.fresh = true;
        const Answer ids = co_await c.select(c.t("cmd.trim.erase.select"), std::move(sel));
        std::vector<Id> del;
        for (const Id id : ids.set) {
          const Value* e = c.doc.entities().get(id);
          if (e != nullptr && c.doc.editable(*e)) {
            del.push_back(id);
          }
        }
        if (!del.empty()) {
          c.doc.change("ERASE", [&] {
            for (const Id id : del) {
              c.doc.remove(id);
            }
          });
        }
        c.host.set_selection({});
      } else if (r.option == "F") {
        std::vector<Pt> pts;
        for (;;) {
          c.preview([&](Pt cur) {
            base::Array out;
            if (!pts.empty()) {
              base::Array line;
              for (const Pt p : pts) {
                line.push_back(vtx(p.x, p.y));
              }
              line.push_back(vtx(cur.x, cur.y));
              out.push_back(base::make_object({{"type", "poly"}, {"closed", false}, {"pts", std::move(line)}}));
              std::vector<Pt> path = pts;
              path.push_back(cur);
              for (Value& g : ghosts(c, s, edges, cur, !is_trim, path)) {
                out.push_back(std::move(g));
              }
            }
            return out;
          });
          Ask fa;
          if (!pts.empty()) {
            fa.base = pts.back();
            fa.options = {c.opt("U", c.t("opt.undo"))};
          }
          fa.none = true;
          const Answer p =
              co_await c.point(pts.empty() ? c.t("sel.fence.first") : c.t("sel.fence.next"), std::move(fa));
          if (!p.truthy()) {
            break;
          }
          if (p.is("U")) {
            if (!pts.empty()) {
              pts.pop_back();
            }
            continue;
          }
          pts.push_back(p.p);
        }
        if (pts.size() >= 2) {
          How how;
          how.path = pts;
          run(how, is_trim);
        }
      } else if (r.option == "C") {
        c.preview(nullptr);
        const Pt a0 = (co_await c.point(c.t("sel.window.first"))).p;
        c.preview([a0](Pt cur) { return base::Array{rect(a0, cur)}; });
        Ask wb;
        wb.base = a0;
        const Pt b0 = (co_await c.point(c.t("sel.window.second"), std::move(wb))).p;
        How how;
        how.window = Rect{.min_x = std::min(a0.x, b0.x),
                          .min_y = std::min(a0.y, b0.y),
                          .max_x = std::max(a0.x, b0.x),
                          .max_y = std::max(a0.y, b0.y)};
        run(how, is_trim);
      }
      continue;
    }
    if (r.kind == Answer::Kind::path) {
      How how;
      how.path = r.path;
      run(how, flips(r.shift));
      continue;
    }
    if (r.kind == Answer::Kind::empty) {
      // two clicks on empty spots: a fence between them
      const Pt a0 = r.p;
      c.preview([&, a0](Pt cur) {
        base::Array out{line_e(a0.x, a0.y, cur.x, cur.y)};
        for (Value& g : ghosts(c, s, edges, cur, !flips(c.host.shift()), {a0, cur})) {
          out.push_back(std::move(g));
        }
        return out;
      });
      Ask fb;
      fb.base = a0;
      fb.none = true;
      const Answer b = co_await c.point(c.t("cmd.trim.fence.second"), std::move(fb));
      if (b.kind == Answer::Kind::point) {
        How how;
        how.path = {a0, b.p};
        run(how, flips(c.host.shift()));
      }
      continue;
    }
    How how;
    how.pick = std::pair{r.id, r.p};
    run(how, flips(r.shift));
  }
}

Task<void> trim_cmd(Ctx& c) {
  co_await trim_or_extend(c, true);
}
Task<void> extend_cmd(Ctx& c) {
  co_await trim_or_extend(c, false);
}

/// commands.js closestT(p, q)
double closest_t(const cad::Prim& p, Pt q) {
  if (!p.arc) {
    const double dx = p.x2 - p.x1;
    const double dy = p.y2 - p.y1;
    const double L = dx * dx + dy * dy;
    return L != 0 ? std::max(0.0, std::min(1.0, ((q.x - p.x1) * dx + (q.y - p.y1) * dy) / L)) : 0;
  }
  const double a = norm_angle(base::js_atan2(q.y - p.cy, q.x - p.cx) - p.a0);
  const double s = p.a1 - p.a0;
  if (a <= s) {
    return a / s;
  }
  return a - s < kTau - a ? 1 : 0;
}
Pt prim_point(const cad::Prim& p, double t) {
  if (!p.arc) {
    return {p.x1 + (p.x2 - p.x1) * t, p.y1 + (p.y2 - p.y1) * t};
  }
  const double a = p.a0 + (p.a1 - p.a0) * t;
  return {p.cx + p.r * base::js_cos(a), p.cy + p.r * base::js_sin(a)};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the JS's OFFSET
Task<void> offset_cmd(Ctx& c) {
  Ask ad;
  ad.options = {c.opt("T", "Through")};
  const Value last = c.engine.state.get("lastOffset");
  ad.def = last.is_undefined() || last.is_null() ? Value("Through") : last;
  const Answer d = co_await c.distance(c.t("cmd.offset.specify_offset_distance"), std::move(ad));
  const bool through = (d.kind == Answer::Kind::text && d.text == "Through") || d.is("T");
  if (!through) {
    if (d.kind != Answer::Kind::number || !(d.number > 0)) {
      co_return;
    }
    c.engine.state.set("lastOffset", d.number);
  } else {
    c.engine.state.set("lastOffset", "Through");
  }
  const double dd = d.number;
  c.host.set_selection({});
  for (;;) {
    Ask ap;
    ap.none = true;
    const Answer pick = co_await c.entity(c.t("cmd.offset.select_object_offset"), std::move(ap));
    if (!pick.truthy()) {
      co_return;
    }
    const Value* ep = c.doc.entities().get(pick.id);
    const std::string t = ep != nullptr ? type_of(*ep) : std::string();
    if (t != "line" && t != "circle" && t != "arc" && t != "poly") {
      c.print(c.t("cmd.offset.cannot_offset_object"));
      continue;
    }
    const Value e = *ep;
    IdSet one;
    one.add(pick.id);
    c.host.set_selection(one);
    const std::vector<cad::Prim> ps = cad::entity_prims(e, lookup_of(c));
    const auto dist_for = [&ps, through, dd](Pt p) {
      if (!through) {
        return dd;
      }
      double m = std::numeric_limits<double>::infinity();
      for (const cad::Prim& q : ps) {
        m = std::min(m, dist(p, prim_point(q, closest_t(q, p))));
      }
      return m;
    };
    c.preview([e, dist_for](Pt cur) {
      const auto o = cad::offset_entity(e, dist_for(cur), {cur.x, cur.y});
      return o ? base::Array{*o} : base::Array{};
    });
    Ask as;
    as.none = true;
    const Answer side = co_await c.point(
        through ? c.t("cmd.offset.specify_through_point") : c.t("cmd.offset.specify_point_side_offset"), std::move(as));
    c.preview(nullptr);
    c.host.set_selection({});
    if (!side.truthy() || side.kind != Answer::Kind::point) {
      co_return;
    }
    auto o = cad::offset_entity(e, dist_for(side.p), p2(side.p));
    if (!o) {
      c.print(c.t("cmd.offset.cannot_offset_far"));
      continue;
    }
    o->erase("id");
    c.doc.change("OFFSET", [&] { (void)c.doc.add(*o); });
  }
}

}  // namespace

void add_trim(std::vector<CommandDef>& t) {
  t.push_back(def("OFFSET", {"O"}, offset_cmd, "cmd.offset.parallel_copies_distance"));
  t.push_back(def("TRIM", {"TR"}, trim_cmd, "cmd.trim.help"));
  t.push_back(def("EXTEND", {"EX"}, extend_cmd, "cmd.extend.help"));
}

}  // namespace cc::cmd::detail
