// SPDX-License-Identifier: Apache-2.0
// commands.js (CadCraft 0.5) in C++, M5.1a: ELLIPSE, POINT, DUPLICATE, SELECTALL, DESELECT, STRETCH, BREAK,
// BREAKATPOINT, LENGTHEN, JOIN, EXPLODE.
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cad/dims.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/geom.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

cad::P2 p2(Pt p) {
  return {p.x, p.y};
}
std::string type_of(const Value& e) {
  return base::js_string(e.get("type"));
}
cad::BlockLookup blocks_of(const Ctx& c) {
  return [&c](const std::string& n) { return c.doc.blocks().get(n); };
}
cad::Mat2 m_translate(double x, double y) {
  return {1, 0, 0, 1, x, y};
}
/// commands.js fmt(n): '0' under 1e-10, else Number(n.toFixed(4)).toString()
std::string fmt(double n) {
  if (std::abs(n) < 1e-10) {
    return "0";
  }
  std::string s;
  base::append_number(s, base::js_number(base::js_to_fixed(n, 4)));
  return s;
}
/// commands.js fmtLen(c, n): a length in the drawing's format
std::string fmt_len(const Ctx& c, double n) {
  const Value lu = c.doc.meta.get("lunits");
  return cad::format_length(n, lu.is_undefined() || lu.is_null() ? Value(2) : lu, c.doc.meta.get("luprec"));
}
/// normAngle(x) || TAU
double sweep_of(double a0, double a1) {
  const double s = norm_angle(a1 - a0);
  return s != 0 && !std::isnan(s) ? s : kTau;
}
/// The ids of a selection's objects as they are now.
std::vector<Value> current(const Ctx& c, const IdSet& ids) {
  std::vector<Value> out;
  for (const Id id : ids) {
    if (const Value* e = c.doc.entities().get(id)) {
      out.push_back(*e);
    }
  }
  return out;
}

// ---- ELLIPSE, POINT

Task<void> ellipse_cmd(Ctx& c) {
  Ask aa;
  aa.options = {c.opt("C", "Center")};
  const Answer a = co_await c.point(c.t("cmd.ellipse.specify_axis_endpoint_ellipse"), std::move(aa));
  Pt center;
  Pt major;
  if (a.is("C")) {
    center = (co_await c.point(c.t("cmd.ellipse.specify_center_ellipse"))).p;
    Ask ae;
    ae.base = center;
    const Pt e = (co_await c.point(c.t("cmd.ellipse.specify_endpoint_axis"), std::move(ae))).p;
    major = {e.x - center.x, e.y - center.y};
  } else {
    Ask ab;
    ab.base = a.p;
    const Pt b = (co_await c.point(c.t("cmd.ellipse.specify_other_endpoint_axis"), std::move(ab))).p;
    center = {(a.p.x + b.x) / 2, (a.p.y + b.y) / 2};
    major = {b.x - center.x, b.y - center.y};
  }
  const double l = base::js_hypot(major.x, major.y);
  if (l < 1e-12) {
    co_return;
  }
  const auto make = [center, major, l](double half) {
    return base::make_object({{"type", "ellipse"},
                              {"cx", center.x},
                              {"cy", center.y},
                              {"mx", major.x},
                              {"my", major.y},
                              {"ratio", std::min(std::max(half / l, 1e-6), 1.0)},
                              {"t0", 0},
                              {"t1", kTau}});
  };
  c.preview([make, center, major, l](Pt cur) {
    return base::Array{make(std::abs(((cur.x - center.x) * -major.y + (cur.y - center.y) * major.x) / l))};
  });
  Ask ad;
  ad.base = center;
  const Answer d = co_await c.distance(c.t("cmd.ellipse.specify_distance_other_axis"), std::move(ad));
  if (!d.truthy()) {
    co_return;
  }
  if (d.number > l) {  // the other axis is the longer one: swap so the ratio stays ≤ 1
    const Pt u{-major.y / l * d.number, major.x / l * d.number};
    (void)c.add(c.ent(base::make_object({{"type", "ellipse"},
                                         {"cx", center.x},
                                         {"cy", center.y},
                                         {"mx", u.x},
                                         {"my", u.y},
                                         {"ratio", l / d.number},
                                         {"t0", 0},
                                         {"t1", kTau}})));
  } else {
    (void)c.add(c.ent(make(d.number)));
  }
}

Task<void> point_cmd(Ctx& c) {
  for (;;) {
    Ask a;
    a.none = true;
    const Answer p = co_await c.point(c.t("cmd.point.specify_point"), std::move(a));
    if (!p.truthy()) {
      co_return;
    }
    (void)c.add(c.ent(base::make_object({{"type", "point"}, {"x", p.p.x}, {"y", p.p.y}})));
  }
}

// ---- DUPLICATE, SELECTALL, DESELECT

Task<void> duplicate_cmd(Ctx& c) {
  const Answer ids = co_await c.select(c.t("cmd.duplicate.select_objects_duplicate"));
  if (ids.kind != Answer::Kind::set || ids.set.empty()) {
    co_return;
  }
  const std::vector<Value> src = current(c, ids.set);
  // the copy starts on top of the original and moves with the pointer from where it is now
  std::optional<Pt> at = c.host.cursor();
  Pt start;
  if (at && std::isfinite(at->x) && std::isfinite(at->y)) {
    start = *at;
  } else {
    cad::Bounds b;
    for (const Value& e : src) {
      const cad::Bounds eb = cad::entity_bounds(e, blocks_of(c));
      if (eb.valid()) {
        b = {std::min(b.min_x, eb.min_x), std::min(b.min_y, eb.min_y), std::max(b.max_x, eb.max_x),
             std::max(b.max_y, eb.max_y)};
      }
    }
    start = b.valid() ? Pt{(b.min_x + b.max_x) / 2, (b.min_y + b.max_y) / 2} : Pt{0, 0};
  }
  c.engine.last_point = start;
  c.preview([&src, start](Pt cur) {
    base::Array out;
    for (const Value& e : src) {
      out.push_back(cad::tx_entity(e, m_translate(cur.x - start.x, cur.y - start.y)));
    }
    return out;
  });
  Ask ap;
  ap.base = start;
  const Answer p = co_await c.point(c.t("cmd.duplicate.specify_where_copy_goes"), std::move(ap));
  c.preview(nullptr);
  const cad::Mat2 m = m_translate(p.p.x - start.x, p.p.y - start.y);
  IdSet made;
  c.doc.change("DUPLICATE", [&] {
    for (const Value& e : src) {
      const Value* now = c.doc.entities().get(id_of(e));
      Value t = cad::tx_entity(now != nullptr ? *now : e, m);
      t.erase("id");
      made.add(id_of(c.doc.add(t)));
    }
  });
  const std::size_t n = made.size();
  c.host.set_selection(std::move(made));
  c.print(c.t("cmd.duplicate.done", {{"objects", c.words.tn("cmd.n.objects", static_cast<double>(n))}}));
}

Task<void> selectall_cmd(Ctx& c) {
  IdSet ids;
  c.doc.entities().each([&](const Id& id, const Value& e) {
    if (c.doc.editable(e)) {
      ids.add(id);
    }
  });
  const std::size_t n = ids.size();
  c.host.set_selection(std::move(ids));
  c.print(c.words.tn("cmd.selectall.done", static_cast<double>(n)));
  co_return;
}

Task<void> deselect_cmd(Ctx& c) {
  c.host.set_selection({});
  co_return;
}

// ---- STRETCH

Task<void> stretch_cmd(Ctx& c) {
  c.print(c.t("cmd.stretch.hint"));
  const Answer ids = co_await c.select(c.t("cmd.stretch.select"));
  if (ids.kind != Answer::Kind::set || ids.set.empty()) {
    co_return;
  }
  const std::vector<Rect> wins = ids.set.windows;
  // the frame outlives every use (the preview is cleared before the command ends; an Esc clears it too)
  const std::function<bool(cad::P2)> inside = [&wins](cad::P2 p) {
    return std::ranges::any_of(wins, [p](const Rect& w) {
      return p.x >= w.min_x - 1e-9 && p.x <= w.max_x + 1e-9 && p.y >= w.min_y - 1e-9 && p.y <= w.max_y + 1e-9;
    });
  };
  if (wins.empty()) {
    c.print(c.t("cmd.stretch.nowindow"));
  }
  std::vector<Value> src;
  for (const Value& e : current(c, ids.set)) {
    if (type_of(e) != "viewport") {
      src.push_back(e);
    }
  }
  const auto moved = [&wins, &inside](const Value& e, double dx, double dy) -> std::optional<Value> {
    if (!wins.empty()) {
      return cad::stretch_entity(e, inside, dx, dy);
    }
    return cad::tx_entity(e, m_translate(dx, dy));
  };
  Ask ab;
  ab.options = {c.opt("D", c.t("opt.displacement"))};
  const Answer b = co_await c.point(c.t("cmd.stretch.base"), std::move(ab));
  Pt d;
  if (b.is("D")) {
    d = (co_await c.point(c.t("cmd.stretch.disp"))).p;
  } else {
    const Pt base = b.p;
    c.preview([&src, moved, base](Pt cur) {
      base::Array out;
      for (const Value& e : src) {
        if (auto x = moved(e, cur.x - base.x, cur.y - base.y)) {
          out.push_back(std::move(*x));
        }
      }
      return out;
    });
    Ask a2;
    a2.base = base;
    const Pt p = (co_await c.point(c.t("cmd.stretch.second"), std::move(a2))).p;
    d = {p.x - base.x, p.y - base.y};
  }
  c.preview(nullptr);
  std::size_t n = 0;
  c.doc.change("STRETCH", [&] {
    for (const Value& e : src) {
      const Value* cur = c.doc.entities().get(id_of(e));
      if (cur == nullptr) {
        continue;
      }
      if (auto nx = moved(*cur, d.x, d.y)) {
        (void)c.doc.update(id_of(e), *nx);
        ++n;
      }
    }
  });
  c.print(c.words.tn("cmd.stretch.done", static_cast<double>(n)));
}

// ---- BREAK and LENGTHEN

/// commands.js closestT(p, q): geom.primParam without the arc's rev flag
double closest_t(const cad::Prim& p, cad::P2 q) {
  if (!p.arc) {
    const double dx = p.x2 - p.x1;
    const double dy = p.y2 - p.y1;
    const double l = dx * dx + dy * dy;
    return l != 0 ? std::max(0.0, std::min(1.0, ((q.x - p.x1) * dx + (q.y - p.y1) * dy) / l)) : 0;
  }
  const double a = norm_angle(base::js_atan2(q.y - p.cy, q.x - p.cx) - p.a0);
  const double s = p.a1 - p.a0;
  if (a <= s) {
    return a / s;
  }
  return a - s < kTau - a ? 1 : 0;
}
cad::P2 prim_at(const cad::Prim& p, double t) {
  if (!p.arc) {
    return {p.x1 + (p.x2 - p.x1) * t, p.y1 + (p.y2 - p.y1) * t};
  }
  const double a = p.a0 + (p.a1 - p.a0) * t;
  return {p.cx + p.r * base::js_cos(a), p.cy + p.r * base::js_sin(a)};
}
/// commands.js onEntity(e, q): the point on e nearest q
cad::P2 on_entity(const Value& e, cad::P2 q) {
  std::optional<cad::P2> best;
  double bd = std::numeric_limits<double>::infinity();
  for (const cad::Prim& p : cad::entity_prims(e, nullptr)) {
    const cad::P2 x = prim_at(p, closest_t(p, q));
    const double d = base::js_hypot(q.x - x.x, q.y - x.y);
    if (d < bd) {
      bd = d;
      best = x;
    }
  }
  return best ? *best : q;
}

bool breakable(const std::string& t) {
  return t == "line" || t == "arc" || t == "circle" || t == "poly" || t == "xline" || t == "ray";
}

Task<void> break_like(Ctx& c, bool at_point) {
  for (;;) {
    const Answer pick = co_await c.entity(c.t("cmd.break.select"));
    const Value* ep = c.doc.entities().get(pick.id);
    if (ep == nullptr) {
      co_return;
    }
    const Value e = *ep;
    if (!c.doc.editable(e)) {
      c.print(c.t("cmd.corner.err.locked"));
      continue;
    }
    if (!breakable(type_of(e))) {
      c.print(c.t("cmd.break.type"));
      continue;
    }
    IdSet sel;
    sel.add(pick.id);
    c.host.set_selection(std::move(sel));
    cad::P2 p1 = on_entity(e, p2(pick.p));
    cad::P2 q2;
    if (at_point) {
      const Pt p = (co_await c.point(c.t("cmd.break.at"))).p;
      p1 = on_entity(e, p2(p));
      q2 = p1;
    } else {
      Ask aq;
      aq.options = {c.opt("F", c.t("opt.firstpoint"))};
      const Answer q = co_await c.point(c.t("cmd.break.second"), std::move(aq));
      if (q.is("F")) {
        p1 = on_entity(e, p2((co_await c.point(c.t("cmd.break.first"))).p));
        q2 = on_entity(e, p2((co_await c.point(c.t("cmd.break.second"))).p));
      } else {
        q2 = on_entity(e, p2(q.p));
      }
    }
    const cad::BreakResult r = cad::break_entity(e, p1, q2);
    if (!r.error.empty()) {
      c.print(c.t(r.error == "circle" ? "cmd.break.circle" : "cmd.break.type"));
      co_return;
    }
    c.doc.change("BREAK", [&] {
      c.doc.remove(id_of(e));
      for (const Value& piece : r.pieces) {
        Value x = piece;
        x.erase("id");
        (void)c.doc.add(x);
      }
    });
    c.print(c.words.tn("cmd.break.done", static_cast<double>(r.pieces.size())));
    co_return;
  }
}
Task<void> break_cmd(Ctx& c) {
  co_await break_like(c, false);
}
Task<void> breakat_cmd(Ctx& c) {
  co_await break_like(c, true);
}

/// The engine's LENGTHEN settings (E.lengthen).
struct LenState {
  std::string mode = "T";
  double delta = 0;
  std::optional<double> d_angle;
  double percent = 100;
  double total = 1;
  std::optional<double> t_angle;
};
LenState len_state(const Ctx& c) {
  LenState s;
  const Value v = c.engine.state.get("lengthen");
  if (!v.is_object()) {
    return s;
  }
  const auto opt_num = [&v](std::string_view k) -> std::optional<double> {
    const Value x = v.get(k);
    return x.is_number() ? std::optional<double>(x.as_number()) : std::nullopt;
  };
  s.mode = base::js_string(v.get("mode"));
  s.delta = base::js_to_number(v.get("delta"));
  s.d_angle = opt_num("dAngle");
  s.percent = base::js_to_number(v.get("percent"));
  s.total = base::js_to_number(v.get("total"));
  s.t_angle = opt_num("tAngle");
  return s;
}
void save_len_state(Ctx& c, const LenState& s) {
  c.engine.state.set("lengthen", base::make_object({{"mode", s.mode},
                                                    {"delta", s.delta},
                                                    {"dAngle", s.d_angle ? Value(*s.d_angle) : Value(nullptr)},
                                                    {"percent", s.percent},
                                                    {"total", s.total},
                                                    {"tAngle", s.t_angle ? Value(*s.t_angle) : Value(nullptr)}}));
}

/// LENGTHEN's change for one object in the current mode (cur: DYnamic's pointer)
cad::LengthenResult len_change(const LenState& s, const Value& e, cad::P2 pick, std::optional<cad::P2> cur) {
  const std::optional<double> l = cad::curve_length(e);
  const std::string t = type_of(e);
  if (!l || (t == "poly" && e.get("closed").truthy())) {
    return {.error = "type", .e = {}};
  }
  if (s.mode == "DE") {
    return s.d_angle && t == "arc" ? cad::lengthen_entity(e, pick, 0, s.d_angle)
                                   : cad::lengthen_entity(e, pick, s.delta);
  }
  if (s.mode == "P") {
    return cad::lengthen_entity(e, pick, *l * (s.percent / 100 - 1));
  }
  if (s.mode == "T") {
    if (s.t_angle && t == "arc") {
      const double sw = sweep_of(base::js_to_number(e.get("a0")), base::js_to_number(e.get("a1")));
      return cad::lengthen_entity(e, pick, 0, *s.t_angle - sw);
    }
    return cad::lengthen_entity(e, pick, s.total - *l);
  }
  // DY: the end follows the pointer
  if (!cur) {
    return {.error = "type", .e = {}};
  }
  const auto num = [&e](std::string_view k) { return base::js_to_number(e.get(k)); };
  const auto dist2 = [](cad::P2 a, cad::P2 b) { return base::js_hypot(b.x - a.x, b.y - a.y); };
  if (t == "arc") {
    const Pt c0{num("cx"), num("cy")};
    const double r = num("r");
    const Pt s0 = polar(c0, num("a0"), r);
    const Pt s1 = polar(c0, num("a1"), r);
    const bool at_start = dist2(pick, {s0.x, s0.y}) < dist2(pick, {s1.x, s1.y});
    const double a = norm_angle(angle_of(c0, {cur->x, cur->y}));
    Value out = e;
    out.set(at_start ? "a0" : "a1", a);
    return {.error = {}, .e = std::move(out)};
  }
  cad::P2 fixed;
  cad::P2 moving;
  if (t == "line") {
    const cad::P2 s0{num("x1"), num("y1")};
    const cad::P2 s1{num("x2"), num("y2")};
    const bool at_start = dist2(pick, s0) < dist2(pick, s1);
    fixed = at_start ? s1 : s0;
    moving = at_start ? s0 : s1;
  } else {
    const Value pts = e.get("pts");
    const std::size_t n = pts.as_array().size();
    const auto at = [&pts](std::size_t i) {
      const Value& p = pts.as_array()[i];
      return cad::P2{base::js_to_number(p.get("x")), base::js_to_number(p.get("y"))};
    };
    const bool at_start = dist2(pick, at(0)) < dist2(pick, at(n - 1));
    const Value bv = pts.as_array()[at_start ? 0 : n - 2].get("b");
    if (std::abs(bv.truthy() ? base::js_to_number(bv) : 0) > 1e-12) {
      return {.error = "type", .e = {}};
    }
    fixed = at_start ? at(1) : at(n - 2);
    moving = at_start ? at(0) : at(n - 1);
  }
  const double ls = dist2(fixed, moving);
  const double along =
      ((cur->x - fixed.x) * (moving.x - fixed.x) + (cur->y - fixed.y) * (moving.y - fixed.y)) / (ls != 0 ? ls : 1);
  return cad::lengthen_entity(e, pick, along - ls);
}

Task<void> set_len_mode(Ctx& c, LenState& s, const std::string& m) {
  if (m == "DE") {
    Ask a;
    a.def = s.delta;
    a.length = true;
    a.options = {c.opt("A", c.t("opt.angle"))};
    const Answer v = co_await c.number(c.t("cmd.len.delta"), std::move(a));
    if (v.is("A")) {
      Ask aa;
      aa.def = deg(s.d_angle && *s.d_angle != 0 && !std::isnan(*s.d_angle) ? *s.d_angle : 0);  // dAngle || 0
      const Answer an = co_await c.number(c.t("cmd.len.dangle"), std::move(aa));
      s.d_angle = rad(an.number);
    } else if (v.kind == Answer::Kind::number) {
      s.delta = v.number;
      s.d_angle.reset();
    }
  }
  if (m == "P") {
    Ask a;
    a.def = s.percent;
    const Answer v = co_await c.number(c.t("cmd.len.percent"), std::move(a));
    if (v.kind == Answer::Kind::number && v.number > 0) {
      s.percent = v.number;
    } else {
      c.print(c.t("cmd.len.percent.bad"));
    }
  }
  if (m == "T") {
    Ask a;
    a.def = s.total;
    a.length = true;
    a.options = {c.opt("A", c.t("opt.angle"))};
    const Answer v = co_await c.number(c.t("cmd.len.total"), std::move(a));
    if (v.is("A")) {
      Ask aa;
      aa.def = deg(s.t_angle && *s.t_angle != 0 && !std::isnan(*s.t_angle) ? *s.t_angle : kPi / 2);
      const Answer an = co_await c.number(c.t("cmd.len.tangle"), std::move(aa));
      if (an.kind == Answer::Kind::number && an.number > 0 && an.number < 360) {
        s.t_angle = rad(an.number);
      }
    } else if (v.kind == Answer::Kind::number && v.number > 0) {
      s.total = v.number;
      s.t_angle.reset();
    }
  }
  s.mode = m;
  save_len_state(c, s);
}

Task<void> lengthen_cmd(Ctx& c) {
  LenState s = len_state(c);
  save_len_state(c, s);
  const std::vector<Option> mode_opts = {c.opt("DE", c.t("opt.delta")), c.opt("P", c.t("opt.percent")),
                                         c.opt("T", c.t("opt.total")), c.opt("DY", c.t("opt.dynamic"))};
  // as in AutoCAD: picking here only measures; an option (or Enter: the last one) leads on to changing objects
  for (;;) {
    Ask a;
    a.options = mode_opts;
    a.none = true;
    const Answer pick = co_await c.entity(c.t("cmd.len.measure"), std::move(a));
    if (!pick.truthy()) {
      break;
    }
    if (pick.kind == Answer::Kind::option) {
      co_await set_len_mode(c, s, pick.option);
      break;
    }
    const Value* e = c.doc.entities().get(pick.id);
    const std::optional<double> l = e != nullptr ? cad::curve_length(*e) : std::nullopt;
    if (!l) {
      c.print(c.t("cmd.len.type"));
      continue;
    }
    if (type_of(*e) == "arc") {
      const double sw = sweep_of(base::js_to_number(e->get("a0")), base::js_to_number(e->get("a1")));
      c.print(c.t("cmd.len.current.arc", {{"l", fmt_len(c, *l)}, {"a", fmt(deg(sw))}}));
    } else {
      c.print(c.t("cmd.len.current", {{"l", fmt_len(c, *l)}}));
    }
  }
  for (;;) {
    if (s.mode == "DY") {
      c.preview(nullptr);
    }
    Ask a;
    a.none = true;
    a.options = {c.opt("U", c.t("opt.undo"))};
    const Answer pick = co_await c.entity(c.t("cmd.len.change"), std::move(a));
    if (!pick.truthy()) {
      co_return;
    }
    if (pick.is("U")) {
      undo_inside(c);
      continue;
    }
    const Value* ep = c.doc.entities().get(pick.id);
    if (ep == nullptr) {
      continue;
    }
    const Value e = *ep;
    if (!c.doc.editable(e)) {
      c.print(c.t("cmd.corner.err.locked"));
      continue;
    }
    cad::LengthenResult r;
    if (s.mode == "DY") {
      const cad::P2 at = p2(pick.p);
      c.preview([s, e, at](Pt cur) {
        cad::LengthenResult x = len_change(s, e, at, p2(cur));
        return x.error.empty() ? base::Array{std::move(x.e)} : base::Array{};
      });
      const Pt to = (co_await c.point(c.t("cmd.len.dynamic"))).p;
      c.preview(nullptr);
      r = len_change(s, e, at, p2(to));
    } else {
      r = len_change(s, e, p2(pick.p), std::nullopt);
    }
    if (!r.error.empty()) {
      c.print(c.t("cmd.len.err." + r.error));
      continue;
    }
    c.doc.change("LENGTHEN", [&] { (void)c.doc.update(id_of(e), r.e); });
  }
}

// ---- JOIN, EXPLODE

/// An id from a stored entity's id value (ids the document handed out).
Id id_from(const Value& v) {
  const double d = base::js_to_number(v);
  return std::isfinite(d) && d >= 0 && d < 9007199254740992.0 ? static_cast<Id>(d) : 0;
}

Task<void> join_cmd(Ctx& c) {
  const Answer ids = co_await c.select(c.t("cmd.join.select"));
  if (ids.kind != Answer::Kind::set || ids.set.size() < 2) {
    if (ids.kind == Answer::Kind::set && !ids.set.empty()) {
      c.print(c.t("cmd.join.two"));
    }
    co_return;
  }
  const std::vector<Value> ents = current(c, ids.set);
  const cad::JoinResult r = cad::join_entities(ents);
  if (r.results.empty()) {
    c.print(c.t("cmd.join.none"));
    co_return;
  }
  c.doc.change("JOIN", [&] {
    for (const cad::JoinPart& part : r.results) {
      for (const Value& id : part.from) {
        c.doc.remove(id_from(id));
      }
      Value x = part.to;
      x.erase("id");
      (void)c.doc.add(x);
    }
  });
  std::size_t n = 0;
  for (const cad::JoinPart& part : r.results) {
    n += part.from.size();
  }
  c.print(c.words.tn("cmd.join.done", static_cast<double>(n), {{"m", std::to_string(r.results.size())}}));
  if (!r.left.empty()) {
    c.print(c.words.tn("cmd.join.left", static_cast<double>(r.left.size())));
  }
}

/// commands.js linkOf(doc, name): the linked drawing a block belongs to (its root's name), or nothing
std::optional<std::string> link_of(const Ctx& c, const std::string& name) {
  const Value* b = c.doc.blocks().get(name);
  if (b == nullptr) {
    return std::nullopt;
  }
  const Value x = b->get("xref");
  if (!x.truthy()) {
    return std::nullopt;
  }
  const Value of = x.get("of");
  return of.is_undefined() ? base::js_string(b->get("name")) : base::js_string(of);
}

Task<void> explode_cmd(Ctx& c) {
  const Answer ids = co_await c.select();
  std::size_t n = 0;
  std::vector<std::string> linked;
  c.doc.change("EXPLODE", [&] {
    for (const Id id : ids.set) {
      const Value* ep = c.doc.entities().get(id);
      if (ep == nullptr) {
        continue;
      }
      const Value e = *ep;
      if (type_of(e) == "insert") {
        if (auto link = link_of(c, base::js_string(e.get("block")))) {
          if (std::ranges::find(linked, *link) == linked.end()) {
            linked.push_back(*link);
          }
          continue;
        }
      }
      const std::optional<base::Array> parts = cad::explode_entity(e, blocks_of(c));
      if (!parts) {
        continue;
      }
      c.doc.remove(id);
      for (const Value& p : *parts) {
        (void)c.doc.add(p);
      }
      ++n;
    }
  });
  c.host.set_selection({});
  for (const std::string& name : linked) {
    c.print(c.t("cmd.xref.readonly", {{"name", name}}));
  }
  if (n != 0 || linked.empty()) {
    c.print(n != 0 ? std::to_string(n) + " exploded" : c.t("cmd.explode.only_polylines_block_references"));
  }
}

}  // namespace

void add_reshape(std::vector<CommandDef>& t) {
  t.push_back(def("ELLIPSE", {"EL"}, ellipse_cmd, "cmd.ellipse.axis_endpoints_other_half"));
  t.push_back(def("POINT", {"PO"}, point_cmd, "cmd.point.point_objects"));
  CommandDef dup = def("DUPLICATE", {"DUP"}, duplicate_cmd, "cmd.duplicate.copy_moves_pointer_click");
  dup.keep_selection = true;
  t.push_back(std::move(dup));
  CommandDef all = def("SELECTALL", {"AI_SELALL"}, selectall_cmd, "cmd.selectall.select_every_object_you", true);
  all.keep_selection = true;
  t.push_back(std::move(all));
  t.push_back(def("DESELECT", {"AI_DESELECT", "SELECTNONE"}, deselect_cmd, "cmd.deselect.clear_selection_esc", true));
  t.push_back(def("STRETCH", {"S"}, stretch_cmd, "cmd.stretch.help"));
  t.push_back(def("BREAK", {"BR"}, break_cmd, "cmd.break.help"));
  t.push_back(def("BREAKATPOINT", {}, breakat_cmd, "cmd.breakat.help"));
  t.push_back(def("LENGTHEN", {"LEN"}, lengthen_cmd, "cmd.len.help"));
  t.push_back(def("JOIN", {"J"}, join_cmd, "cmd.join.help"));
  t.push_back(def("EXPLODE", {"X"}, explode_cmd, "cmd.explode.break_polylines_blocks_dimensions"));
}

}  // namespace cc::cmd::detail
