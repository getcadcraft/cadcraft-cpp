// SPDX-License-Identifier: Apache-2.0
// commands.js (CadCraft 0.5) in C++, M5.6a: sheets — LAYOUT (a new sheet with a title block and a viewport), MVIEW,
// VPORTS / SPLIT (panes in the model, MVIEW on a sheet), VPVIEW (what a viewport shows), VPLAYER (layers frozen in some
// viewports), MSPACE / PSPACE (working through a viewport), VIEWLABEL (a view title that follows its viewport's name).
// What only a drawing window can do (panes, entering a viewport) asks the host and says so when it cannot.
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/sheets/sheets.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

std::string text_of(const Answer& a) {
  if (a.kind == Answer::Kind::text) {
    return a.text;
  }
  return a.kind == Answer::Kind::typed ? base::js_string(a.typed) : std::string();
}
/// k?.option || k as a key
std::string key_of(const Answer& a) {
  return a.kind == Answer::Kind::option ? a.option : text_of(a);
}
std::string paper_unit(const doc::Layout& lay) {
  return base::js_string(lay.paper.get("unit"));
}
/// String(s).slice(0, n): the first n UTF-16 code units (a pair is never split in two: it goes whole or not at all)
std::string first_units(std::string_view s, std::size_t n) {
  std::size_t units = 0;
  std::size_t i = 0;
  while (i < s.size()) {
    const auto b = static_cast<unsigned char>(s[i]);
    std::size_t len = 4;
    if (b < 0x80) {
      len = 1;
    } else if (b < 0xE0) {
      len = 2;
    } else if (b < 0xF0) {
      len = 3;
    }
    const std::size_t u = len == 4 ? 2 : 1;
    if (units + u > n) {
      break;
    }
    units += u;
    i += len;
  }
  return std::string(s.substr(0, i));
}
bool is_viewport(const Value* e) {
  return e != nullptr && e->get("type") == Value("viewport");
}

/// Restores the open sheet when the command ends or is cancelled (the JS's finally).
class SpaceBack {
 public:
  SpaceBack(doc::Doc& d, std::string name) : d_(&d), name_(std::move(name)) {}
  ~SpaceBack() { d_->set_space(name_); }
  SpaceBack(const SpaceBack&) = delete;
  SpaceBack& operator=(const SpaceBack&) = delete;
  SpaceBack(SpaceBack&&) = delete;
  SpaceBack& operator=(SpaceBack&&) = delete;

 private:
  doc::Doc* d_;
  std::string name_;
};

// ---- LAYOUT

Task<void> layout_cmd(Ctx& c) {
  doc::Doc& d = c.doc;
  std::size_t n = d.layouts().size() + 1;
  const auto taken = [&d](const std::string& nm) {
    return std::ranges::any_of(d.layouts(), [&nm](const doc::Layout& l) { return l.name == nm; });
  };
  while (taken("Layout" + std::to_string(n))) {
    ++n;
  }
  const std::string name = "Layout" + std::to_string(n);
  const cc::sheets::Paper& p = cc::sheets::default_paper(d);
  const cc::sheets::SheetContents made = cc::sheets::sheet_contents(
      d, cc::sheets::make_paper(p.key),
      {.title = std::nullopt, .sheet_no = static_cast<int>(d.layouts().size()) + 1, .date_ms = c.host.now_ms()},
      c.words);
  for (const Value& l : made.layers) {
    if (d.layers().get(base::js_string(l.get("name"))) == nullptr) {
      (void)d.add_layer(l);
    }
  }
  doc::Layout layout{.name = name, .paper = made.paper, .entities = std::make_shared<doc::EntityMap>()};
  const doc::MapPtr map = layout.entities;
  d.change("LAYOUT", [&] {
    std::vector<doc::Layout> list = d.layouts();
    list.push_back(layout);
    d.set_layouts(std::move(list));
    for (const Value& e : made.entities) {
      (void)d.add(e, map);
    }
  });
  d.set_space(name);
  c.print(c.t("cmd.layout.made", {{"name", name}, {"paper", cc::sheets::paper_label(p, c.words)}}));
  co_return;
}

// ---- MVIEW

Task<void> mview_cmd(Ctx& c) {
  if (c.doc.layout() == nullptr) {
    c.print(c.t("cmd.mview.viewports_go_layout_open"));
    co_return;
  }
  const Pt a = (co_await c.point(c.t("cmd.mview.specify_corner_viewport"))).p;
  c.preview([a](Pt cur) { return base::Array{rect(a, cur)}; });
  Ask ab;
  ab.base = a;
  const Pt b = (co_await c.point(c.t("cmd.mtext.corner"), std::move(ab))).p;
  const double w = std::abs(b.x - a.x);
  const double h = std::abs(b.y - a.y);
  if (w < 1e-9 || h < 1e-9) {
    c.print(c.t("cmd.mview.viewport_needs_width_height"));
    co_return;
  }
  const doc::Layout* lay = c.doc.layout();
  const std::string unit = paper_unit(*lay);
  const cc::sheets::Aim aim = cc::sheets::aim_viewport(c.doc, unit, w, h, cc::sheets::model_extents(c.doc));
  if (c.doc.layers().get("Viewports") == nullptr) {
    (void)c.doc.add_layer(base::make_object({{"name", "Viewports"}, {"color", 8}, {"plot", false}}));
  }
  base::Array frz;  // layers frozen in new viewports (VP.2)
  c.doc.layers().each([&](const std::string& n, const Value& l) {
    if (l.get("newVpFrozen").truthy()) {
      frz.emplace_back(n);
    }
  });
  Value vp = base::make_object({{"type", "viewport"},
                                {"layer", "Viewports"},
                                {"color", nullptr},
                                {"x", (a.x + b.x) / 2},
                                {"y", (a.y + b.y) / 2},
                                {"w", w},
                                {"h", h},
                                {"cx", aim.cx},
                                {"cy", aim.cy},
                                {"s", aim.s}});
  if (!frz.empty()) {
    vp.set("frz", std::move(frz));
  }
  c.doc.change("MVIEW", [&] { (void)c.doc.add(vp); });
  c.print(c.t("cmd.mview.made", {{"scale", cc::sheets::scale_label(c.doc, unit, aim.s)}}));
}

// ---- VPORTS / SPLIT: panes in the drawing window (panes.js)

Task<void> split_panes(Ctx& c) {
  if (!c.host.can_split()) {
    c.print(c.t("cmd.vports.browser"));
    co_return;
  }
  const auto hidden = [](const char* k) { return Option{.key = k, .label = k, .en = {}, .hidden = true}; };
  Ask ak;
  ak.options = {c.opt("SI", c.t("opt.single")),
                c.opt("2", "2"),
                c.opt("3", "3"),
                c.opt("4", "4"),
                hidden("2V"),
                hidden("2H"),
                hidden("3R"),
                hidden("3L")};
  ak.def = "3";
  std::string key = base::js_to_upper(key_of(co_await c.keyword(c.t("cmd.vports.ask"), std::move(ak))));
  if (key == "2") {
    Ask a2;
    a2.options = {c.opt("H", c.t("opt.horizontal")), c.opt("V", c.t("opt.vertical"))};
    a2.def = "V";
    const std::string r = base::js_to_upper(key_of(co_await c.keyword(c.t("cmd.vports.two"), std::move(a2))));
    key = "2" + (r.empty() ? std::string("V") : r.substr(0, 1));
  } else if (key == "3") {
    Ask a3;
    a3.options = {c.opt("L", c.t("opt.left")), c.opt("R", c.t("opt.right"))};
    a3.def = "R";
    const std::string r = base::js_to_upper(key_of(co_await c.keyword(c.t("cmd.vports.three"), std::move(a3))));
    key = "3" + (r.empty() ? std::string("R") : r.substr(0, 1));
  }
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kLayouts{
      {{"SI", "single"}, {"2V", "2v"}, {"2H", "2h"}, {"3R", "3r"}, {"3L", "3l"}, {"4", "4"}}};
  // an iterator: a pointer with some standard libraries, a class with others (Emscripten's)
  // NOLINTNEXTLINE(readability-qualified-auto)
  const auto it = std::ranges::find_if(kLayouts, [&key](const auto& x) { return x.first == key; });
  if (it == kLayouts.end()) {
    c.print(c.t("cmd.vports.which"));
    co_return;
  }
  const std::string layout(it->second);
  c.host.set_panes(layout);
  if (layout == "single") {
    c.print(c.t("cmd.vports.single"));
  } else {
    const int n = layout[0] == '4' ? 4 : layout[0] - '0';
    c.print(c.t("cmd.vports.done", {{"n", std::to_string(n)}}));
  }
}
Task<void> vports_cmd(Ctx& c) {
  if (c.doc.layout() != nullptr) {
    co_await mview_cmd(c);
  } else {
    co_await split_panes(c);
  }
}

// ---- VPVIEW: what a viewport shows (a window on the model, or a named view)

/// The selected viewport, else one picked (Select a viewport until a viewport border is picked).
Task<Value> viewport_of(Ctx& c) {
  for (const Id id : c.host.selection()) {
    if (const Value* e = c.doc.entities().get(id); is_viewport(e)) {
      co_return *e;
    }
  }
  for (;;) {
    const Answer pick = co_await c.entity(c.t("cmd.vpview.select_viewport"));
    const Value* e = c.doc.entities().get(pick.id);
    if (is_viewport(e)) {
      co_return *e;
    }
    c.print(c.t("cmd.vpview.pick_viewport_border"));
  }
}

Task<void> vpview_cmd(Ctx& c) {
  if (c.doc.layout() == nullptr) {
    c.print(c.t("cmd.vpview.open_layout_viewport_first"));
    co_return;
  }
  const std::string lay_name = c.doc.layout()->name;
  const std::string unit = paper_unit(*c.doc.layout());
  const Value vp = co_await viewport_of(c);
  const Id vid = id_of(vp);
  if (vp.get("locked").truthy()) {
    c.print(c.t("cmd.vpview.locked"));
    co_return;
  }
  // WL.11: a named view of the model (option View at the first corner) sets the viewport's view and its name
  std::vector<Value> model_views;
  if (c.doc.views.is_array()) {
    for (const Value& v : c.doc.views.as_array()) {
      if (!v.get("layout").truthy()) {
        model_views.push_back(v);
      }
    }
  }
  c.host.set_selection({});
  std::optional<Value> named;
  std::optional<cad::Bounds> win;
  {
    c.doc.set_space();
    const SpaceBack back(c.doc, lay_name);
    c.print(c.t("cmd.vpview.pick"));
    Ask aa;
    if (!model_views.empty()) {
      aa.options = {c.opt("V", "View")};
    }
    const Answer a = co_await c.point(c.t("cmd.vpview.specify_first_corner_window"), std::move(aa));
    if (a.is("V")) {
      std::string list;
      for (const Value& v : model_views) {
        list += (list.empty() ? "" : ", ") + base::js_string(v.get("name"));
      }
      const std::string name(base::js_trim(text_of(co_await c.string(c.t("cmd.vpview.view_name", {{"list", list}})))));
      const std::string want = base::js_to_lower(name);
      for (const Value& v : model_views) {
        if (base::js_to_lower(base::js_string(v.get("name"))) == want) {
          named = v;
          break;
        }
      }
      if (!named) {
        c.print(c.t("cmd.nv.unknown", {{"name", name}, {"list", list}}));
        co_return;
      }
    } else {
      const Pt pa = a.p;
      c.preview([pa](Pt cur) { return base::Array{rect(pa, cur)}; });
      Ask ab;
      ab.base = pa;
      const Pt pb = (co_await c.point(c.t("cmd.mtext.corner"), std::move(ab))).p;
      win = cad::Bounds{.min_x = std::min(pa.x, pb.x),
                        .min_y = std::min(pa.y, pb.y),
                        .max_x = std::max(pa.x, pb.x),
                        .max_y = std::max(pa.y, pb.y)};
    }
  }
  const auto num = [](const Value& e, std::string_view k) { return base::js_to_number(e.get(k)); };
  if (named) {
    const double s = std::min(num(vp, "w") / num(*named, "w"), num(vp, "h") / num(*named, "h"));
    c.doc.change("VPVIEW", [&] {
      if (const Value* cur = c.doc.entities().get(vid)) {
        Value n = *cur;
        n.set("cx", named->get("cx"));
        n.set("cy", named->get("cy"));
        n.set("s", s);
        n.set("vname", named->get("name"));
        (void)c.doc.update(vid, n);
      }
    });
    c.print(c.t("cmd.vpview.view_done",
                {{"name", base::js_string(named->get("name"))}, {"scale", cc::sheets::scale_label(c.doc, unit, s)}}));
    co_return;
  }
  const cc::sheets::Aim aim = cc::sheets::aim_viewport(c.doc, unit, num(vp, "w"), num(vp, "h"), *win);
  c.doc.change("VPVIEW", [&] {
    if (const Value* cur = c.doc.entities().get(vid)) {
      Value n = *cur;
      n.set("cx", aim.cx);
      n.set("cy", aim.cy);
      n.set("s", aim.s);
      (void)c.doc.update(vid, n);
    }
  });
  c.print(c.t("cmd.vpview.done", {{"scale", cc::sheets::scale_label(c.doc, unit, aim.s)}}));
}

// ---- VPLAYER (VP.2): layers frozen in some viewports only

/// wildRe(pat): * and ? wildcards, any case, the whole name
bool wild_match(std::string_view pat, std::string_view name) {
  const std::string p = base::js_to_lower(base::js_trim(pat));
  const std::string s = base::js_to_lower(name);
  std::size_t pi = 0;
  std::size_t si = 0;
  std::size_t star = std::string::npos;
  std::size_t mark = 0;
  while (si < s.size()) {
    if (pi < p.size() && (p[pi] == '?' || p[pi] == s[si])) {
      ++pi;
      ++si;
    } else if (pi < p.size() && p[pi] == '*') {
      star = pi++;
      mark = si;
    } else if (star != std::string::npos) {
      pi = star + 1;
      si = ++mark;
    } else {
      return false;
    }
  }
  while (pi < p.size() && p[pi] == '*') {
    ++pi;
  }
  return pi == p.size();
}
/// layerNames(c, text): the layers the comma-separated patterns name, each once, in pattern order
std::vector<std::string> layer_names(const Ctx& c, std::string_view text) {
  std::vector<std::string> names;
  c.doc.layers().each([&](const std::string& n, const Value&) { names.push_back(n); });
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::string_view part =
        base::js_trim(text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
    if (!part.empty()) {
      for (const std::string& n : names) {
        if (wild_match(part, n) && std::ranges::find(out, n) == out.end()) {
          out.push_back(n);
        }
      }
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return out;
}

/// The word in VPLAYER's keys for an option: names_freeze / done.freeze …; `prompt`: Vpvisdflt asks names_dflt
std::string vplayer_word(const std::string& key, bool prompt) {
  if (key == "F") {
    return "freeze";
  }
  if (key == "T") {
    return "thaw";
  }
  if (key == "R") {
    return "reset";
  }
  return prompt ? "dflt" : "reset";
}

Task<void> vplayer_cmd(Ctx& c) {
  const std::optional<Value> ev = c.host.viewport_entered();
  doc::MapPtr map;
  if (ev) {
    for (const doc::Layout& l : c.doc.layouts()) {
      if (l.entities->get(id_of(*ev)) != nullptr) {
        map = l.entities;
      }
    }
  } else if (c.doc.layout() != nullptr) {
    map = c.doc.space();
  }
  if (!map) {
    c.print(c.t("cmd.vplayer.sheet_first"));
    co_return;
  }
  const auto all_vps = [&map] {
    std::vector<Value> out;
    map->each([&](const Id&, const Value& e) {
      if (is_viewport(&e)) {
        out.push_back(e);
      }
    });
    return out;
  };
  const auto current = [&]() -> std::optional<Value> {
    if (ev) {
      return ev;  // NOLINT(performance-no-automatic-move) — a copy: the command keeps the viewport it entered with
    }
    for (const Id id : c.host.selection()) {
      if (const Value* e = map->get(id); is_viewport(e)) {
        return *e;
      }
    }
    return std::nullopt;
  };
  for (;;) {
    Ask ak;
    ak.options = {c.opt("?", "?"), c.opt("F", "Freeze"), c.opt("T", "Thaw"), c.opt("R", "Reset"),
                  c.opt("V", "Vpvisdflt")};
    ak.none = true;
    const Answer k = co_await c.keyword(c.t("cmd.vplayer.ask"), std::move(ak));
    const std::string key = k.kind == Answer::Kind::option ? k.option : std::string();
    if (key.empty()) {
      co_return;
    }
    if (key == "?") {
      std::optional<Value> vp = current();
      if (!vp) {
        const std::vector<Value> vps = all_vps();
        if (!vps.empty()) {
          vp = vps.front();
        }
      }
      if (!vp) {
        c.print(c.t("cmd.vplayer.none"));
        continue;
      }
      const Value frz = vp->get("frz");
      if (frz.is_array() && !frz.as_array().empty()) {
        std::string list;
        for (const Value& n : frz.as_array()) {
          list += (list.empty() ? "" : ", ") + base::js_string(n);
        }
        c.print(c.t("cmd.vplayer.list", {{"list", list}}));
      } else {
        c.print(c.t("cmd.vplayer.list_none"));
      }
      continue;
    }
    const std::string ask = "cmd.vplayer.names_" + vplayer_word(key, true);
    const std::string raw = text_of(co_await c.string(c.t(ask)));
    const std::vector<std::string> names = layer_names(c, raw);
    if (names.empty()) {
      c.print(c.t("cmd.vplayer.no_layer", {{"text", first_units(raw, 60)}}));
      continue;
    }
    if (key == "V") {
      Ask af;
      af.options = {c.opt("F", "Frozen"), c.opt("T", "Thawed")};
      af.def = "Thawed";
      const Answer f = co_await c.keyword(c.t("cmd.vplayer.dflt"), std::move(af));
      const bool frozen = f.is("F");
      c.doc.change("VPLAYER", [&] {
        Value st = c.doc.layer_state();
        for (Value& l : st.find("list")->as_array()) {
          if (std::ranges::find(names, base::js_string(l.get("name"))) != names.end()) {
            if (frozen) {
              l.set("newVpFrozen", true);
            } else {
              l.erase("newVpFrozen");
            }
          }
        }
        c.doc.set_layer_state(st);
      });
      c.print(
          c.t(frozen ? "cmd.vplayer.dflt_frozen" : "cmd.vplayer.dflt_thawed", {{"n", std::to_string(names.size())}}));
      continue;
    }
    Ask aw;
    aw.options = {c.opt("A", "All"), c.opt("S", "Select"), c.opt("C", "Current")};
    aw.def = "Current";
    const Answer w = co_await c.keyword(c.t("cmd.vplayer.where"), std::move(aw));
    std::string wk = w.kind == Answer::Kind::option ? w.option : std::string();
    if (text_of(w) == "Current") {
      wk = "C";
    }
    std::vector<Value> vps;
    if (wk == "A") {
      vps = all_vps();
    } else if (wk == "S") {
      for (;;) {
        Ask ap;
        ap.none = true;
        const Answer pick = co_await c.entity(c.t("cmd.vplayer.pick"), std::move(ap));
        if (pick.kind != Answer::Kind::entity) {
          break;
        }
        const Value* e = map->get(pick.id);
        if (is_viewport(e)) {
          if (std::ranges::none_of(vps, [&](const Value& v) { return id_of(v) == pick.id; })) {
            vps.push_back(*e);
          }
        } else {
          c.print(c.t("cmd.vpview.pick_viewport_border"));
        }
      }
    } else if (const std::optional<Value> v = current()) {
      vps.push_back(*v);
    }
    if (vps.empty()) {
      c.print(c.t("cmd.vplayer.none"));
      continue;
    }
    c.doc.change("VPLAYER", [&] {
      for (const Value& vp0 : vps) {
        const Id vid = id_of(vp0);
        const Value* vp = map->get(vid);
        if (vp == nullptr) {
          continue;
        }
        base::Array frz;
        const Value old = vp->get("frz");
        if (old.is_array()) {
          for (const Value& n : old.as_array()) {
            const std::string up = base::js_to_upper(base::js_string(n));
            if (std::ranges::none_of(names, [&up](const std::string& x) { return base::js_to_upper(x) == up; })) {
              frz.push_back(n);
            }
          }
        }
        if (key == "F") {
          for (const std::string& n : names) {
            frz.emplace_back(n);
          }
        }
        if (key == "R") {
          for (const std::string& n : names) {
            if (const Value* l = c.doc.layers().get(n); l != nullptr && l->get("newVpFrozen").truthy()) {
              frz.emplace_back(n);
            }
          }
        }
        Value nv = *vp;
        const bool none = frz.empty();
        nv.set("frz", std::move(frz));
        (void)c.doc.update(vid, nv, map);
        if (none) {
          Value n2 = *map->get(vid);
          n2.erase("frz");
          (void)c.doc.update(vid, n2, map);
        }
      }
    });
    const std::string done = "cmd.vplayer.done." + vplayer_word(key, false);
    c.print(c.t(done, {{"n", std::to_string(names.size())}, {"v", std::to_string(vps.size())}}));
  }
}

// ---- MSPACE / PSPACE (VP.4)

Task<void> mspace_cmd(Ctx& c) {
  if (!c.host.can_enter_viewport()) {
    c.print(c.t("cmd.mspace.browser"));
    co_return;
  }
  if (c.host.viewport_entered()) {
    c.print(c.t("cmd.mspace.already"));
    co_return;
  }
  if (c.doc.layout() == nullptr) {
    c.print(c.t("cmd.mspace.sheet_first"));
    co_return;
  }
  const auto usable = [](const Value* e) { return is_viewport(e) && !(e->get("on") == Value(false)); };
  std::vector<Value> vps;
  c.doc.entities().each([&](const Id&, const Value& e) {
    if (usable(&e)) {
      vps.push_back(e);
    }
  });
  if (vps.empty()) {
    c.print(c.t("cmd.mspace.none"));
    co_return;
  }
  std::optional<Value> vp;
  for (const Id id : c.host.selection()) {
    if (const Value* e = c.doc.entities().get(id); usable(e)) {
      vp = *e;
      break;
    }
  }
  if (!vp && vps.size() == 1) {
    vp = vps.front();
  }
  while (!vp) {
    const Pt p = (co_await c.point(c.t("cmd.mspace.pick"))).p;
    std::optional<Value> at = c.host.viewport_at(p);
    if (at && usable(&*at)) {
      vp = std::move(at);
    } else {
      c.print(c.t("cmd.mspace.not_inside"));
    }
  }
  c.host.set_selection({});
  c.host.enter_viewport(*vp);
}
Task<void> pspace_cmd(Ctx& c) {
  if (!c.host.viewport_entered()) {
    c.print(c.t("cmd.pspace.not_inside"));
    co_return;
  }
  c.host.leave_viewport();
  c.print(c.t("cmd.pspace.done"));
}

// ---- VIEWLABEL (WL.11): a view title that shows its viewport's name as a live field

std::string view_label_text(const Ctx& c, const Value& vp) {
  return vp.get("vname").truthy() ? base::js_string(vp.get("vname")) : c.t("field.vpview.none");
}

Task<void> viewlabel_cmd(Ctx& c) {
  if (c.doc.layout() == nullptr) {
    c.print(c.t("cmd.viewlabel.sheet"));
    co_return;
  }
  const Value vp = co_await viewport_of(c);
  c.host.set_selection({});
  const Value th = c.doc.current.get("textHeight");
  const double h = th.truthy() ? base::js_to_number(th) : 2.5;
  const std::string s = view_label_text(c, vp);
  const auto label = [h](double x, double y, const std::string& str) {
    return base::make_object({{"type", "text"},
                              {"x", x},
                              {"y", y},
                              {"h", h},
                              {"rot", 0},
                              {"s", str},
                              {"ha", "left"},
                              {"va", "baseline"},
                              {"wf", 1}});
  };
  c.preview([label, txt = s](Pt cur) { return base::Array{label(cur.x, cur.y, txt)}; });
  Ask aw;
  aw.none = true;
  const Answer at = co_await c.point(c.t("cmd.viewlabel.where"), std::move(aw));
  const auto num = [&vp](std::string_view k) { return base::js_to_number(vp.get(k)); };
  const Pt p = at.kind == Answer::Kind::point ? at.p : Pt{num("x") - num("w") / 2, num("y") - num("h") / 2 - h * 1.6};
  Value e = label(p.x, p.y, s);
  e.set("field", base::make_object({{"k", "vpview"}, {"vp", static_cast<double>(id_of(vp))}}));
  (void)c.add(c.ent(e));
  if (!vp.get("vname").truthy()) {
    c.print(c.t("cmd.viewlabel.unnamed"));
  }
}

}  // namespace

/// The VIEWLABEL hook (commands.js Doc.editHooks): a viewport renamed or gone → its labels' text, in the same step.
void view_label_hook(doc::Doc& doc, const std::vector<doc::Op>& ops, const i18n::Catalog& words) {
  for (const doc::Op& op : ops) {
    if (op.kind != doc::Op::Kind::entity || !op.map) {
      continue;
    }
    const bool vp_before = op.before.get("type") == Value("viewport");
    const bool vp_after = op.after.get("type") == Value("viewport");
    if (!vp_before && !vp_after) {
      continue;
    }
    const auto vname = [](const Value& v) {
      const Value n = v.get("vname");
      return n.truthy() ? base::js_string(n) : std::string();
    };
    if (!op.after.is_undefined() && !op.before.is_undefined() && vname(op.after) == vname(op.before)) {
      continue;
    }
    std::vector<Value> texts;
    op.map->each([&](const Id&, const Value& e) {
      const Value f = e.get("field");
      if (e.get("type") == Value("text") && f.get("k") == Value("vpview") &&
          base::js_to_number(f.get("vp")) == static_cast<double>(op.id)) {
        texts.push_back(e);
      }
    });
    for (const Value& e : texts) {
      std::string s2 = base::js_string(e.get("s"));
      if (!op.after.is_undefined()) {
        s2 = op.after.get("vname").truthy() ? base::js_string(op.after.get("vname")) : words.t("field.vpview.none");
      }
      if (Value(s2) != e.get("s")) {
        Value n = e;
        n.set("s", s2);
        (void)doc.update(id_of(e), n, op.map);
      }
    }
  }
}

void add_layouts(std::vector<CommandDef>& t) {
  t.push_back(def("LAYOUT", {"LO", "NEWLAYOUT"}, layout_cmd, "cmd.layout.new_sheet_title_block", true));
  t.push_back(def("MVIEW", {"MV"}, mview_cmd, "cmd.mview.viewport_onto_model_layout"));
  CommandDef vports = def("VPORTS", {"VIEWPORTS"}, vports_cmd, "cmd.vports.help", true);
  vports.keep_selection = true;
  t.push_back(std::move(vports));
  CommandDef split = def("SPLIT", {}, split_panes, "cmd.split.help", true);
  split.keep_selection = true;
  t.push_back(std::move(split));
  t.push_back(def("VPVIEW", {}, vpview_cmd, "cmd.vpview.set_what_viewport_shows"));
}

void add_sheet_tools(std::vector<CommandDef>& t) {
  t.push_back(def("VPLAYER", {}, vplayer_cmd, "cmd.vplayer.help"));
  t.push_back(def("MSPACE", {"MS"}, mspace_cmd, "cmd.mspace.help", true));
  t.push_back(def("PSPACE", {"PS"}, pspace_cmd, "cmd.pspace.help", true));
  t.push_back(def("VIEWLABEL", {"VL"}, viewlabel_cmd, "cmd.viewlabel.help"));
}

}  // namespace cc::cmd::detail
