// SPDX-License-Identifier: Apache-2.0
// commands.js (CadCraft 0.5) in C++, M5.1d: HATCH (hatch.js patterns, loopsOf, boundaryAt).
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/save_parts.hpp"
#include "cc/docview/walk.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

/// hatch.js PATTERNS, in its order
constexpr std::array<const char*, 16> kPatterns{"ANSI31", "ANSI32", "ANSI33", "ANSI34", "ANSI35", "ANSI36",
                                                "ANSI37", "ANSI38", "LINE",   "NET",    "NET3",   "DASH",
                                                "SQUARE", "BRICK",  "DOTS",   "EARTH"};

std::string fmt(double n) {
  if (std::abs(n) < 1e-10) {
    return "0";
  }
  std::string s;
  base::append_number(s, base::js_number(base::js_to_fixed(n, 4)));
  return s;
}
/// String(v).trim().toUpperCase()
std::string trim_upper(const std::string& s) {
  const std::size_t a = s.find_first_not_of(" \t\n\r\f\v");
  if (a == std::string::npos) {
    return {};
  }
  const std::size_t b = s.find_last_not_of(" \t\n\r\f\v");
  return base::js_to_upper(s.substr(a, b - a + 1));
}

struct HatchState {
  std::string pattern = "ANSI31";
  double scale = 1;
  double angle = 0;
};
HatchState hatch_state(const Ctx& c) {
  HatchState s;
  const Value v = c.engine.state.get("hatch");
  if (v.is_object()) {
    s.pattern = base::js_string(v.get("pattern"));
    s.scale = base::js_to_number(v.get("scale"));
    s.angle = base::js_to_number(v.get("angle"));
  }
  return s;
}
void save_hatch(Ctx& c, const HatchState& s) {
  c.engine.state.set("hatch", base::make_object({{"pattern", s.pattern}, {"scale", s.scale}, {"angle", s.angle}}));
}

Task<void> hatch_cmd(Ctx& c) {
  HatchState s = hatch_state(c);
  save_hatch(c, s);
  const auto label = [&c, &s] {
    return s.pattern == "SOLID"
               ? c.t("cmd.hatch.solid")
               : s.pattern + ", " +
                     c.t("cmd.hatch.scaleangle", {{"scale", fmt(s.scale)}, {"angle", fmt(deg(s.angle))}});
  };
  c.print(c.t("cmd.hatch.current", {{"what", label()}}));
  std::vector<base::Array> picks;  // every area picked or object selected: one hatch at the end
  const auto make = [&c, &s, &picks] {
    base::Array loops;
    for (const base::Array& p : picks) {
      loops.insert(loops.end(), p.begin(), p.end());
    }
    const bool solid = s.pattern == "SOLID";
    Value h = c.ent(base::make_object({{"type", "hatch"}, {"loops", std::move(loops)}, {"solid", solid}}));
    if (!solid) {
      h.set("pattern", s.pattern);
      h.set("pscale", s.scale);
      h.set("pangle", s.angle);
      h.set("piso", cad::hatch_is_metric(Value::object(), c.doc.meta));
    }
    return h;
  };
  for (;;) {
    c.preview([&picks, make](Pt) { return picks.empty() ? base::Array{} : base::Array{make()}; });
    Ask ap;
    ap.none = true;
    ap.options = {c.opt("S", c.t("opt.selectobjects")), c.opt("P", c.t("opt.pattern")), c.opt("SC", c.t("opt.scale")),
                  c.opt("A", c.t("opt.angle"))};
    if (!picks.empty()) {
      ap.options.push_back(c.opt("U", c.t("opt.undo")));
    }
    const Answer p = co_await c.point(c.t("cmd.hatch.pick"), std::move(ap));
    if (!p.truthy()) {
      break;
    }
    if (p.is("U")) {
      picks.pop_back();
      continue;
    }
    if (p.is("P")) {
      for (;;) {
        Ask as;
        as.def = s.pattern;
        const Answer a = co_await c.string(c.t("cmd.hatch.pattern"), std::move(as));
        const std::string v = trim_upper(a.kind == Answer::Kind::text ? a.text : base::js_string(a.typed));
        if (v.empty()) {
          break;
        }
        if (v == "?") {
          std::string list = std::string("SOLID — ") + c.t("cmd.hatch.solid");
          for (const char* n : kPatterns) {
            list += std::string("\n") + n + " — " + c.t("hatch.pat." + base::js_to_lower(n));
          }
          c.print(list);
          continue;
        }
        if (v == "SOLID" || v == "S" || cad::hatch_pattern(v) != nullptr) {
          s.pattern = v == "S" ? "SOLID" : v;
          save_hatch(c, s);
          break;
        }
        c.print(c.t("cmd.hatch.unknown", {{"name", v}}));
      }
      continue;
    }
    if (p.is("SC")) {
      Ask an;
      an.def = s.scale;
      const Answer v = co_await c.number(c.t("cmd.hatch.scale"), std::move(an));
      if (v.kind == Answer::Kind::number && v.number > 0) {
        s.scale = v.number;
        save_hatch(c, s);
      }
      continue;
    }
    if (p.is("A")) {
      Ask aa;
      aa.def = s.angle;
      aa.ucs = true;
      const Answer v = co_await c.angle(c.t("cmd.hatch.angle"), std::move(aa));
      s.angle = norm_angle(v.kind == Answer::Kind::number && v.number != 0 && !std::isnan(v.number) ? v.number : 0);
      save_hatch(c, s);
      continue;
    }
    if (p.is("S")) {
      c.preview(nullptr);
      Ask asel;
      asel.fresh = true;
      const Answer ids = co_await c.select(c.t("cmd.hatch.select"), std::move(asel));
      base::Array loops;
      std::size_t skipped = 0;
      for (const Id id : ids.set) {
        const Value* e = c.doc.entities().get(id);
        if (const auto l = e != nullptr ? cad::loops_of(*e) : std::nullopt) {
          loops.insert(loops.end(), l->begin(), l->end());
        } else {
          ++skipped;
        }
      }
      if (skipped != 0) {
        c.print(c.words.tn("cmd.hatch.notclosed", static_cast<double>(skipped)));
      }
      if (!loops.empty()) {
        picks.push_back(std::move(loops));
      }
      continue;
    }
    std::vector<const Value*> all;
    c.doc.entities().each([&](const Id&, const Value& e) {
      if (c.doc.visible(e)) {
        all.push_back(&e);
      }
    });
    const cad::Boundary r = cad::boundary_at(
        all, {p.p.x, p.p.y}, [&c](const std::string& n) { return c.doc.blocks().get(n); }, std::nullopt);
    if (!r.error.empty()) {
      c.print(c.t("cmd.hatch.err." + r.error));
      continue;
    }
    picks.push_back(r.loops);
  }
  c.preview(nullptr);
  if (picks.empty()) {
    co_return;
  }
  const Value h = make();
  if (!h.get("solid").truthy() && !docview::hatch_lines(h, c.doc)) {
    c.print(c.t("cmd.hatch.dense"));
  }
  c.doc.change("HATCH", [&] { (void)c.doc.add(h); });
}

}  // namespace

void add_hatch(std::vector<CommandDef>& t) {
  t.push_back(def("HATCH", {"H", "BH", "BHATCH", "-HATCH"}, hatch_cmd, "cmd.hatch.help"));
}

}  // namespace cc::cmd::detail
