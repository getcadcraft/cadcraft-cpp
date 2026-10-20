// SPDX-License-Identifier: Apache-2.0
// The commands cadcraft.js registers through commands.js's registry when it loads (CadCraft 0.5), M5.8a: TEXTMASK
// (textmask-cmd.js, WL.8) and XCLIP (xclip.js, WL.16) — after every command of commands.js, in that order.
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/cad/edit.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

std::string key_of(const Answer& a) {
  if (a.kind == Answer::Kind::option) {
    return a.option;
  }
  if (a.kind == Answer::Kind::text) {
    return a.text;
  }
  return a.kind == Answer::Kind::typed ? base::js_string(a.typed) : std::string();
}
std::optional<double> number_of(const Answer& a) {
  if (a.kind == Answer::Kind::number) {
    return a.number;
  }
  if (a.kind == Answer::Kind::typed && a.typed.is_number()) {
    return a.typed.as_number();
  }
  return std::nullopt;
}

Task<void> textmask_cmd(Ctx& c) {
  const Answer set = co_await c.select(c.t("mask.select"));
  std::vector<Id> ids;
  for (const Id id : set.set) {
    if (const Value* e = c.doc.entities().get(id); e != nullptr && e->get("type") == Value("text")) {
      ids.push_back(id);
    }
  }
  if (ids.empty()) {
    c.print(c.t("mask.none"));
    co_return;
  }
  Ask ak;
  ak.options = {c.opt("B", c.t("opt.background")), c.opt("C", c.t("opt.color")), c.opt("O", c.t("opt.off"))};
  ak.def = "B";
  const std::string o = key_of(co_await c.keyword(c.t("mask.prompt"), std::move(ak)));
  if (o.empty()) {
    co_return;
  }
  std::optional<Value> bgm;
  if (o != "O") {
    Value color("bg");
    if (o == "C") {
      Ask an;
      an.def = 7;
      const std::optional<double> n = number_of(co_await c.number(c.t("mask.color"), std::move(an)));
      const bool usable = n && *n >= 1 && *n <= 255;
      if (!usable) {
        c.print(c.t("mask.badcolor"));
        co_return;
      }
      color = Value(base::js_round(*n));
    }
    Ask as;
    as.def = 1.5;
    const std::optional<double> sc = number_of(co_await c.number(c.t("mask.scale"), std::move(as)));
    const double scale = sc && *sc >= 1 && *sc <= 5 ? *sc : 1.5;
    bgm = base::make_object({{"color", color}, {"scale", scale}});
  }
  c.doc.change("TEXTMASK", [&] {
    for (const Id id : ids) {
      const Value* cur = c.doc.entities().get(id);
      if (cur == nullptr) {
        continue;
      }
      Value e = *cur;
      e.erase("bgm");
      if (bgm) {
        e.set("bgm", *bgm);
      }
      (void)c.doc.update(id, e);
    }
  });
  c.print(c.words.tn(bgm ? "mask.done" : "mask.off", static_cast<double>(ids.size()), {}));
}

Task<void> xclip_cmd(Ctx& c) {
  const Answer set = co_await c.select(c.t("xclip.select"));
  std::vector<Id> ids;
  for (const Id id : set.set) {
    const Value* e = c.doc.entities().get(id);
    if (e != nullptr && e->get("type") == Value("insert") &&
        c.doc.blocks().get(base::js_string(e->get("block"))) != nullptr) {
      ids.push_back(id);
    }
  }
  if (ids.empty()) {
    c.print(c.t("xclip.none"));
    co_return;
  }
  Ask ak;
  ak.options = {c.opt("N", c.t("opt.new")), c.opt("D", c.t("opt.delete"))};
  ak.def = "N";
  const std::string o = key_of(co_await c.keyword(c.t("xclip.prompt"), std::move(ak)));
  if (o.empty()) {
    co_return;
  }
  std::optional<std::vector<Pt>> world;
  if (o == "N") {
    Ask as;
    as.options = {c.opt("R", c.t("opt.rectangular")), c.opt("P", c.t("opt.polygonal"))};
    as.def = "R";
    const std::string sh = key_of(co_await c.keyword(c.t("xclip.shape"), std::move(as)));
    if (sh.empty()) {
      co_return;
    }
    if (sh == "R") {
      const Answer a = co_await c.point(c.t("xclip.corner1"));
      if (a.kind != Answer::Kind::point) {
        co_return;
      }
      Ask ab;
      ab.base = a.p;
      const Answer b = co_await c.point(c.t("xclip.corner2"), std::move(ab));
      if (b.kind != Answer::Kind::point) {
        co_return;
      }
      world = std::vector<Pt>{{a.p.x, a.p.y}, {b.p.x, a.p.y}, {b.p.x, b.p.y}, {a.p.x, b.p.y}};
    } else {
      std::vector<Pt> pts;
      for (;;) {
        Ask av;
        if (!pts.empty()) {
          av.base = pts.back();
        }
        av.none = pts.size() >= 3;
        const Answer q =
            co_await c.point(c.t(pts.size() < 3 ? "xclip.vertex" : "xclip.vertex_or_enter"), std::move(av));
        if (q.kind == Answer::Kind::nothing) {
          break;
        }
        if (q.kind != Answer::Kind::point) {
          co_return;
        }
        pts.push_back(q.p);
      }
      if (pts.size() < 3) {
        co_return;
      }
      world = std::move(pts);
    }
  }
  c.doc.change("XCLIP", [&] {
    for (const Id id : ids) {
      const Value* cur = c.doc.entities().get(id);
      if (cur == nullptr) {
        continue;
      }
      Value rest = *cur;
      rest.erase("clip");
      const Value* blk = c.doc.blocks().get(base::js_string(cur->get("block")));
      const cad::Mat2 m = blk != nullptr ? cad::insert_matrix(*cur, *blk) : cad::Mat2{1, 0, 0, 1, 0, 0};
      const double d = m[0] * m[3] - m[1] * m[2];
      if (!world || std::abs(d) < 1e-300) {
        (void)c.doc.update(id, rest);
        continue;
      }
      const cad::Mat2 inv{
          m[3] / d, -m[1] / d, -m[2] / d, m[0] / d, (m[2] * m[5] - m[3] * m[4]) / d, (m[1] * m[4] - m[0] * m[5]) / d};
      base::Array pts;
      for (const Pt p : *world) {
        pts.emplace_back(base::make_object(
            {{"x", inv[0] * p.x + inv[2] * p.y + inv[4]}, {"y", inv[1] * p.x + inv[3] * p.y + inv[5]}}));
      }
      rest.set("clip", base::make_object({{"pts", Value(std::move(pts))}}));
      (void)c.doc.update(id, rest);
    }
  });
  c.print(c.words.tn(world ? "xclip.done" : "xclip.deleted", static_cast<double>(ids.size()), {}));
}

}  // namespace

void add_registered(std::vector<CommandDef>& t) {
  t.push_back(def("TEXTMASK", {"TMASK"}, textmask_cmd, "mask.help"));
  t.push_back(def("XCLIP", {"XC"}, xclip_cmd, "xclip.help"));
}

}  // namespace cc::cmd::detail
