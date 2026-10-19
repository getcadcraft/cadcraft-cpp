// SPDX-License-Identifier: Apache-2.0
// commands.js (CadCraft 0.5) in C++, M5.1b: FILLET and CHAMFER (geom.js filletPair / chamferPair / polyCorners).
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cad/edit.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

cad::P2 p2(Pt p) {
  return {p.x, p.y};
}
/// commands.js fmt(n)
std::string fmt(double n) {
  if (std::abs(n) < 1e-10) {
    return "0";
  }
  std::string s;
  base::append_number(s, base::js_number(base::js_to_fixed(n, 4)));
  return s;
}

/// commands.js cornerError(kind, code)
std::string corner_error(const Ctx& c, bool fillet, const std::string& code) {
  std::string key;
  if (fillet) {
    if (code == "type" || code == "large" || code == "none") {
      key = "cmd.fillet.err." + code;
    } else if (code == "parallel") {
      key = "cmd.chamfer.err.parallel";
    }
  } else {
    if (code == "type" || code == "large" || code == "parallel" || code == "angle") {
      key = "cmd.chamfer.err." + code;
    } else if (code == "none") {
      key = "cmd.chamfer.err.parallel";
    }
  }
  if (key.empty() && (code == "same" || code == "collinear" || code == "middle")) {
    key = "cmd.corner.err." + code;
  }
  return c.t(key.empty() ? "cmd.corner.err.none" : key);
}

bool corner_trim(const Ctx& c) {
  const Value v = c.engine.state.get("cornerTrim");
  return !v.is_bool() || v.as_bool();  // cornerTrim !== false
}
std::string trim_label(const Ctx& c) {
  return c.t(corner_trim(c) ? "cmd.corner.mode.trim" : "cmd.corner.mode.notrim");
}

/// commands.js cornerPick: → a target, an option, the object that is no target, or nothing (Enter)
struct Pick {
  enum class Kind : std::uint8_t { nothing, option, target, other };
  Kind kind = Kind::nothing;
  std::string option;
  std::optional<cad::CornerTarget> target;
  Value e;
  bool shift = false;
};
Task<Pick> corner_pick(Ctx& c, std::string prompt, std::vector<Option> options, bool none) {
  for (;;) {
    Ask a;
    a.options = options;
    a.none = none;
    const Answer r = co_await c.entity(prompt, std::move(a));
    Pick p;
    if (r.kind == Answer::Kind::option) {
      p.kind = Pick::Kind::option;
      p.option = r.option;
      co_return p;
    }
    if (r.kind != Answer::Kind::entity) {
      co_return p;
    }
    const Value* e = c.doc.entities().get(r.id);
    if (e == nullptr) {
      co_return p;
    }
    if (!c.doc.editable(*e)) {
      c.print(c.t("cmd.corner.err.locked"));
      continue;
    }
    p.target = cad::corner_target(*e, p2(r.p));
    p.kind = p.target ? Pick::Kind::target : Pick::Kind::other;
    p.e = *e;
    p.shift = r.shift;
    co_return p;
  }
}

/// commands.js applyCorner(c, kind, r, label): true when it was made
bool apply_corner(Ctx& c, bool fillet, const cad::CornerResult& r) {
  if (!r.ok) {
    c.print(corner_error(c, fillet, r.error));
    return false;
  }
  if (r.edits.empty() && r.add.empty()) {
    c.print(c.t("cmd.corner.nothing"));
    return true;
  }
  c.doc.change(fillet ? "FILLET" : "CHAMFER", [&] {
    for (const Value& e : r.edits) {
      (void)c.doc.update(id_of(e), e);
    }
    for (const Value& e : r.add) {
      (void)c.doc.add(e);
    }
  });
  return true;
}

/// commands.js cornerPreview(c, first, make): the corner with the object under the cursor as the second pick
void corner_preview(Ctx& c, std::function<cad::CornerResult(const cad::CornerTarget&)> make) {
  c.preview([&c, fn = std::move(make)](Pt cur) {
    base::Array out;
    const auto id = c.host.pick_at(cur);
    const Value* e = id ? c.doc.entities().get(*id) : nullptr;
    if (e == nullptr || !c.doc.editable(*e)) {
      return out;
    }
    const auto tb = cad::corner_target(*e, p2(cur));
    if (!tb) {
      return out;
    }
    const cad::CornerResult r = fn(*tb);
    if (r.ok) {
      out = r.edits;
      out.insert(out.end(), r.add.begin(), r.add.end());
    }
    return out;
  });
}

Task<void> ask_trim_mode(Ctx& c) {
  Ask a;
  a.options = {c.opt("T", c.t("opt.trim")), c.opt("N", c.t("opt.notrim"))};
  a.def = corner_trim(c) ? "T" : "N";
  const Answer k = co_await c.keyword(c.t("cmd.corner.trim"), std::move(a));
  c.engine.state.set("cornerTrim", k.key() != "N");
}

/// commands.js polyCornerCmd: the P option, every corner of one polyline
// (by value: the coroutine's frame keeps its own copies, nothing refers to the caller's temporaries)
Task<void> poly_corner_cmd(Ctx& c, bool fillet, std::vector<Option> options,
                           std::function<Task<void>(Ctx&, const std::string&)> ask, std::function<double()> radius,
                           std::function<cad::ChamferSpec()> spec) {
  for (;;) {
    const Pick p = co_await corner_pick(c, c.t("cmd.corner.poly"), options, false);
    if (p.kind == Pick::Kind::nothing) {
      co_return;
    }
    if (p.kind == Pick::Kind::option) {
      co_await ask(c, p.option);
      continue;
    }
    const Value e = p.target ? p.target->e : p.e;
    if (base::js_string(e.get("type")) != "poly") {
      c.print(c.t("cmd.corner.notpoly"));
      continue;
    }
    const cad::PolyCornersResult r = cad::poly_corners(e, fillet, radius(), spec());
    if (r.done != 0 || r.removed != 0) {
      c.doc.change(fillet ? "FILLET" : "CHAMFER", [&] { (void)c.doc.update(id_of(e), r.e); });
    }
    c.print(r.done != 0
                ? c.words.tn(fillet ? "cmd.fillet.poly.done" : "cmd.chamfer.poly.done", static_cast<double>(r.done))
                : c.t("cmd.corner.poly.none"));
    if (r.short_ != 0) {
      c.print(c.words.tn("cmd.corner.poly.short", static_cast<double>(r.short_)));
    }
    if (r.arcs != 0) {
      c.print(c.words.tn("cmd.corner.poly.arcs", static_cast<double>(r.arcs)));
    }
    co_return;
  }
}

double fillet_r(const Ctx& c) {
  const Value v = c.engine.state.get("filletR");
  return v.is_number() ? v.as_number() : 0;
}

Task<void> ask_radius(Ctx& c) {
  Ask a;
  a.def = fillet_r(c);
  const Answer r = co_await c.distance(c.t("cmd.fillet.radius"), std::move(a));
  if (r.kind == Answer::Kind::number) {
    c.engine.state.set("filletR", r.number);
  }
}

Task<void> fillet_cmd(Ctx& c) {
  if (!c.engine.state.get("filletR").is_number()) {
    c.engine.state.set("filletR", 0);
  }
  c.host.set_selection({});
  bool multiple = false;
  c.print(c.t("cmd.fillet.settings", {{"mode", trim_label(c)}, {"r", fmt(fillet_r(c))}}));
  const std::function<Task<void>(Ctx&, const std::string&)> ask_opt =
      [](Ctx& cc, const std::string& /*o*/) -> Task<void> { co_await ask_radius(cc); };
  for (;;) {
    std::vector<Option> first_opts = {c.opt("U", c.t("opt.undo")), c.opt("P", c.t("opt.polyline")),
                                      c.opt("R", c.t("opt.radius")), c.opt("T", c.t("opt.trim")),
                                      c.opt("M", c.t("opt.multiple"))};
    const Pick a = co_await corner_pick(c, c.t("cmd.fillet.first"), std::move(first_opts), true);
    if (a.kind == Pick::Kind::nothing) {
      co_return;
    }
    if (a.kind == Pick::Kind::option) {
      if (a.option == "U") {
        undo_inside(c);
      } else if (a.option == "R") {
        co_await ask_radius(c);
      } else if (a.option == "T") {
        co_await ask_trim_mode(c);
      } else if (a.option == "M") {
        multiple = true;
      } else if (a.option == "P") {
        co_await poly_corner_cmd(
            c, true, {c.opt("R", c.t("opt.radius"))}, ask_opt, [&c] { return fillet_r(c); },
            [] { return cad::ChamferSpec{}; });
        if (!multiple) {
          co_return;
        }
      }
      continue;
    }
    if (!a.target) {
      c.print(c.t("cmd.fillet.err.type"));
      continue;
    }
    const cad::CornerTarget first = *a.target;
    IdSet sel;
    sel.add(id_of(first.e));
    c.host.set_selection(std::move(sel));
    corner_preview(c, [&c, first](const cad::CornerTarget& tb) {
      return cad::fillet_pair(first, tb, fillet_r(c), corner_trim(c), c.doc.current);
    });
    Pick b;
    for (;;) {
      std::vector<Option> second_opts = {c.opt("R", c.t("opt.radius"))};
      b = co_await corner_pick(c, c.t("cmd.fillet.second"), std::move(second_opts), false);
      if (b.kind == Pick::Kind::option && b.option == "R") {
        co_await ask_radius(c);
        continue;
      }
      break;
    }
    c.preview(nullptr);
    c.host.set_selection({});
    if (b.kind == Pick::Kind::nothing) {  // Enter where an object is required: the JS command stops there
      co_return;
    }
    if (!b.target) {
      c.print(c.t("cmd.fillet.err.type"));
      continue;
    }
    const cad::CornerResult r =
        cad::fillet_pair(first, *b.target, b.shift ? 0 : fillet_r(c), corner_trim(c), c.doc.current);
    if (r.ok && r.semicircle) {
      c.print(c.t("cmd.fillet.parallel"));
    }
    if (!apply_corner(c, true, r)) {
      continue;
    }
    if (!multiple) {
      co_return;
    }
  }
}

cad::ChamferSpec chamfer_spec(const Ctx& c) {
  cad::ChamferSpec s;
  const Value v = c.engine.state.get("chamfer");
  if (!v.is_object()) {
    s.d2 = 0;
    return s;
  }
  s.method = base::js_string(v.get("method"));
  s.d1 = base::js_to_number(v.get("d1"));
  s.d2 = base::js_to_number(v.get("d2"));
  s.len = base::js_to_number(v.get("len"));
  s.ang = base::js_to_number(v.get("ang"));
  return s;
}
void save_chamfer(Ctx& c, const cad::ChamferSpec& s) {
  c.engine.state.set(
      "chamfer", base::make_object(
                     {{"method", s.method}, {"d1", s.d1}, {"d2", s.d2 ? *s.d2 : 0}, {"len", s.len}, {"ang", s.ang}}));
}

Task<void> chamfer_ask(Ctx& c, const std::string& o) {
  cad::ChamferSpec s = chamfer_spec(c);
  if (o == "D") {
    Ask a1;
    a1.def = s.d1;
    const Answer d1 = co_await c.distance(c.t("cmd.chamfer.d1"), std::move(a1));
    if (d1.kind == Answer::Kind::number) {
      s.d1 = d1.number;
      s.d2 = d1.number;
    }
    Ask a2;
    a2.def = s.d2 ? *s.d2 : 0;
    const Answer d2 = co_await c.distance(c.t("cmd.chamfer.d2"), std::move(a2));
    if (d2.kind == Answer::Kind::number) {
      s.d2 = d2.number;
    }
    s.method = "D";
  } else if (o == "A") {
    Ask alen;
    alen.def = s.len;
    const Answer len = co_await c.distance(c.t("cmd.chamfer.len"), std::move(alen));
    if (len.kind == Answer::Kind::number) {
      s.len = len.number;
    }
    Ask aa;
    aa.def = s.ang;
    const Answer ang = co_await c.angle(c.t("cmd.chamfer.ang"), std::move(aa));
    if (ang.kind == Answer::Kind::number) {
      s.ang = norm_angle(ang.number);
    }
    s.method = "A";
  } else if (o == "E") {
    Ask am;
    am.options = {c.opt("D", c.t("opt.distance")), c.opt("A", c.t("opt.angle"))};
    am.def = s.method;
    const Answer k = co_await c.keyword(c.t("cmd.chamfer.method"), std::move(am));
    s.method = k.key() == "A" ? "A" : "D";
  }
  save_chamfer(c, s);
}

Task<void> chamfer_cmd(Ctx& c) {
  if (!c.engine.state.get("chamfer").is_object()) {
    cad::ChamferSpec s;
    s.d2 = 0;
    save_chamfer(c, s);
  }
  c.host.set_selection({});
  bool multiple = false;
  const auto set_opts = [&c] {
    return std::vector<Option>{c.opt("D", c.t("opt.distance")), c.opt("A", c.t("opt.angle")),
                               c.opt("E", c.t("opt.method"))};
  };
  {
    const cad::ChamferSpec s = chamfer_spec(c);
    c.print(s.method == "A" ? c.t("cmd.chamfer.settings.angle",
                                  {{"mode", trim_label(c)}, {"len", fmt(s.len)}, {"ang", fmt(deg(s.ang))}})
                            : c.t("cmd.chamfer.settings",
                                  {{"mode", trim_label(c)}, {"d1", fmt(s.d1)}, {"d2", fmt(s.d2 ? *s.d2 : 0)}}));
  }
  const std::function<Task<void>(Ctx&, const std::string&)> ask_opt = chamfer_ask;
  for (;;) {
    std::vector<Option> first_opts = {c.opt("U", c.t("opt.undo")), c.opt("P", c.t("opt.polyline"))};
    for (Option& o : set_opts()) {
      first_opts.push_back(std::move(o));
    }
    first_opts.push_back(c.opt("T", c.t("opt.trim")));
    first_opts.push_back(c.opt("M", c.t("opt.multiple")));
    const Pick a = co_await corner_pick(c, c.t("cmd.chamfer.first"), std::move(first_opts), true);
    if (a.kind == Pick::Kind::nothing) {
      co_return;
    }
    if (a.kind == Pick::Kind::option) {
      if (a.option == "U") {
        undo_inside(c);
      } else if (a.option == "T") {
        co_await ask_trim_mode(c);
      } else if (a.option == "M") {
        multiple = true;
      } else if (a.option == "P") {
        co_await poly_corner_cmd(c, false, set_opts(), ask_opt, [] { return 0.0; }, [&c] { return chamfer_spec(c); });
        if (!multiple) {
          co_return;
        }
      } else {
        co_await chamfer_ask(c, a.option);
      }
      continue;
    }
    if (!a.target || a.target->prim.arc) {
      c.print(c.t("cmd.chamfer.err.type"));
      continue;
    }
    const cad::CornerTarget first = *a.target;
    IdSet sel;
    sel.add(id_of(first.e));
    c.host.set_selection(std::move(sel));
    corner_preview(c, [&c, first](const cad::CornerTarget& tb) {
      return cad::chamfer_pair(first, tb, chamfer_spec(c), corner_trim(c), c.doc.current);
    });
    Pick b;
    for (;;) {
      b = co_await corner_pick(c, c.t("cmd.chamfer.second"), set_opts(), false);
      if (b.kind == Pick::Kind::option) {
        co_await chamfer_ask(c, b.option);
        continue;
      }
      break;
    }
    c.preview(nullptr);
    c.host.set_selection({});
    if (b.kind == Pick::Kind::nothing) {  // Enter where an object is required: the JS command stops there
      co_return;
    }
    if (!b.target) {
      c.print(c.t("cmd.chamfer.err.type"));
      continue;
    }
    cad::ChamferSpec spec = chamfer_spec(c);
    if (b.shift) {
      spec = cad::ChamferSpec{};
      spec.d2 = 0;
    }
    const cad::CornerResult r = cad::chamfer_pair(first, *b.target, spec, corner_trim(c), c.doc.current);
    if (!apply_corner(c, false, r)) {
      continue;
    }
    if (!multiple) {
      co_return;
    }
  }
}

}  // namespace

void add_corner(std::vector<CommandDef>& t) {
  t.push_back(def("FILLET", {"F"}, fillet_cmd, "cmd.fillet.help"));
  t.push_back(def("CHAMFER", {"CHA"}, chamfer_cmd, "cmd.chamfer.help"));
}

}  // namespace cc::cmd::detail
