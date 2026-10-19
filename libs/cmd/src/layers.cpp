// SPDX-License-Identifier: Apache-2.0
// layertools.js (CadCraft 0.5) in C++, M5.5a: LAYISO, LAYUNISO, LAYOFF, LAYFRZ, LAYMCUR, LAYON, LAYTHW, LAYMRG, LAYDEL,
// RENAME, COUNT, LINETYPE, LTSCALE, LWEIGHT; XATTACH / XREF / PUBLISH / SHEETSET say what they need without a host that
// opens files or saves downloads (XREF ? lists the linked drawings a DWG brought).
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

std::string text_of(const Answer& a) {
  if (a.kind == Answer::Kind::text) {
    return a.text;
  }
  return a.kind == Answer::Kind::typed ? base::js_string(a.typed) : std::string();
}
std::string type_of(const Value& e) {
  return base::js_string(e.get("type"));
}
/// listOf(names): the first 8, then …
std::string list_of(const std::vector<std::string>& names) {
  std::string s;
  for (std::size_t i = 0; i < names.size() && i < 8; ++i) {
    s += (i != 0 ? ", " : "") + names[i];
  }
  return names.size() > 8 ? s + " …" : s;
}
/// layerOf(doc, id): the layer the object is on, or nothing
std::optional<std::string> layer_of(const Ctx& c, Id id) {
  const Value* e = c.doc.entities().get(id);
  if (e == nullptr) {
    return std::nullopt;
  }
  const Value l = e->get("layer");
  if (l.is_undefined() || l.is_null()) {
    return std::nullopt;
  }
  return base::js_string(l);
}
std::string current_layer(const Ctx& c) {
  return base::js_string(c.doc.current.get("layer"));
}

/// layertools.js changeLayers(doc, label, edit): one undo step for a changed copy of the layer table; none when
/// nothing changed
template <class F>
bool change_layers(Ctx& c, const std::string& label, const F& edit) {
  Value state = c.doc.layer_state();
  const Value before = state;
  edit(state);
  if (state == before) {
    return false;
  }
  c.doc.change(label, [&] { c.doc.set_layer_state(state); });
  return true;
}
/// a layer in a layer-state's list
Value* layer_in(Value& state, const std::string& name) {
  for (Value& l : state.find("list")->as_array()) {
    if (base::js_string(l.get("name")) == name) {
      return &l;
    }
  }
  return nullptr;
}

Task<void> layiso_cmd(Ctx& c) {
  const Answer ids = co_await c.select(c.t("cmd.layiso.select"));
  if (ids.kind != Answer::Kind::set || ids.set.empty()) {
    co_return;
  }
  std::vector<std::string> names;
  for (const Id id : ids.set) {
    if (const auto n = layer_of(c, id); n && std::ranges::find(names, *n) == names.end()) {
      names.push_back(*n);
    }
  }
  std::vector<std::string> off;
  (void)change_layers(c, "LAYISO", [&](Value& s) {
    for (Value& l : s.find("list")->as_array()) {
      const std::string n = base::js_string(l.get("name"));
      if (std::ranges::find(names, n) != names.end()) {
        l.set("on", true);
        continue;
      }
      if (l.get("on").truthy()) {
        l.set("on", false);
        off.push_back(n);
      }
    }
    const std::string was = base::js_string(s.get("current"));
    if (std::ranges::find(names, was) == names.end()) {
      s.set("current", names.empty() ? Value() : Value(names[0]));
    }
    if (Value* target = layer_in(s, base::js_string(s.get("current")))) {
      target->set("frozen", false);
    }
    base::Array offv;
    for (const std::string& n : off) {
      offv.emplace_back(n);
    }
    c.doc.lay_iso =
        base::make_object({{"off", std::move(offv)},
                           {"current", was != base::js_string(s.get("current")) ? Value(was) : Value(nullptr)}});
  });
  c.print(c.words.tn("cmd.layiso.done", static_cast<double>(names.size()),
                     {{"list", list_of(names)}, {"off", std::to_string(off.size())}}));
}

Task<void> layuniso_cmd(Ctx& c) {
  const Value iso = c.doc.lay_iso;
  if (!iso.is_object()) {
    c.print(c.t("cmd.layuniso.none"));
    co_return;
  }
  c.doc.lay_iso = Value(nullptr);
  std::size_t n = 0;
  (void)change_layers(c, "LAYUNISO", [&](Value& s) {
    const Value offv = iso.get("off");  // get() copies: keep it alive while `off` refers into it
    const base::Array& off = offv.as_array();
    for (Value& l : s.find("list")->as_array()) {
      if (std::ranges::find(off, l.get("name")) != off.end() && !l.get("on").truthy()) {
        l.set("on", true);
        ++n;
      }
    }
    const Value cur = iso.get("current");
    if (cur.truthy()) {
      if (const Value* back = layer_in(s, base::js_string(cur));
          back != nullptr && back->get("on").truthy() && !back->get("frozen").truthy()) {
        s.set("current", back->get("name"));
      }
    }
  });
  c.print(c.words.tn("cmd.layuniso.done", static_cast<double>(n)));
}

/// LAYOFF / LAYFRZ: pick objects one by one; each pick is its own undo step, and U takes the last one back
Task<void> by_pick(Ctx& c, const char* name, const char* key, const char* label) {
  struct Done {
    std::string layer;
    std::size_t mark;
  };
  std::vector<Done> done;
  const std::string base_key = std::string("cmd.") + label;
  for (;;) {
    Ask a;
    a.none = true;
    if (!done.empty()) {
      a.options = {c.opt("U", c.t("opt.undo"))};
    }
    const Answer pick = co_await c.entity(c.t(base_key + ".pick"), std::move(a));
    if (!pick.truthy()) {
      co_return;
    }
    if (pick.is("U")) {
      if (!done.empty()) {
        const Done last = done.back();
        done.pop_back();
        if (c.doc.mark() == last.mark) {  // the last undo step is still that pick's
          (void)c.doc.undo();
          c.print(c.t("cmd.layer.undone", {{"layer", last.layer}}));
        }
      }
      continue;
    }
    const std::optional<std::string> layer = layer_of(c, pick.id);
    if (!layer) {
      continue;
    }
    if (*layer == current_layer(c)) {
      c.print(c.t(base_key + ".current", {{"layer", *layer}}));
      continue;
    }
    const std::string k = key;
    const bool changed = change_layers(c, name, [&](Value& s) {
      if (Value* l = layer_in(s, *layer)) {
        l->set(k, k != "on");
      }
    });
    if (!changed) {
      c.print(c.t(base_key + ".already", {{"layer", *layer}}));
      continue;
    }
    done.push_back({*layer, c.doc.mark()});
    c.host.set_selection({});
    c.print(c.t(base_key + ".done", {{"layer", *layer}}));
  }
}
Task<void> layoff_cmd(Ctx& c) {
  co_await by_pick(c, "LAYOFF", "on", "layoff");
}
Task<void> layfrz_cmd(Ctx& c) {
  co_await by_pick(c, "LAYFRZ", "frozen", "layfrz");
}

Task<void> laymcur_cmd(Ctx& c) {
  std::vector<Id> pre;
  for (const Id id : c.host.selection()) {
    if (c.doc.entities().get(id) != nullptr) {
      pre.push_back(id);
    }
  }
  Id id = 0;
  if (pre.size() == 1) {
    id = pre[0];
  } else {
    Ask a;
    a.none = true;
    const Answer p = co_await c.entity(c.t("cmd.laymcur.pick"), std::move(a));
    id = p.kind == Answer::Kind::entity ? p.id : 0;
  }
  const std::optional<std::string> layer = id != 0 ? layer_of(c, id) : std::nullopt;
  if (!layer) {
    co_return;
  }
  (void)change_layers(c, "LAYMCUR", [&](Value& s) {
    if (Value* l = layer_in(s, *layer)) {
      l->set("on", true);
      l->set("frozen", false);
      s.set("current", *layer);
    }
  });
  c.print(c.t("cmd.laymcur.done", {{"layer", *layer}}));
}

Task<void> all_layers(Ctx& c, const char* name, const char* key, const char* label) {
  const bool on = std::string(key) == "on";
  std::size_t n = 0;
  (void)change_layers(c, name, [&](Value& s) {
    for (Value& l : s.find("list")->as_array()) {
      if (on ? !l.get("on").truthy() : l.get("frozen").truthy()) {
        l.set(key, on);
        ++n;
      }
    }
  });
  if (on) {
    c.doc.lay_iso = Value(nullptr);
  }
  c.print(c.words.tn(std::string("cmd.") + label + ".done", static_cast<double>(n)));
  co_return;
}
Task<void> layon_cmd(Ctx& c) {
  co_await all_layers(c, "LAYON", "on", "layon");
}
Task<void> laythw_cmd(Ctx& c) {
  co_await all_layers(c, "LAYTHW", "frozen", "laythw");
}

/// layertools.js forEachObject(doc, fn): every object in the model, on every sheet and in every block, with put / drop
void for_each_object(
    Ctx& c,
    const std::function<void(const Value&, const std::function<void(Value)>&, const std::function<void()>&)>& fn) {
  std::vector<doc::MapPtr> maps{c.doc.model()};
  for (const doc::Layout& l : c.doc.layouts()) {
    maps.push_back(l.entities);
  }
  for (const doc::MapPtr& m : maps) {
    std::vector<Value> all;
    m->each([&](const Id&, const Value& e) { all.push_back(e); });
    for (const Value& e : all) {
      const Id id = id_of(e);
      fn(e, [&](const Value& next) { (void)c.doc.update(id, next, m); }, [&] { c.doc.remove(id, m); });
    }
  }
  std::vector<Value> blocks;
  c.doc.blocks().each([&](const std::string&, const Value& b) { blocks.push_back(b); });
  for (const Value& b : blocks) {
    bool changed = false;
    base::Array ents;
    const Value src = b.get("entities");
    for (const Value& e : src.is_array() ? src.as_array() : base::Array{}) {
      Value keep = e;
      bool gone = false;
      fn(
          e,
          [&](Value next) {
            keep = std::move(next);
            changed = true;
          },
          [&] {
            gone = true;
            changed = true;
          });
      if (!gone) {
        ents.push_back(std::move(keep));
      }
    }
    if (changed) {
      Value nb = b;
      nb.set("entities", std::move(ents));
      (void)c.doc.define_block(nb);
    }
  }
}

/// layertools.js layerByPick(c, prompt): a layer by an object picked on it, or N and its name; nothing on Enter
Task<std::optional<std::string>> layer_by_pick(Ctx& c, std::string prompt) {
  for (;;) {
    Ask a;
    a.none = true;
    a.options = {c.opt("N", c.t("opt.name"))};
    const Answer p = co_await c.entity(prompt, std::move(a));
    if (!p.truthy()) {
      co_return std::nullopt;
    }
    if (p.is("N")) {
      const std::string s(base::js_trim(text_of(co_await c.string(c.t("cmd.layer.name")))));
      if (s.empty()) {
        continue;
      }
      std::optional<std::string> hit;
      c.doc.layers().each([&](const std::string& n, const Value&) {
        if (!hit && base::js_to_upper(n) == base::js_to_upper(s)) {
          hit = n;
        }
      });
      if (hit) {
        co_return hit;
      }
      c.print(c.t("cmd.layer.none", {{"layer", s}}));
      continue;
    }
    if (auto n = layer_of(c, p.id)) {
      co_return n;
    }
  }
}

Task<bool> yes_no(Ctx& c, std::string prompt) {
  Ask a;
  a.options = {c.opt("Y", c.t("opt.yes")), c.opt("N", c.t("opt.no"))};
  a.def = "N";
  const Answer ok = co_await c.keyword(std::move(prompt), std::move(a));
  co_return ok.key() == "Y";
}

Task<void> laymrg_cmd(Ctx& c) {
  std::vector<std::string> from;
  for (;;) {
    const auto n = co_await layer_by_pick(
        c, from.empty() ? c.t("cmd.laymrg.pick") : c.t("cmd.laymrg.more", {{"list", list_of(from)}}));
    if (!n) {
      break;
    }
    if (std::ranges::find(from, *n) == from.end()) {
      from.push_back(*n);
    }
  }
  if (from.empty()) {
    co_return;
  }
  const auto to = co_await layer_by_pick(c, c.t("cmd.laymrg.target"));
  if (!to) {
    co_return;
  }
  std::vector<std::string> src;
  for (const std::string& n : from) {
    if (n != *to) {
      src.push_back(n);
    }
  }
  if (!co_await yes_no(c, c.words.tn("cmd.laymrg.confirm", static_cast<double>(src.size()),
                                     {{"list", list_of(src)}, {"to", *to}}))) {
    c.print(c.t("cmd.layer.nothing"));
    co_return;
  }
  // mergeLayers(doc, from, to)
  if (src.empty()) {
    c.print(c.t("cmd.laymrg.same"));
    co_return;
  }
  if (!c.doc.layers().has(*to)) {
    c.print(c.t("cmd.layer.none", {{"layer", *to}}));
    co_return;
  }
  if (std::ranges::find(src, "0") != src.end()) {
    c.print(c.t("cmd.laymrg.zero"));
    co_return;
  }
  std::size_t moved = 0;
  c.doc.change("LAYMRG", [&] {
    for_each_object(c, [&](const Value& e, const std::function<void(Value)>& put, const std::function<void()>&) {
      if (std::ranges::find(src, base::js_string(e.get("layer"))) != src.end() && e.get("layer").is_string()) {
        Value n = e;
        n.set("layer", *to);
        put(std::move(n));
        ++moved;
      }
    });
    Value s = c.doc.layer_state();
    base::Array kept;
    const Value list = s.get("list");
    for (const Value& l : list.as_array()) {
      if (std::ranges::find(src, base::js_string(l.get("name"))) == src.end()) {
        kept.push_back(l);
      }
    }
    s.set("list", std::move(kept));
    if (std::ranges::find(src, base::js_string(s.get("current"))) != src.end()) {
      s.set("current", *to);
    }
    c.doc.set_layer_state(s);
  });
  c.print(
      c.words.tn("cmd.laymrg.done", static_cast<double>(moved), {{"layers", std::to_string(src.size())}, {"to", *to}}));
}

Task<void> laydel_cmd(Ctx& c) {
  const auto n = co_await layer_by_pick(c, c.t("cmd.laydel.pick"));
  if (!n) {
    co_return;
  }
  std::size_t count = 0;
  c.doc.each_entity([&](const Value& e) {
    if (e.get("layer") == Value(*n)) {
      ++count;
    }
  });
  c.doc.blocks().each([&](const std::string&, const Value& b) {
    const Value ents = b.get("entities");
    for (const Value& e : ents.is_array() ? ents.as_array() : base::Array{}) {
      if (e.get("layer") == Value(*n)) {
        ++count;
      }
    }
  });
  if (!co_await yes_no(c, c.words.tn("cmd.laydel.confirm", static_cast<double>(count), {{"layer", *n}}))) {
    c.print(c.t("cmd.layer.nothing"));
    co_return;
  }
  // deleteLayer(doc, name)
  if (*n == "0" || base::js_to_lower(*n) == "defpoints") {
    c.print(c.t("cmd.laydel.fixed", {{"layer", *n}}));
    co_return;
  }
  if (*n == current_layer(c)) {
    c.print(c.t("cmd.laydel.current", {{"layer", *n}}));
    co_return;
  }
  if (!c.doc.layers().has(*n)) {
    c.print(c.t("cmd.layer.none", {{"layer", *n}}));
    co_return;
  }
  std::size_t erased = 0;
  c.doc.change("LAYDEL", [&] {
    for_each_object(c, [&](const Value& e, const std::function<void(Value)>&, const std::function<void()>& drop) {
      if (e.get("layer") == Value(*n)) {
        drop();
        ++erased;
      }
    });
    Value s = c.doc.layer_state();
    base::Array kept;
    const Value list = s.get("list");
    for (const Value& l : list.as_array()) {
      if (base::js_string(l.get("name")) != *n) {
        kept.push_back(l);
      }
    }
    s.set("list", std::move(kept));
    c.doc.set_layer_state(s);
  });
  c.print(c.words.tn("cmd.laydel.done", static_cast<double>(erased), {{"layer", *n}}));
}

Task<void> rename_cmd(Ctx& c) {
  Ask ak;
  ak.options = {c.opt("LA", c.t("opt.layer")), c.opt("B", c.t("opt.block"))};
  const Answer k = co_await c.keyword(c.t("cmd.rename.type"), std::move(ak));
  if (k.kind != Answer::Kind::option) {
    co_return;
  }
  const bool layer = k.option == "LA";
  std::vector<std::string> names;
  if (layer) {
    c.doc.layers().each([&](const std::string& n, const Value&) { names.push_back(n); });
  } else {
    c.doc.blocks().each([&](const std::string& n, const Value&) {
      if (n.empty() || n[0] != '*') {
        names.push_back(n);
      }
    });
  }
  const std::string old_s(base::js_trim(text_of(co_await c.string(c.t("cmd.rename.old")))));
  const auto hit = std::ranges::find_if(
      names, [&](const std::string& n) { return base::js_to_upper(n) == base::js_to_upper(old_s); });
  if (hit == names.end()) {
    if (!old_s.empty()) {
      c.print(layer ? c.t("cmd.layer.none", {{"layer", old_s}}) : c.t("cmd.rename.noblock", {{"name", old_s}}));
    }
    co_return;
  }
  const std::string& from = *hit;  // `names` is the command's own list: nothing edits it
  const std::string to(base::js_trim(text_of(co_await c.string(c.t("cmd.rename.new", {{"old", from}})))));
  if (to.empty() || to == from) {
    co_return;
  }
  if (layer) {
    if (from == "0") {
      c.print(c.t("cmd.rename.zero"));
      co_return;
    }
    if (std::ranges::any_of(
            names, [&](const std::string& n) { return base::js_to_upper(n) == base::js_to_upper(to) && n != from; })) {
      c.print(c.t("cmd.rename.taken", {{"name", to}}));
      co_return;
    }
    c.doc.set_layer(from, base::make_object({{"name", to}}));
    c.print(c.t("cmd.rename.done", {{"old", from}, {"name", to}}));
    co_return;
  }
  // renameBlock(doc, from, to)
  if (c.doc.blocks().has(to) || to[0] == '*') {
    c.print(c.t("cmd.rename.taken", {{"name", to}}));
    co_return;
  }
  const Value b = *c.doc.blocks().get(from);
  c.doc.change("RENAME", [&] {
    Value nb = b;
    nb.set("name", to);
    (void)c.doc.define_block(nb);
    for_each_object(c, [&](const Value& e, const std::function<void(Value)>& put, const std::function<void()>&) {
      if (e.get("block") == Value(from)) {
        Value n = e;
        n.set("block", to);
        put(std::move(n));
      }
    });
    c.doc.remove_block(from);
  });
  c.print(c.t("cmd.rename.done", {{"old", from}, {"name", to}}));
}

// ---- linked drawings, sheets, counts

/// xref.js xrefs(doc): the linked drawings (root blocks), by name
struct Link {
  std::string name, path;
  bool loaded = false;
  std::size_t refs = 0, objects = 0;
};
std::vector<Link> xrefs(const Ctx& c) {
  std::vector<Link> out;
  c.doc.blocks().each([&](const std::string& n, const Value& b) {
    const Value x = b.get("xref");
    if (!x.truthy() || !x.get("of").is_undefined()) {
      return;
    }
    Link l;
    l.name = n;
    l.path = x.get("path").truthy() ? base::js_string(x.get("path")) : "";
    const Value ents = b.get("entities");
    l.objects = ents.is_array() ? ents.as_array().size() : 0;
    l.loaded = !(x.get("loaded") == Value(false)) && l.objects > 0;
    c.doc.each_entity([&](const Value& e) {
      if (base::js_string(e.get("type")) == "insert" && e.get("block") == Value(n)) {
        ++l.refs;
      }
    });
    out.push_back(std::move(l));
  });
  std::ranges::sort(out,
                    [](const Link& a, const Link& b) { return base::js_locale_compare_numeric(a.name, b.name) < 0; });
  return out;
}

Task<void> xattach_cmd(Ctx& c) {
  c.print(c.t("xref.nobrowser"));
  co_return;
}

Task<void> xref_cmd(Ctx& c) {
  Ask ak;
  ak.options = {c.opt("?", "?"), c.opt("A", c.t("opt.attach")), c.opt("R", c.t("opt.reload")),
                c.opt("B", c.t("opt.bind")), c.opt("D", c.t("opt.detach"))};
  ak.def = "?";
  const Answer k = co_await c.keyword(c.t("xref.prompt"), std::move(ak));
  const std::string o = k.key();
  if (o.empty()) {
    co_return;
  }
  const std::vector<Link> links = xrefs(c);
  if (o == "?") {
    if (links.empty()) {
      c.print(c.t("xref.none"));
      co_return;
    }
    for (const Link& l : links) {
      c.print("  " + c.words.tn(l.loaded ? "xref.row.loaded" : "xref.row.missing", static_cast<double>(l.objects),
                                {{"name", l.name}, {"path", l.path}, {"refs", std::to_string(l.refs)}}));
    }
    co_return;
  }
  if (o == "A" || links.empty()) {
    c.print(c.t(o == "A" ? "xref.nobrowser" : "xref.none"));
    co_return;
  }
  // reloading needs another drawing; binding and detaching are not ported yet (docs/gaps.md)
  c.print(c.t("xref.nobrowser"));
}

Task<void> sheetset_cmd(Ctx& c) {
  c.print(c.t("sheetset.cmd.browser"));
  co_return;
}

Task<void> publish_cmd(Ctx& c) {
  const std::vector<doc::Layout>& sheets = c.doc.layouts();
  if (sheets.empty()) {
    c.print(c.t("publish.nosheets"));
    co_return;
  }
  Ask ak;
  ak.options = {c.opt("A", c.t("opt.all")), c.opt("S", c.t("opt.some"))};
  ak.def = "A";
  const Answer k = co_await c.keyword(c.words.tn("publish.which", static_cast<double>(sheets.size())), std::move(ak));
  const std::string o = k.key();
  if (o.empty()) {
    co_return;
  }
  if (o == "S") {
    const std::string s = text_of(co_await c.string(c.t("publish.names")));
    std::vector<std::string> want;
    for (std::size_t from = 0; from <= s.size();) {
      const std::size_t at = s.find(',', from);
      const std::string part =
          base::js_to_upper(base::js_trim(s.substr(from, at == std::string::npos ? std::string::npos : at - from)));
      if (!part.empty()) {
        want.push_back(part);
      }
      if (at == std::string::npos) {
        break;
      }
      from = at + 1;
    }
    std::size_t picked = 0;
    std::vector<std::string> missing;
    for (const doc::Layout& l : sheets) {
      if (std::ranges::find(want, base::js_to_upper(l.name)) != want.end()) {
        ++picked;
      }
    }
    for (const std::string& w : want) {
      if (std::ranges::none_of(sheets, [&w](const doc::Layout& l) { return base::js_to_upper(l.name) == w; })) {
        missing.push_back(w);
      }
    }
    if (!missing.empty()) {
      std::string list;
      for (const std::string& m : missing) {
        list += (list.empty() ? "" : ", ") + m;
      }
      c.print(c.t("publish.unknown", {{"list", list}}));
    }
    if (picked == 0) {
      co_return;
    }
  }
  c.print(c.t("count.nodownload"));  // the PDF goes out as a download: not this host (docs/gaps.md)
}

/// count.js countBlocks(doc, entities): [{ block, n, ids }] most first, then by name
struct CountRow {
  std::string block;
  std::size_t n = 0;
  std::vector<Id> ids;
};
std::vector<CountRow> count_blocks(const Ctx& c) {
  std::vector<CountRow> rows;
  c.doc.entities().each([&](const Id& id, const Value& e) {
    const std::string b = base::js_string(e.get("block"));
    if (type_of(e) != "insert" || !e.get("block").truthy() || b[0] == '*') {
      return;
    }
    auto it = std::ranges::find_if(rows, [&b](const CountRow& r) { return r.block == b; });
    if (it == rows.end()) {
      rows.push_back({b, 0, {}});
      it = rows.end() - 1;
    }
    ++it->n;
    it->ids.push_back(id);
  });
  std::ranges::stable_sort(rows, [](const CountRow& a, const CountRow& b) {
    if (a.n != b.n) {
      return a.n > b.n;
    }
    return base::js_locale_compare_numeric(a.block, b.block) < 0;
  });
  return rows;
}

Task<void> count_cmd(Ctx& c) {
  const auto list = [&c] {
    const std::vector<CountRow> rows = count_blocks(c);
    if (rows.empty()) {
      c.print(c.t("count.none"));
      return;
    }
    std::size_t total = 0;
    for (const CountRow& r : rows) {
      total += r.n;
    }
    c.print(c.words.tn("count.list", static_cast<double>(total), {{"blocks", std::to_string(rows.size())}}));
    for (std::size_t i = 0; i < rows.size() && i < 40; ++i) {
      c.print("  " + rows[i].block + ": " + std::to_string(rows[i].n));
    }
    if (rows.size() > 40) {
      c.print("  " + c.t("count.more", {{"n", std::to_string(rows.size() - 40)}}));
    }
  };
  for (;;) {
    Ask a;
    a.none = true;
    a.options = {c.opt("A", c.t("opt.all")), c.opt("E", c.t("opt.export"))};
    const Answer p = co_await c.entity(c.t("count.prompt"), std::move(a));
    if (!p.truthy() || p.is("A")) {
      list();
      co_return;
    }
    if (p.is("E")) {
      Ask ak;
      ak.options = {c.opt("C", c.t("opt.counts")), c.opt("A", c.t("opt.attributes"))};
      ak.def = "C";
      const Answer k = co_await c.keyword(c.t("count.export"), std::move(ak));
      if (k.key().empty()) {
        co_return;
      }
      c.print(c.t("count.nodownload"));
      co_return;
    }
    const Value* e = c.doc.entities().get(p.id);
    const std::string b = e != nullptr ? base::js_string(e->get("block")) : std::string();
    if (e == nullptr || type_of(*e) != "insert" || !e->get("block").truthy() || b[0] == '*') {
      c.print(c.t("count.notblock"));
      continue;
    }
    const std::vector<CountRow> rows = count_blocks(c);
    const auto mine = std::ranges::find_if(rows, [&b](const CountRow& r) { return r.block == b; });
    std::size_t all = 0;
    c.doc.each_entity([&](const Value& x) {
      if (type_of(x) == "insert" && x.get("block") == Value(b)) {
        ++all;
      }
    });
    IdSet sel;
    for (const Id id : mine->ids) {
      sel.add(id);
    }
    c.host.set_selection(std::move(sel));
    c.print(c.words.tn("count.one", static_cast<double>(mine->n), {{"block", b}, {"all", std::to_string(all)}}));
    co_return;
  }
}

// ---- linetypes and lineweights (linetypes.js)

constexpr std::array<double, 24> kLineweights{0,    0.05, 0.09, 0.13, 0.15, 0.18, 0.2,  0.25, 0.3, 0.35, 0.4, 0.5,
                                              0.53, 0.6,  0.7,  0.8,  0.9,  1,    1.06, 1.2,  1.4, 1.58, 2,   2.11};

/// linetypes.js linetypeNames(doc): Continuous, the drawing's, the standard ones
std::vector<std::string> linetype_names(const Ctx& c) {
  std::vector<std::string> out{"Continuous"};
  const auto add = [&out](const std::string& n) {
    if (std::ranges::none_of(out,
                             [&n](const std::string& x) { return base::js_to_upper(x) == base::js_to_upper(n); })) {
      out.push_back(n);
    }
  };
  c.doc.linetypes.each([&](const std::string& n, const Value&) { add(n); });
  for (const char* b : {"DASHED", "HIDDEN", "CENTER", "PHANTOM", "DOT", "DASHDOT", "BORDER", "DIVIDE"}) {
    for (const char* suffix : {"", "2", "X2"}) {
      const std::string n = std::string(b) + suffix;
      if (std::ranges::none_of(out, [&n](const std::string& x) { return base::js_to_upper(x) == n; })) {
        out.push_back(n);
      }
    }
  }
  return out;
}
/// linetypes.js ltLabel(name)
std::string lt_label(const Ctx& c, const Value& n) {
  if (n.is_undefined() || n.is_null() || (n.is_string() && base::js_to_lower(n.as_string()) == "bylayer")) {
    return c.t("lt.bylayer");
  }
  if (n.is_string() && base::js_to_lower(n.as_string()) == "byblock") {
    return c.t("lt.byblock");
  }
  if (!n.truthy() || (n.is_string() && base::js_to_lower(n.as_string()) == "continuous")) {
    return c.t("lt.continuous");
  }
  return base::js_string(n);
}
/// linetypes.js lwLabel(mm)
std::string lw_label(const Ctx& c, const Value& w) {
  if (w.is_undefined() || w.is_null() || w == Value(-1)) {
    return c.t("lt.bylayer");
  }
  if (w == Value(-2)) {
    return c.t("lt.byblock");
  }
  if (w == Value(-3)) {
    return c.t("lw.default");
  }
  return c.t("lw.mm", {{"mm", base::js_to_fixed(base::js_to_number(w), 2)}});
}

Task<void> linetype_cmd(Ctx& c) {
  const std::vector<std::string> names = linetype_names(c);
  for (;;) {
    const Value cur = c.doc.current.get("linetype");
    const std::string v(base::js_trim(text_of(
        co_await c.string(c.t("cmd.linetype.prompt", {{"cur", cur.truthy() ? lt_label(c, cur) : "ByLayer"}})))));
    if (v.empty()) {
      co_return;
    }
    if (v == "?") {
      std::string list = "ByLayer, ByBlock";
      for (const std::string& n : names) {
        list += ", " + n;
      }
      c.print(list);
      continue;
    }
    std::optional<Value> hit;
    const std::string low = base::js_to_lower(v);
    if (low == "bylayer") {
      hit = Value(nullptr);
    } else if (low == "byblock") {
      hit = Value("ByBlock");
    } else {
      for (const std::string& n : names) {
        if (base::js_to_upper(n) == base::js_to_upper(v)) {
          hit = Value(n);
          break;
        }
      }
    }
    if (!hit) {
      c.print(c.t("cmd.linetype.unknown", {{"name", v}}));
      continue;
    }
    c.doc.current.set("linetype", *hit);
    c.print(c.t("cmd.linetype.done", {{"name", hit->truthy() ? lt_label(c, *hit) : "ByLayer"}}));
    co_return;
  }
}

Task<void> ltscale_cmd(Ctx& c) {
  const Value m = c.doc.meta.get("ltscale");
  const double cur = m.is_number() && m.as_number() > 0 ? m.as_number() : 1;
  Ask a;
  a.def = cur;
  const Answer v = co_await c.number(c.t("cmd.ltscale.prompt", {{"cur", base::js_string(Value(cur))}}), std::move(a));
  if (v.kind != Answer::Kind::number || !(v.number > 0)) {
    if (v.kind == Answer::Kind::number) {
      c.print(c.t("cmd.ltscale.positive"));
    }
    co_return;
  }
  if (v.number != cur) {
    c.doc.change("LTSCALE", [&] { c.doc.set_meta(base::make_object({{"ltscale", v.number}})); });
  }
  c.print(c.t("cmd.ltscale.done", {{"n", base::js_string(Value(v.number))}}));
}

Task<void> lweight_cmd(Ctx& c) {
  for (;;) {
    const Value cur = c.doc.current.get("lineweight");
    const std::string s(
        base::js_trim(text_of(co_await c.string(c.t("cmd.lweight.prompt", {{"cur", lw_label(c, cur)}})))));
    if (s.empty()) {
      co_return;
    }
    const std::string low = base::js_to_lower(s);
    std::optional<Value> v;
    if (low == "bylayer") {
      v = Value(nullptr);
    } else if (low == "byblock") {
      v = Value(-2);
    } else if (low == "default") {
      v = Value(-3);
    } else {
      const double n = base::js_parse_float(s);
      if (std::isfinite(n) && n >= 0 && n <= 2.11) {
        double best = kLineweights[0];
        for (const double w : kLineweights) {
          if (std::abs(w - n) < std::abs(best - n)) {
            best = w;
          }
        }
        v = Value(best);
      }
    }
    if (!v) {
      c.print(c.t("cmd.lweight.unknown", {{"value", s}}));
      continue;
    }
    c.doc.current.set("lineweight", *v);
    c.print(c.t("cmd.lweight.done", {{"lw", lw_label(c, *v)}}));
    co_return;
  }
}

}  // namespace

void add_layers(std::vector<CommandDef>& t) {
  const auto keep = [&t](CommandDef d) {
    d.keep_selection = true;
    t.push_back(std::move(d));
  };
  t.push_back(def("LAYISO", {}, layiso_cmd, "cmd.layiso.help"));
  keep(def("LAYUNISO", {}, layuniso_cmd, "cmd.layuniso.help", true));
  t.push_back(def("LAYOFF", {}, layoff_cmd, "cmd.layoff.help"));
  t.push_back(def("LAYFRZ", {}, layfrz_cmd, "cmd.layfrz.help"));
  t.push_back(def("LAYMCUR", {}, laymcur_cmd, "cmd.laymcur.help"));
  keep(def("LAYON", {}, layon_cmd, "cmd.layon.help", true));
  keep(def("LAYTHW", {}, laythw_cmd, "cmd.laythw.help", true));
  t.push_back(def("LAYMRG", {}, laymrg_cmd, "cmd.laymrg.help"));
  t.push_back(def("LAYDEL", {}, laydel_cmd, "cmd.laydel.help"));
  t.push_back(def("RENAME", {"-RENAME", "REN"}, rename_cmd, "cmd.rename.help"));
  t.push_back(def("XATTACH", {"XA"}, xattach_cmd, "xref.help.attach"));
  t.push_back(def("XREF", {"XR", "-XREF"}, xref_cmd, "xref.help"));
  t.push_back(def("SHEETSET", {"SSM", "SHEETSETMANAGER"}, sheetset_cmd, "sheetset.cmd.help"));
  t.push_back(def("PUBLISH", {"-PUBLISH"}, publish_cmd, "publish.help"));
  keep(def("COUNT", {"BCOUNT"}, count_cmd, "count.help"));
  keep(def("LINETYPE", {"LT", "CELTYPE"}, linetype_cmd, "cmd.linetype.help", true));
  keep(def("LTSCALE", {"LTS"}, ltscale_cmd, "cmd.ltscale.help", true));
  keep(def("LWEIGHT", {"LW", "LINEWEIGHT"}, lweight_cmd, "cmd.lweight.help", true));
}

}  // namespace cc::cmd::detail
