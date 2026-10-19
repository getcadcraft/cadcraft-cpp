// SPDX-License-Identifier: Apache-2.0
// commands.js (CadCraft 0.5) in C++, M5.4c: the view and the coordinate system — UCS (ucs.js), TURNLEFT / TURNRIGHT /
// TURNWORLD, PLAN, ZOOM, NAMEDVIEW — and the commands that hand over to the host (LAYER, NEW, OPEN, SAVE, SAVEAS,
// DXFOUT, PLOT, SHORTCUTS, COMPARESETTINGS). A host without a view (a script) answers what it cannot do.
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cad/dims.hpp"
#include "cc/cad/edit.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

std::string fmt(double n) {
  if (std::abs(n) < 1e-10) {
    return "0";
  }
  std::string s;
  base::append_number(s, base::js_number(base::js_to_fixed(n, 4)));
  return s;
}
std::string fmt_len(const Ctx& c, double n) {
  const Value lu = c.doc.meta.get("lunits");
  return cad::format_length(n, lu.is_undefined() || lu.is_null() ? Value(2) : lu, c.doc.meta.get("luprec"));
}
std::string text_of(const Answer& a) {
  if (a.kind == Answer::Kind::text) {
    return a.text;
  }
  return a.kind == Answer::Kind::typed ? base::js_string(a.typed) : std::string();
}
double num(const Value& e, std::string_view k) {
  return base::js_to_number(e.get(k));
}

// ---- ucs.js

/// cleanUcs(v): null for the world (or anything that is not a UCS), else { x, y, ang } with ang in [0, 2π)
Value clean_ucs(const Value& v) {
  if (!v.is_object()) {
    return {nullptr};
  }
  const double x = base::js_to_number(v.get("x"));
  const double y = base::js_to_number(v.get("y"));
  const double ang = base::js_to_number(v.get("ang"));
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(ang) || std::abs(x) > 1e15 || std::abs(y) > 1e15) {
    return {nullptr};
  }
  const double a = norm_angle(ang);
  if (std::abs(x) < 1e-12 && std::abs(y) < 1e-12 && std::min(a, kTau - a) < 1e-12) {
    return {nullptr};
  }
  return base::make_object({{"x", x}, {"y", y}, {"ang", a}});
}
Value ucs_of(double x, double y, double ang) {
  return base::make_object({{"x", x}, {"y", y}, {"ang", ang}});
}
/// ucsData(v): the same as a record, the world included
std::optional<Value> ucs_data(const Value& v) {
  if (!v.is_object()) {
    return std::nullopt;
  }
  const double x = base::js_to_number(v.get("x"));
  const double y = base::js_to_number(v.get("y"));
  const double ang = base::js_to_number(v.get("ang"));
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(ang) || std::abs(x) > 1e15 || std::abs(y) > 1e15) {
    return std::nullopt;
  }
  return ucs_of(x, y, norm_angle(ang));
}
/// clean_ucs(…) || { x: 0, y: 0, ang: 0 }
Value mk(double x, double y, double ang) {
  const Value u = clean_ucs(ucs_of(x, y, ang));
  return u.is_null() ? ucs_of(0, 0, 0) : u;
}
/// threePoint(o, px)
std::optional<Value> three_point(Pt o, Pt px) {
  if (dist(o, px) < 1e-9) {
    return std::nullopt;
  }
  return mk(o.x, o.y, base::js_atan2(px.y - o.y, px.x - o.x));
}
/// fromObject(e, pick, blocks): UCS Object
std::optional<Value> from_object(const Value& e, Pt pick, const cad::BlockLookup& blocks) {
  const std::string t = base::js_string(e.get("type"));
  if (t == "line") {
    const Pt a{num(e, "x1"), num(e, "y1")};
    const Pt b{num(e, "x2"), num(e, "y2")};
    if (dist(a, b) < 1e-9) {
      return std::nullopt;
    }
    return dist(pick, b) < dist(pick, a) ? mk(b.x, b.y, base::js_atan2(a.y - b.y, a.x - b.x))
                                         : mk(a.x, a.y, base::js_atan2(b.y - a.y, b.x - a.x));
  }
  if (t == "circle") {
    return mk(num(e, "cx"), num(e, "cy"), base::js_atan2(pick.y - num(e, "cy"), pick.x - num(e, "cx")));
  }
  if (t == "arc") {
    return mk(num(e, "cx"), num(e, "cy"), num(e, "a0"));
  }
  if (t == "poly") {
    const std::vector<cad::Prim> ps = cad::entity_prims(e, blocks);
    if (ps.empty()) {
      return std::nullopt;
    }
    std::size_t best = 0;
    double bd = std::numeric_limits<double>::infinity();
    const auto at = [](const cad::Prim& q, double u) {
      if (!q.arc) {
        return Pt{q.x1 + (q.x2 - q.x1) * u, q.y1 + (q.y2 - q.y1) * u};
      }
      const double a = q.a0 + (q.a1 - q.a0) * u;
      return Pt{q.cx + q.r * base::js_cos(a), q.cy + q.r * base::js_sin(a)};
    };
    for (std::size_t i = 0; i < ps.size(); ++i) {
      const double d = dist(at(ps[i], cad::prim_param(ps[i], {pick.x, pick.y})), pick);
      if (d < bd) {
        bd = d;
        best = i;
      }
    }
    const cad::Prim& q = ps[best];
    const Pt s = at(q, q.rev ? 1 : 0);
    const Pt f = at(q, q.rev ? 0.999 : 0.001);
    return mk(s.x, s.y, base::js_atan2(f.y - s.y, f.x - s.x));
  }
  if (t == "text" || t == "insert") {
    const Value r = e.get("rot");
    return mk(num(e, "x"), num(e, "y"), r.truthy() ? base::js_to_number(r) : 0);
  }
  if (t == "point") {
    return mk(num(e, "x"), num(e, "y"), 0);
  }
  if (t == "xline" || t == "ray") {
    return mk(num(e, "x"), num(e, "y"), base::js_atan2(num(e, "dy"), num(e, "dx")));
  }
  if (t == "dim") {
    const Value p1 = e.get("p1");
    if (!p1.truthy()) {
      return std::nullopt;
    }
    const Value r = e.get("rot");
    return mk(num(p1, "x"), num(p1, "y"), r.truthy() ? base::js_to_number(r) : 0);
  }
  return std::nullopt;
}
/// setUcs(doc, u, { remember })
void set_ucs(Ctx& c, const Value& u, bool remember = true) {
  if (remember) {
    c.doc.ucs_prev.push(c.doc.ucs.is_object() ? c.doc.ucs : Value(nullptr));
    if (c.doc.ucs_prev.as_array().size() > 10) {
      base::Array a = c.doc.ucs_prev.as_array();
      a.erase(a.begin());
      c.doc.ucs_prev = Value(std::move(a));
    }
  }
  c.doc.ucs = clean_ucs(u);
}
std::string ucs_label(const Ctx& c, const Value& u) {
  if (!u.is_object()) {
    return c.t("cmd.ucs.world");
  }
  return c.t("cmd.ucs.is", {{"at", fmt_len(c, num(u, "x")) + ", " + fmt_len(c, num(u, "y"))},
                            {"ang", fmt(deg(norm_angle(num(u, "ang"))))}});
}
void after_ucs(Ctx& c) {
  const Value u = c.doc.ucs;
  c.host.ucs_changed();
  (void)c.host.turn_view(u.is_object() ? num(u, "ang") : 0);
  c.print(ucs_label(c, u));
}
/// UCS_NAME_RE: 1 … 255 characters, none of < > / \ " : ; ? * | , = `
bool ucs_name_ok(const std::string& n) {
  return !n.empty() && base::utf16_length(n) <= 255 && n.find_first_of("<>/\\\":;?*|,=`") == std::string::npos;
}

Task<void> ucs_cmd(Ctx& c) {
  if (!c.doc.in_model()) {
    c.print(c.t("cmd.ucs.sheet"));
    co_return;
  }
  const Value cur = c.doc.ucs;
  const bool has = cur.is_object();
  Ask ar;
  ar.options = {c.opt("NA", "NAmed"), c.opt("OB", "OBject"), c.opt("P", "Previous"),
                c.opt("W", "World"),  c.opt("Z", "Z"),       c.opt("3", "3point")};
  ar.def = "World";
  const Answer r = co_await c.point(c.t("cmd.ucs.origin"), std::move(ar));
  std::string key = r.kind == Answer::Kind::option ? r.option : std::string();
  if (text_of(r) == "World") {
    key = "W";
  }
  if (key.empty() && r.kind == Answer::Kind::point) {  // a new origin, then (Enter keeps the angle) a point on X
    const Pt o = r.p;
    c.preview([o](Pt p) { return base::Array{line_e(o.x, o.y, p.x, p.y)}; });
    Ask ax;
    ax.base = o;
    ax.none = true;
    const Answer px = co_await c.point(c.t("cmd.ucs.xaxis"), std::move(ax));
    std::optional<Value> u;
    if (px.kind == Answer::Kind::point && dist(o, px.p) > 1e-12) {
      u = three_point(o, px.p);
    }
    set_ucs(c, u ? *u : ucs_of(o.x, o.y, has ? num(cur, "ang") : 0));
    after_ucs(c);
    co_return;
  }
  if (key == "W") {
    set_ucs(c, Value(nullptr));
    after_ucs(c);
    co_return;
  }
  if (key == "P") {
    if (c.doc.ucs_prev.as_array().empty()) {
      c.print(c.t("cmd.ucs.no_previous"));
      co_return;
    }
    base::Array a = c.doc.ucs_prev.as_array();
    const Value last = a.back();
    a.pop_back();
    c.doc.ucs_prev = Value(std::move(a));
    set_ucs(c, last, false);
    after_ucs(c);
    co_return;
  }
  if (key == "Z") {
    Ask az;
    az.def = kPi / 2;
    const Answer a = co_await c.angle(c.t("cmd.ucs.zangle"), std::move(az));
    set_ucs(c, ucs_of(has ? num(cur, "x") : 0, has ? num(cur, "y") : 0,
                      (has ? num(cur, "ang") : 0) + (a.kind == Answer::Kind::number ? a.number : kPi / 2)));
    after_ucs(c);
    co_return;
  }
  if (key == "3") {
    Pt o0{0, 0};  // toWorld(cur, { 0, 0 }): the UCS origin
    if (has) {
      o0 = {num(cur, "x"), num(cur, "y")};
    }
    Ask ao;
    ao.none = true;
    const Answer oa = co_await c.point(c.t("cmd.ucs.p_origin"), std::move(ao));
    const Pt o = oa.kind == Answer::Kind::point ? oa.p : o0;
    c.preview([o](Pt p) { return base::Array{line_e(o.x, o.y, p.x, p.y)}; });
    Ask ax;
    ax.base = o;
    ax.none = true;
    const Answer px = co_await c.point(c.t("cmd.ucs.p_x"), std::move(ax));
    std::optional<Value> u;
    if (px.kind == Answer::Kind::point) {
      u = three_point(o, px.p);
      if (!u) {
        c.print(c.t("cmd.ucs.same_point"));
        co_return;
      }
    } else {
      u = ucs_of(o.x, o.y, has ? num(cur, "ang") : 0);
    }
    Ask ay;
    ay.base = o;
    ay.none = true;
    (void)co_await c.point(c.t("cmd.ucs.p_y"), std::move(ay));  // the Y side in a flat drawing: read, not used
    set_ucs(c, *u);
    after_ucs(c);
    co_return;
  }
  if (key == "OB") {
    for (;;) {
      const Answer pick = co_await c.entity(c.t("cmd.ucs.object"));
      const Value* e = c.doc.entities().get(pick.id);
      const std::optional<Value> u =
          e != nullptr ? from_object(*e, pick.p, [&c](const std::string& n) { return c.doc.blocks().get(n); })
                       : std::nullopt;
      if (!u) {
        c.print(c.t("cmd.ucs.object_no"));
        continue;
      }
      set_ucs(c, *u);
      after_ucs(c);
      co_return;
    }
  }
  if (key == "NA") {
    Ask ak;
    ak.options = {c.opt("R", "Restore"), c.opt("S", "Save"), c.opt("D", "Delete"), c.opt("?", "?")};
    ak.def = "Restore";
    const Answer k = co_await c.keyword(c.t("cmd.ucs.named"), std::move(ak));
    std::string kk = k.kind == Answer::Kind::option ? k.option : "";
    if (text_of(k) == "Restore") {
      kk = "R";
    }
    if (!c.doc.ucs_named.is_array()) {
      c.doc.ucs_named = Value::array();
    }
    const auto find = [&c](const std::string& name) -> std::optional<std::size_t> {
      const std::string up = base::js_to_upper(base::js_trim(name));
      const base::Array& list = c.doc.ucs_named.as_array();
      for (std::size_t i = 0; i < list.size(); ++i) {
        if (base::js_to_upper(base::js_string(list[i].get("name"))) == up) {
          return i;
        }
      }
      return std::nullopt;
    };
    if (kk == "?") {
      const base::Array& list = c.doc.ucs_named.as_array();
      std::string out;
      for (const Value& x : list) {
        out += (out.empty() ? "" : "\n") + base::js_string(x.get("name")) + ": " + ucs_label(c, clean_ucs(x));
      }
      c.print(list.empty() ? c.t("cmd.ucs.none_saved") : out);
      co_return;
    }
    std::string prompt = "cmd.ucs.restore_which";
    if (kk == "S") {
      prompt = "cmd.ucs.save_as";
    } else if (kk == "D") {
      prompt = "cmd.ucs.delete_which";
    }
    const std::string name(base::js_trim(text_of(co_await c.string(c.t(prompt)))));
    if (name.empty()) {
      co_return;
    }
    const std::optional<std::size_t> i = find(name);
    if (kk == "S") {
      if (!ucs_name_ok(name)) {
        c.print(c.t("cmd.ucs.bad_name"));
        co_return;
      }
      const Value d = ucs_data(cur).value_or(ucs_of(0, 0, 0));
      const Value rec =
          base::make_object({{"name", name}, {"x", d.get("x")}, {"y", d.get("y")}, {"ang", d.get("ang")}});
      base::Array list = c.doc.ucs_named.as_array();
      if (i) {
        list[*i] = rec;
      } else {
        list.push_back(rec);
      }
      c.doc.ucs_named = Value(std::move(list));
      c.print(c.t("cmd.ucs.saved", {{"name", name}}));
      co_return;
    }
    if (!i) {
      c.print(c.t("cmd.ucs.no_such", {{"name", name}}));
      co_return;
    }
    if (kk == "D") {
      base::Array list = c.doc.ucs_named.as_array();
      list.erase(list.begin() + static_cast<std::ptrdiff_t>(*i));
      c.doc.ucs_named = Value(std::move(list));
      c.print(c.t("cmd.ucs.deleted", {{"name", name}}));
      co_return;
    }
    set_ucs(c, c.doc.ucs_named.as_array()[*i]);
    after_ucs(c);
  }
}

/// commands.js quarterTurn(c, sign): UCS Z ∓90, or back to the world
void quarter_turn(Ctx& c, int sign) {
  if (!c.doc.in_model()) {
    c.print(c.t("cmd.ucs.sheet"));
    return;
  }
  const Value cur = c.doc.ucs;
  const bool has = cur.is_object();
  set_ucs(c, sign != 0 ? ucs_of(has ? num(cur, "x") : 0, has ? num(cur, "y") : 0,
                                (has ? num(cur, "ang") : 0) + sign * kPi / 2)
                       : Value(nullptr));
  after_ucs(c);
}
Task<void> turnleft_cmd(Ctx& c) {
  quarter_turn(c, -1);
  co_return;
}
Task<void> turnright_cmd(Ctx& c) {
  quarter_turn(c, 1);
  co_return;
}
Task<void> turnworld_cmd(Ctx& c) {
  quarter_turn(c, 0);
  co_return;
}

Task<void> plan_cmd(Ctx& c) {
  Ask ak;
  ak.options = {c.opt("C", "Current ucs"), c.opt("U", "Ucs"), c.opt("W", "World")};
  ak.def = "Current ucs";
  const Answer k = co_await c.keyword(c.t("cmd.plan.ask"), std::move(ak));
  std::string kk = k.kind == Answer::Kind::option ? k.option : "";
  if (text_of(k) == "Current ucs") {
    kk = "C";
  }
  double ang = 0;
  if (kk == "C" && c.doc.in_model() && c.doc.ucs.is_object()) {
    const Value a = c.doc.ucs.get("ang");
    ang = a.truthy() ? base::js_to_number(a) : 0;
  }
  if (kk == "U") {
    const std::string name(base::js_trim(text_of(co_await c.string(c.t("cmd.ucs.restore_which")))));
    std::optional<Value> rec;
    if (c.doc.ucs_named.is_array()) {
      for (const Value& x : c.doc.ucs_named.as_array()) {
        if (base::js_to_upper(base::js_string(x.get("name"))) == base::js_to_upper(name)) {
          rec = x;
          break;
        }
      }
    }
    if (!rec) {
      c.print(c.t("cmd.ucs.no_such", {{"name", name}}));
      co_return;
    }
    ang = num(*rec, "ang");
  }
  if (!c.host.turn_view(ang)) {
    c.print(c.t("cmd.plan.browser"));
    co_return;
  }
  c.print(c.t(ang != 0 ? "cmd.plan.turned" : "cmd.plan.world", {{"ang", fmt(deg(norm_angle(ang)))}}));
}

// ---- ZOOM and NAMEDVIEW

/// commands.js zoomFactor(s): "2", "2x", ".5x" → the factor, else nothing
std::optional<double> zoom_factor(std::string_view raw) {
  std::string s(base::js_trim(raw));
  if (!s.empty() && (s.back() == 'x' || s.back() == 'X')) {
    s.pop_back();
    s = std::string(base::js_trim_end(s));
  }
  if (s.empty() || s.find_first_not_of("0123456789.") != std::string::npos || std::count(s.begin(), s.end(), '.') > 1 ||
      s == ".") {
    return std::nullopt;
  }
  const double v = base::js_number(s);
  return v > 0 ? std::optional<double>(v) : std::nullopt;
}

Task<void> zoom_cmd(Ctx& c) {
  Ask az;
  az.options = {c.opt("E", c.t("opt.extents")),  c.opt("A", c.t("opt.all")),    c.opt("W", c.t("opt.window")),
                c.opt("P", c.t("opt.previous")), c.opt("S", c.t("opt.zscale")), c.opt("C", c.t("opt.center"))};
  az.none = true;
  az.accept = [](std::string_view typed) {
    const auto f = zoom_factor(typed);
    return f ? Value(*f) : Value();
  };
  const Answer k = co_await c.point(c.t("cmd.zoom.prompt"), std::move(az));
  if (!k.truthy() || k.is("E") || k.is("A")) {
    c.host.zoom_extents();
    co_return;
  }
  if (k.is("P")) {
    if (!c.host.zoom_previous()) {
      c.print(c.t("cmd.zoom.noprev"));
    }
    co_return;
  }
  if (k.is("_typed") || k.is("S")) {  // _typed: a factor typed at the prompt (2x)
    double f = k.is("_typed") ? base::js_to_number(k.typed) : 0;
    if (k.is("S")) {
      const auto v = zoom_factor(text_of(co_await c.string(c.t("cmd.zoom.factor"))));
      if (!v) {
        c.print(c.t("cmd.zoom.factor.bad"));
        co_return;
      }
      f = *v;
    }
    c.host.zoom_by(f);
    co_return;
  }
  if (k.is("C")) {
    const Pt p = (co_await c.point(c.t("cmd.zoom.center"))).p;
    const std::optional<ViewRect> cur = c.host.view_rect();
    Ask ah;
    if (cur) {
      ah.def = fmt(cur->h);
    }
    const std::string v(base::js_trim(text_of(co_await c.string(c.t("cmd.zoom.height"), std::move(ah)))));
    const bool by = !v.empty() && (v.back() == 'x' || v.back() == 'X');
    const std::optional<double> fx = by ? zoom_factor(v) : std::nullopt;
    std::optional<double> h;
    if (fx) {
      if (cur) {
        h = cur->h / *fx;
      }
    } else {
      h = parse_length(v, c.doc.meta.get("lunits"));
    }
    if (!h || !(*h > 0)) {
      c.print(c.t("cmd.zoom.factor.bad"));
      co_return;
    }
    const double aspect = cur ? cur->w / cur->h : 1;
    c.host.zoom_window({p.x - *h * aspect / 2, p.y - *h / 2}, {p.x + *h * aspect / 2, p.y + *h / 2}, true);
    co_return;
  }
  Pt a = k.p;
  if (k.is("W")) {
    a = (co_await c.point(c.t("cmd.mtext.first"))).p;
  }
  c.preview([a](Pt cur) { return base::Array{rect(a, cur)}; });
  Ask ab;
  ab.base = a;
  const Pt b = (co_await c.point(c.t("cmd.mtext.corner"), std::move(ab))).p;
  c.host.zoom_window(a, b, false);
}

Task<void> namedview_cmd(Ctx& c) {
  const doc::Layout* lay = c.doc.layout();
  const Value here = lay != nullptr ? Value(lay->name) : Value(nullptr);
  const auto mine = [&c, &here] {
    std::vector<Value> out;
    for (const Value& v : c.doc.views.is_array() ? c.doc.views.as_array() : base::Array{}) {
      const Value l = v.get("layout");
      if ((l.is_undefined() || l.is_null() ? Value(nullptr) : l) == here) {
        out.push_back(v);
      }
    }
    return out;
  };
  const auto find = [&mine](const std::string& name) -> std::optional<Value> {
    const std::string low = base::js_to_lower(name);
    for (const Value& v : mine()) {
      if (base::js_to_lower(base::js_string(v.get("name"))) == low) {
        return v;
      }
    }
    return std::nullopt;
  };
  const auto names = [&c, &mine] {
    std::string s;
    for (const Value& v : mine()) {
      s += (s.empty() ? "" : ", ") + base::js_string(v.get("name"));
    }
    return s.empty() ? c.t("cmd.nv.none") : s;
  };
  Ask ak;
  ak.options = {c.opt("?", c.t("opt.list")), c.opt("R", c.t("opt.restore")), c.opt("S", c.t("opt.save")),
                c.opt("W", c.t("opt.window")), c.opt("D", c.t("opt.delete"))};
  ak.def = "R";
  const Answer k = co_await c.keyword(c.t("cmd.nv.option"), std::move(ak));
  const std::string o = k.key();
  if (o == "?") {
    std::string out;
    for (const Value& v : mine()) {
      out += (out.empty() ? "" : "\n") + base::js_string(v.get("name")) + "  (" + fmt_len(c, num(v, "w")) + " × " +
             fmt_len(c, num(v, "h")) + ")";
    }
    c.print(out.empty() ? c.t("cmd.nv.none") : out);
    co_return;
  }
  std::string prompt = "cmd.nv.name";
  if (o == "D") {
    prompt = "cmd.nv.delname";
  } else if (o == "R") {
    prompt = "cmd.nv.restname";
  }
  const std::string name(base::js_trim(text_of(co_await c.string(c.t(prompt)))));
  if (name.empty()) {
    co_return;
  }
  if (o == "R" || o == "D") {
    const std::optional<Value> v = find(name);
    if (!v) {
      c.print(c.t("cmd.nv.unknown", {{"name", name}, {"list", names()}}));
      co_return;
    }
    if (o == "R") {
      const double cx = num(*v, "cx");
      const double cy = num(*v, "cy");
      const double w = num(*v, "w");
      const double h = num(*v, "h");
      c.host.zoom_window({cx - w / 2, cy - h / 2}, {cx + w / 2, cy + h / 2}, true);
      c.print(c.t("cmd.nv.restored", {{"name", base::js_string(v->get("name"))}}));
      co_return;
    }
    base::Array list;
    for (const Value& x : c.doc.views.as_array()) {
      if (!(x == *v)) {
        list.push_back(x);
      }
    }
    c.doc.change("NAMEDVIEW", [&] { c.doc.set_views(Value(std::move(list))); });
    c.print(c.t("cmd.nv.deleted", {{"name", base::js_string(v->get("name"))}}));
    co_return;
  }
  if (base::utf16_length(name) > 255 || name.find_first_of("<>/\\\":;?*|=`") != std::string::npos) {
    c.print(c.t("cmd.nv.badname"));
    co_return;
  }
  double cx = 0;
  double cy = 0;
  double w = 0;
  double h = 0;
  if (o == "W") {
    const Pt a = (co_await c.point(c.t("sel.window.first"))).p;
    c.preview([a](Pt cur) { return base::Array{rect(a, cur)}; });
    Ask ab;
    ab.base = a;
    const Pt b = (co_await c.point(c.t("sel.window.second"), std::move(ab))).p;
    c.preview(nullptr);
    cx = (a.x + b.x) / 2;
    cy = (a.y + b.y) / 2;
    w = std::abs(b.x - a.x);
    h = std::abs(b.y - a.y);
  } else {
    const std::optional<ViewRect> r = c.host.view_rect();
    if (!r) {
      c.print(c.t("cmd.polarang.browser"));
      co_return;
    }
    cx = r->cx;
    cy = r->cy;
    w = r->w;
    h = r->h;
  }
  if (!(w > 0) || !(h > 0)) {
    c.print(c.t("cmd.nv.empty"));
    co_return;
  }
  const std::optional<Value> old = find(name);
  Value v =
      base::make_object({{"name", old ? old->get("name") : Value(name)}, {"cx", cx}, {"cy", cy}, {"w", w}, {"h", h}});
  if (!here.is_null()) {
    v.set("layout", here);
  }
  base::Array list;
  for (const Value& x : c.doc.views.is_array() ? c.doc.views.as_array() : base::Array{}) {
    if (!old || !(x == *old)) {
      list.push_back(x);
    }
  }
  list.push_back(v);
  c.doc.change("NAMEDVIEW", [&] { c.doc.set_views(Value(std::move(list))); });
  c.print(c.t(old ? "cmd.nv.replaced" : "cmd.nv.saved", {{"name", base::js_string(v.get("name"))}}));
}

// ---- what the host does (a window shows panels and files; a script has none)

Task<void> layer_cmd(Ctx& c) {
  c.host.show_layers();
  co_return;
}
Task<void> new_cmd(Ctx& c) {
  c.host.new_drawing();
  co_return;
}
Task<void> open_cmd(Ctx& c) {
  c.host.open_file();
  co_return;
}
Task<void> save_cmd(Ctx& c) {
  c.host.save_file({}, true);
  co_return;
}
Task<void> saveas_cmd(Ctx& c) {
  c.host.save_as();
  co_return;
}
Task<void> dxfout_cmd(Ctx& c) {
  c.host.save_file("dxf", false);
  co_return;
}
Task<void> plot_cmd(Ctx& c) {
  if (!c.host.plot()) {
    c.print(c.t("cmd.plot.plotting_needs_browser"));
  }
  co_return;
}
Task<void> shortcuts_cmd(Ctx& c) {
  c.print(c.t("keys.browser"));
  co_return;
}
Task<void> comparesettings_cmd(Ctx& c) {
  c.print(c.t("cmd.cmp.browser"));
  co_return;
}

}  // namespace

void add_views(std::vector<CommandDef>& t) {
  const auto keep = [&t](CommandDef d) {
    d.keep_selection = true;
    t.push_back(std::move(d));
  };
  keep(def("UCS", {}, ucs_cmd, "cmd.ucs.help", true));
  keep(def("TURNLEFT", {}, turnleft_cmd, "cmd.turnleft.help", true));
  keep(def("TURNRIGHT", {}, turnright_cmd, "cmd.turnright.help", true));
  keep(def("TURNWORLD", {}, turnworld_cmd, "cmd.turnworld.help", true));
  keep(def("PLAN", {}, plan_cmd, "cmd.plan.help", true));
  keep(def("SHORTCUTS", {"KEYS"}, shortcuts_cmd, "keys.help", true));
  keep(def("COMPARESETTINGS", {"CMPSET"}, comparesettings_cmd, "cmd.cmp.help", true));
  keep(def("PLOT", {"PRINT", "PUBLISH", "EXPORTPDF"}, plot_cmd, "cmd.plot.plot_pdf_layout_model", true));
  keep(def("ZOOM", {"Z"}, zoom_cmd, "cmd.zoom.extents_window", true));
  keep(def("NAMEDVIEW", {"NV", "VIEWS"}, namedview_cmd, "cmd.nv.help", true));
  keep(def("LAYER", {"LA"}, layer_cmd, "cmd.layer.show_layers_panel", true));
  t.push_back(def("NEW", {}, new_cmd, "cmd.new.start_new_drawing", true));
  t.push_back(def("OPEN", {}, open_cmd, "cmd.open.open_dwg_dxf_computer", true));
  t.push_back(def("SAVE", {"QSAVE"}, save_cmd, "cmd.save.download_drawing_format_dwg", true));
  t.push_back(def("SAVEAS", {}, saveas_cmd, "cmd.saveas.download_drawing_dwg_dxf", true));
  t.push_back(def("DXFOUT", {}, dxfout_cmd, "cmd.dxfout.download_drawing_dxf_r12", true));
}

}  // namespace cc::cmd::detail
