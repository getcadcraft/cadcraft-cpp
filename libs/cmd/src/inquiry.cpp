// SPDX-License-Identifier: Apache-2.0
// commands.js (CadCraft 0.5) in C++, M5.4a: inquiry — DIST, AREA, MEASUREGEOM, ID, LIST (with describe()).
#include <algorithm>
#include <array>
#include <cmath>
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

std::string type_of(const Value& e) {
  return base::js_string(e.get("type"));
}
double num(const Value& e, std::string_view k) {
  return base::js_to_number(e.get(k));
}
std::string fmt(double n) {
  if (std::abs(n) < 1e-10) {
    return "0";
  }
  std::string s;
  base::append_number(s, base::js_number(base::js_to_fixed(n, 4)));
  return s;
}
Value lunits_of(const Ctx& c) {
  const Value lu = c.doc.meta.get("lunits");
  return lu.is_undefined() || lu.is_null() ? Value(2) : lu;
}
std::string fmt_len(const Ctx& c, double n) {
  return cad::format_length(n, lunits_of(c), c.doc.meta.get("luprec"));
}
/// units.js formatArea(a, unitsOf(meta))
std::string fmt_area(const Ctx& c, double a) {
  const int code = cad::length_code(lunits_of(c));
  const int p = cad::length_prec(c.doc.meta.get("luprec"));
  if (code == 3 || code == 4) {
    return c.t("units.area.imperial", {{"sqin", cad::format_decimals(a, std::max(p, 2))},
                                       {"sqft", cad::format_decimals(a / 144, std::max(p, 2))}});
  }
  if (code == 1) {
    return cad::format_length(a, Value(1), Value(p));
  }
  return cad::format_decimals(a, code == 5 ? 4 : p);
}
std::optional<Value> active_ucs(const Ctx& c) {
  if (c.doc.in_model() && c.doc.ucs.truthy()) {
    return c.doc.ucs;
  }
  return std::nullopt;
}

/// a point prompt's default 'O' (Enter): the option, however the answer carries it
bool is_o(const Answer& p) {
  return p.is("O") || (p.kind == Answer::Kind::text && p.text == "O") ||
         (p.kind == Answer::Kind::typed && base::js_string(p.typed) == "O");
}

std::string dist_line(const Ctx& c, Pt a, Pt b) {
  return c.t("cmd.mea.dist", {{"d", fmt_len(c, dist(a, b))},
                              {"ang", fmt(deg(norm_angle(angle_of(a, b))))},
                              {"dx", fmt_len(c, b.x - a.x)},
                              {"dy", fmt_len(c, b.y - a.y)}});
}

Task<void> dist_cmd(Ctx& c) {
  const Pt a = (co_await c.point(c.t("cmd.mea.p1"))).p;
  c.preview([a](Pt cur) { return base::Array{line_e(a.x, a.y, cur.x, cur.y)}; });
  Ask ab;
  ab.base = a;
  const Pt b = (co_await c.point(c.t("cmd.mea.p2"), std::move(ab))).p;
  c.print(dist_line(c, a, b));
}

/// commands.js areaByPoints(c, first): the area and perimeter of points picked one by one
Task<std::optional<double>> area_by_points(Ctx& c, Pt first) {
  std::vector<Pt> pts{first};
  for (;;) {
    c.preview([&pts](Pt cur) {
      base::Array vs;
      for (const Pt p : pts) {
        vs.push_back(vtx(p.x, p.y));
      }
      vs.push_back(vtx(cur.x, cur.y));
      return base::Array{base::make_object({{"type", "poly"}, {"closed", true}, {"pts", std::move(vs)}})};
    });
    Ask a;
    a.base = pts.back();
    a.none = true;
    if (pts.size() > 1) {
      a.options = {c.opt("U", c.t("opt.undo"))};
    }
    const Answer p = co_await c.point(c.t("cmd.area.next"), std::move(a));
    if (!p.truthy()) {
      break;
    }
    if (p.is("U")) {
      pts.pop_back();
      continue;
    }
    if (dist(p.p, pts.back()) > 1e-12) {
      pts.push_back(p.p);
    }
  }
  c.preview(nullptr);
  if (pts.size() < 3) {
    c.print(c.t("cmd.area.few"));
    co_return std::nullopt;
  }
  std::vector<cad::P2> ring;
  ring.reserve(pts.size());
  for (const Pt p : pts) {
    ring.push_back({p.x, p.y});
  }
  const cad::PolygonMeasure m = cad::polygon_measure(ring);
  c.print(c.t("cmd.area.result", {{"area", fmt_area(c, std::abs(m.area))}, {"perimeter", fmt_len(c, m.perimeter)}}));
  co_return std::abs(m.area);
}

/// commands.js areaOfObject(c, e): one object's area, printed
std::optional<double> area_of_object(const Ctx& c, const Value* e) {
  const std::optional<cad::EntityArea> m = e != nullptr ? cad::entity_area(*e) : std::nullopt;
  if (!m) {
    c.print(c.t("cmd.area.noarea"));
    return std::nullopt;
  }
  std::string key = "cmd.area.result";
  if (m->kind == "circle") {
    key = "cmd.area.result.circle";
  } else if (m->kind == "open") {
    key = "cmd.area.result.open";
  }
  c.print(c.t(key, {{"area", fmt_area(c, m->area)}, {"perimeter", fmt_len(c, m->perimeter)}}));
  return m->area;
}

Task<void> area_cmd(Ctx& c) {
  std::string mode;  // '', add or sub (a running total)
  double total = 0;
  const auto tally = [&](std::optional<double> a) {
    if (!a || mode.empty()) {
      return;
    }
    total += mode == "add" ? *a : -*a;
    c.print(c.t("cmd.area.total", {{"area", fmt_area(c, total)}}));
  };
  const auto prefix = [&c, &mode] {
    return mode.empty() ? std::string() : "(" + c.t(mode == "add" ? "cmd.area.mode.add" : "cmd.area.mode.sub") + ") ";
  };
  for (;;) {
    Ask ap;
    ap.options = {c.opt("O", c.t("opt.object"))};
    if (mode != "add") {
      ap.options.push_back(c.opt("A", c.t("opt.addarea")));
    }
    if (mode != "sub") {
      ap.options.push_back(c.opt("S", c.t("opt.subarea")));
    }
    ap.none = true;
    if (mode.empty()) {
      ap.def = "O";
    }
    const Answer p = co_await c.point(prefix() + c.t("cmd.area.first"), std::move(ap));
    if (!p.truthy()) {
      co_return;
    }
    if (p.is("A") || p.is("S")) {
      mode = p.is("A") ? "add" : "sub";
      continue;
    }
    if (is_o(p)) {
      for (;;) {
        Ask ae;
        ae.none = true;
        const Answer pick = co_await c.entity(prefix() + c.t("cmd.area.object"), std::move(ae));
        if (pick.kind != Answer::Kind::entity) {
          break;
        }
        IdSet sel;
        sel.add(pick.id);
        c.host.set_selection(std::move(sel));
        tally(area_of_object(c, c.doc.entities().get(pick.id)));
        if (mode.empty()) {
          co_return;
        }
      }
      c.host.set_selection({});
      if (mode.empty()) {
        co_return;
      }
      continue;
    }
    const std::optional<double> m = co_await area_by_points(c, p.p);
    tally(m);
    if (mode.empty()) {
      co_return;
    }
  }
}

Task<void> measuregeom_cmd(Ctx& c) {
  for (bool first = true;; first = false) {
    Ask ak;
    ak.options = {c.opt("D", c.t("opt.distance")), c.opt("R", c.t("opt.radius")), c.opt("A", c.t("opt.angle")),
                  c.opt("AR", c.t("opt.area"))};
    if (!first) {
      ak.options.push_back(c.opt("X", c.t("opt.exit")));
    }
    ak.def = first ? "D" : "X";
    const Answer k = co_await c.keyword(c.t("cmd.mea.option"), std::move(ak));
    const std::string o = k.key();
    if (o == "X" || o.empty()) {
      co_return;
    }
    if (o == "D") {
      const Pt a = (co_await c.point(c.t("cmd.mea.p1"))).p;
      c.preview([a](Pt cur) { return base::Array{line_e(a.x, a.y, cur.x, cur.y)}; });
      Ask ab;
      ab.base = a;
      const Pt b = (co_await c.point(c.t("cmd.mea.p2"), std::move(ab))).p;
      c.preview(nullptr);
      c.print(dist_line(c, a, b));
    } else if (o == "R") {
      for (;;) {
        const Answer pick = co_await c.entity(c.t("cmd.mea.arc"));
        const Value* e = c.doc.entities().get(pick.id);
        if (e == nullptr || (type_of(*e) != "arc" && type_of(*e) != "circle")) {
          c.print(c.t("cmd.mea.arc.only"));
          continue;
        }
        c.print(c.t("cmd.mea.radius", {{"r", fmt_len(c, num(*e, "r"))}, {"d", fmt_len(c, 2 * num(*e, "r"))}}));
        break;
      }
    } else if (o == "A") {
      Ask ae;
      ae.none = true;
      const Answer pick = co_await c.entity(c.t("cmd.mea.angle.pick"), std::move(ae));
      double ang = 0;
      if (pick.kind != Answer::Kind::entity) {
        const Pt v = (co_await c.point(c.t("cmd.mea.vertex"))).p;
        Ask a1;
        a1.base = v;
        const Pt a = (co_await c.point(c.t("cmd.mea.end1"), std::move(a1))).p;
        Ask a2;
        a2.base = v;
        const Pt b = (co_await c.point(c.t("cmd.mea.end2"), std::move(a2))).p;
        ang = std::abs(norm_angle(angle_of(v, b) - angle_of(v, a)));
        if (ang > kPi) {
          ang = kTau - ang;
        }
      } else {
        const Value* ep = c.doc.entities().get(pick.id);
        const Value e = ep != nullptr ? *ep : Value::object();
        if (type_of(e) == "arc") {
          const double sw = norm_angle(num(e, "a1") - num(e, "a0"));
          ang = sw != 0 && !std::isnan(sw) ? sw : kTau;
        } else if (type_of(e) == "line") {
          const Answer q = co_await c.entity(c.t("cmd.mea.line2"));
          const Value* fp = c.doc.entities().get(q.id);
          if (fp == nullptr || type_of(*fp) != "line") {
            c.print(c.t("cmd.mea.line2.only"));
            continue;
          }
          const Value f = *fp;
          // between the two lines, each toward its end farther from where they meet
          const auto prim = [](const Value& l) {
            cad::Prim p;
            p.x1 = num(l, "x1");
            p.y1 = num(l, "y1");
            p.x2 = num(l, "x2");
            p.y2 = num(l, "y2");
            return p;
          };
          const std::vector<cad::P2> xs = cad::intersect(prim(e), prim(f), true, true);
          if (xs.empty()) {
            c.print(c.t("cmd.chamfer.err.parallel"));
            continue;
          }
          const Pt x{xs[0].x, xs[0].y};
          const auto far = [x](const Value& l) {
            const Pt s{num(l, "x1"), num(l, "y1")};
            const Pt t{num(l, "x2"), num(l, "y2")};
            return dist(x, s) > dist(x, t) ? s : t;
          };
          ang = std::abs(norm_angle(angle_of(x, far(f)) - angle_of(x, far(e))));
          if (ang > kPi) {
            ang = kTau - ang;
          }
        } else {
          c.print(c.t("cmd.mea.angle.only"));
          continue;
        }
      }
      c.print(c.t("cmd.mea.angle", {{"a", fmt(deg(ang))}}));
    } else if (o == "AR") {
      Ask ap;
      ap.options = {c.opt("O", c.t("opt.object"))};
      ap.def = "O";
      const Answer p = co_await c.point(c.t("cmd.area.first"), std::move(ap));
      if (is_o(p)) {
        const Answer pick = co_await c.entity(c.t("cmd.area.object"));
        (void)area_of_object(c, c.doc.entities().get(pick.id));
      } else {
        (void)co_await area_by_points(c, p.p);
      }
    }
  }
}

Task<void> id_cmd(Ctx& c) {
  const Pt p = (co_await c.point(c.t("cmd.id.specify_point"))).p;
  Pt q = p;  // in the UCS (DR.8)
  if (const auto u = active_ucs(c)) {
    const double ang = num(*u, "ang");
    const Pt v{p.x - num(*u, "x"), p.y - num(*u, "y")};
    q = {v.x * base::js_cos(ang) + v.y * base::js_sin(ang), -v.x * base::js_sin(ang) + v.y * base::js_cos(ang)};
  }
  c.print(c.t("cmd.id.result", {{"x", fmt_len(c, q.x)}, {"y", fmt_len(c, q.y)}}));
}

/// commands.js describe(e, units): what LIST says about an object
std::string describe(const Ctx& c, const Value& e) {
  const auto l = [&c](double n) { return fmt_len(c, n); };
  const auto pt = [&l](double x, double y) { return l(x) + ", " + l(y); };
  const std::string t = type_of(e);
  if (t == "line") {
    return c.t("cmd.desc.line",
               {{"from", pt(num(e, "x1"), num(e, "y1"))},
                {"to", pt(num(e, "x2"), num(e, "y2"))},
                {"length", l(base::js_hypot(num(e, "x2") - num(e, "x1"), num(e, "y2") - num(e, "y1")))}});
  }
  if (t == "circle") {
    return c.t("cmd.desc.circle", {{"center", pt(num(e, "cx"), num(e, "cy"))}, {"r", l(num(e, "r"))}});
  }
  if (t == "arc") {
    return c.t("cmd.desc.arc", {{"center", pt(num(e, "cx"), num(e, "cy"))},
                                {"r", l(num(e, "r"))},
                                {"a0", fmt(deg(num(e, "a0")))},
                                {"a1", fmt(deg(num(e, "a1")))}});
  }
  if (t == "poly") {
    const Value pts = e.get("pts");
    const double n = pts.is_array() ? static_cast<double>(pts.as_array().size()) : 0;
    return c.words.tn(e.get("closed").truthy() ? "cmd.desc.poly.closed" : "cmd.desc.poly", n);
  }
  if (t == "text") {
    return c.t("cmd.desc.text",
               {{"s", base::js_string(e.get("s"))}, {"at", pt(num(e, "x"), num(e, "y"))}, {"h", l(num(e, "h"))}});
  }
  if (t == "insert") {
    const Value at = e.get("attrs");
    const std::size_t na = at.is_object() ? base::js_members(at.as_object()).size() : 0;
    const std::string block = base::js_string(e.get("block"));
    if (na != 0) {
      return c.words.tn("cmd.desc.insert.attrs", static_cast<double>(na),
                        {{"block", block}, {"at", pt(num(e, "x"), num(e, "y"))}});
    }
    return c.t("cmd.desc.insert", {{"block", block}, {"at", pt(num(e, "x"), num(e, "y"))}});
  }
  if (t == "point") {
    return pt(num(e, "x"), num(e, "y"));
  }
  if (t == "ellipse") {
    return c.t("cmd.desc.ellipse", {{"center", pt(num(e, "cx"), num(e, "cy"))}, {"ratio", fmt(num(e, "ratio"))}});
  }
  if (t == "hatch") {
    const Value loops = e.get("loops");
    return c.words.tn("cmd.desc.hatch", loops.is_array() ? static_cast<double>(loops.as_array().size()) : 0);
  }
  if (t == "dim") {
    const std::string k = base::js_string(e.get("kind"));
    const bool known = k == "linear" || k == "aligned" || k == "angular" || k == "radius" || k == "diameter";
    const std::string kind = base::js_to_lower(known ? c.t("dimkind." + k) : c.t("cmd.desc.dim.imported"));
    if (!e.get("kind").truthy()) {
      return c.t("cmd.desc.dim.block", {{"kind", kind}, {"block", base::js_string(e.get("block"))}});
    }
    const std::string value = cad::dim_value_main(e, cad::dim_measure(e));
    if (e.get("text").truthy()) {
      return c.t("cmd.desc.dim.text", {{"kind", kind}, {"value", value}, {"text", base::js_string(e.get("text"))}});
    }
    return c.t("cmd.desc.dim", {{"kind", kind}, {"value", value}});
  }
  if (t == "viewport") {
    return c.t("cmd.desc.viewport", {{"w", l(num(e, "w"))},
                                     {"h", l(num(e, "h"))},
                                     {"at", pt(num(e, "x"), num(e, "y"))},
                                     {"view", pt(num(e, "cx"), num(e, "cy"))},
                                     {"s", fmt(num(e, "s"))}});
  }
  if (t == "xline" || t == "ray") {
    return c.t("cmd.desc." + t, {{"at", pt(num(e, "x"), num(e, "y"))},
                                 {"angle", fmt(deg(norm_angle(base::js_atan2(num(e, "dy"), num(e, "dx")))))}});
  }
  if (t == "mleader") {
    const Value pts = e.get("pts");
    const Value p0 = pts.is_array() && !pts.as_array().empty() ? pts.as_array()[0] : Value::object();
    const Value s = e.get("s");
    return c.words.tn("cmd.mld.describe", pts.is_array() ? static_cast<double>(pts.as_array().size()) : 0,
                      {{"s", cad::one_line(s.truthy() ? base::js_string(s) : std::string())},
                       {"at", pt(num(p0, "x"), num(p0, "y"))}});
  }
  return {};
}

/// attrs.js attrRows(e, blk): every attribute of a reference, in definition order (then tags the block lacks)
struct AttrRow {
  std::string tag, value;
  bool invisible = false, constant = false;
};
std::vector<AttrRow> attr_rows(const Value& e, const Value* blk) {
  std::vector<AttrRow> rows;
  const Value vals = e.get("attrs").is_object() ? e.get("attrs") : Value::object();
  const Value loc = e.get("attloc");
  const auto loc_invisible = [&loc](const std::string& tag) {
    const Value l = loc.is_object() ? loc.get(tag) : Value();
    return l.is_object() ? l.get("invisible") : Value();
  };
  std::vector<std::string> seen;
  const Value ads = blk != nullptr ? blk->get("attdefs") : Value();
  for (const Value& d : ads.is_array() ? ads.as_array() : base::Array{}) {
    const std::string tag = base::js_string(d.get("tag"));
    seen.push_back(tag);
    const bool constant = d.get("constant").truthy();
    if (!constant && !vals.has(tag)) {
      continue;
    }
    const Value v = vals.get(tag);
    const Value li = loc_invisible(tag);
    std::string value;  // constant: its default; else String(vals[tag] ?? '')
    if (constant) {
      value = base::js_string(d.get("def"));
    } else if (!v.is_undefined() && !v.is_null()) {
      value = base::js_string(v);
    }
    rows.push_back({.tag = tag,
                    .value = std::move(value),
                    .invisible = (li.is_undefined() || li.is_null() ? d.get("invisible") : li).truthy(),
                    .constant = constant});
  }
  for (const base::Member* m : base::js_members(vals.as_object())) {
    if (std::ranges::find(seen, m->key) != seen.end()) {
      continue;
    }
    const Value& v = m->value;
    rows.push_back({.tag = m->key,
                    .value = v.is_undefined() || v.is_null() ? std::string() : base::js_string(v),
                    .invisible = loc_invisible(m->key).truthy(),
                    .constant = false});
  }
  return rows;
}

/// geom.js / doc.js typeName(e)
std::string type_name(const Ctx& c, const Value& e) {
  static constexpr std::array<const char*, 14> kTypes{"line",     "circle",  "arc",    "poly",  "ellipse",
                                                      "text",     "point",   "insert", "hatch", "dim",
                                                      "viewport", "mleader", "xline",  "ray"};
  std::string t = type_of(e);
  for (const char* k : kTypes) {
    if (t == k) {
      return c.t(std::string("type.") + k);
    }
  }
  return t;
}

Task<void> list_cmd(Ctx& c) {
  const Answer ids = co_await c.select();
  for (const Id id : ids.set) {
    const Value* ep = c.doc.entities().get(id);
    if (ep == nullptr) {
      continue;
    }
    const Value& e = *ep;
    c.print(c.t("cmd.list.line",
                {{"type", type_name(c, e)}, {"layer", base::js_string(e.get("layer"))}, {"what", describe(c, e)}}));
    if (type_of(e) != "insert") {
      continue;
    }
    const Value* blk = c.doc.blocks().get(base::js_string(e.get("block")));
    for (const AttrRow& a : attr_rows(e, blk)) {
      std::string line = "  " + a.tag + " = " + (a.value.empty() ? c.t("cmd.list.attr.empty") : a.value);
      if (a.invisible) {
        line += "  " + c.t("cmd.list.attr.invisible");
      }
      if (a.constant) {
        line += "  " + c.t("cmd.list.attr.constant");
      }
      c.print(line);
    }
  }
}

// ---- UNITS (units.js; commands.js setLengthFormat, lengthFormatLabel)

struct LunitOpt {
  const char* key;
  int code;
  const char* label;
};
constexpr std::array<LunitOpt, 5> kLunits{{{"S", 1, "opt.lunit.scientific"},
                                           {"D", 2, "opt.lunit.decimal"},
                                           {"E", 3, "opt.lunit.engineering"},
                                           {"A", 4, "opt.lunit.architectural"},
                                           {"F", 5, "opt.lunit.fractional"}}};

std::string format_label(const Ctx& c, int code, int prec) {
  const std::string name = c.t(kLunits.at(static_cast<std::size_t>(code - 1)).label);
  if (code == 4 || code == 5) {
    return c.t("cmd.units.label.frac", {{"format", name}, {"den", fmt(std::pow(2.0, prec))}});
  }
  return c.t("cmd.units.label.dec", {{"format", name}, {"n", std::to_string(prec)}});
}

Task<void> units_cmd(Ctx& c) {
  const int cur_code = cad::length_code(lunits_of(c));
  const int cur_prec = cad::length_prec(c.doc.meta.get("luprec"));
  const auto example = [](int code, int prec) { return cad::format_length(150.5, Value(code), Value(prec)); };
  c.print(c.t("cmd.units.now",
              {{"format", format_label(c, cur_code, cur_prec)}, {"example", example(cur_code, cur_prec)}}));
  Ask ak;
  for (const LunitOpt& o : kLunits) {
    ak.options.push_back(c.opt(o.key, c.t(o.label)));
  }
  ak.def = kLunits.at(static_cast<std::size_t>(cur_code - 1)).key;
  const Answer k = co_await c.keyword(c.t("cmd.units.format"), std::move(ak));
  int code = cur_code;
  for (const LunitOpt& o : kLunits) {
    if (k.key() == o.key) {
      code = o.code;
    }
  }
  int prec = 0;
  for (;;) {
    if (code == 4 || code == 5) {
      Ask ad;
      ad.def = std::pow(2.0, code == cur_code ? cur_prec : 4);
      const Answer den = co_await c.number(c.t("cmd.units.den"), std::move(ad));
      const double p = den.kind == Answer::Kind::number ? std::log2(den.number) : std::nan("");
      if (std::isfinite(p) && std::floor(p) == p && p >= 0 && p <= 8) {
        prec = static_cast<int>(p);
        break;
      }
      c.print(c.t("cmd.units.den.bad"));
    } else {
      Ask an;
      an.def = code == cur_code ? cur_prec : 4;
      const Answer n = co_await c.number(c.t("cmd.units.dec"), std::move(an));
      if (n.kind == Answer::Kind::number && std::floor(n.number) == n.number && n.number >= 0 && n.number <= 8) {
        prec = static_cast<int>(n.number);
        break;
      }
      c.print(c.t("cmd.units.dec.bad"));
    }
  }
  // setLengthFormat(doc, code, prec): one undo step; new dimensions follow it
  c.doc.change("UNITS", [&] { c.doc.set_meta(base::make_object({{"lunits", code}, {"luprec", prec}})); });
  Value dim = c.doc.current.get("dim");
  dim.set("lunit", code);
  dim.set("dec", prec);
  c.doc.current.set("dim", dim);
  c.print(c.t("cmd.units.set", {{"format", format_label(c, code, prec)}, {"example", example(code, prec)}}));
  if (code == 3 || code == 4) {
    c.print(c.t("cmd.units.typing"));
    const Value u = c.doc.meta.get("units");
    if (u.truthy() && base::js_string(u) != "inches") {
      c.print(c.t("cmd.units.inches", {{"units", base::js_string(u)}}));
    }
  }
}

}  // namespace

void add_inquiry(std::vector<CommandDef>& t) {
  t.push_back(def("DIST", {"DI"}, dist_cmd, "cmd.dist.distance_angle_between_two"));
  t.push_back(def("AREA", {"AA"}, area_cmd, "cmd.area.help"));
  t.push_back(def("MEASUREGEOM", {"MEA"}, measuregeom_cmd, "cmd.mea.help"));
  t.push_back(def("ID", {}, id_cmd, "cmd.id.coordinates_point"));
  t.push_back(def("LIST", {"LI", "LS"}, list_cmd, "cmd.list.describe_objects_history"));
  CommandDef un = def("UNITS", {"UN", "DDUNITS"}, units_cmd, "cmd.units.help", true);
  un.keep_selection = true;
  t.push_back(std::move(un));
}

}  // namespace cc::cmd::detail
