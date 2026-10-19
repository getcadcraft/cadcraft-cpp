// SPDX-License-Identifier: Apache-2.0
// commands.js (CadCraft 0.5) in C++, M5.3a: BLOCK, ATTDEF, INSERT, PURGE, and LIBRARY / BPARAMETER (which say what
// they need). INSERT's File and Library sources need a host that can open another drawing or the library panel.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cad/geom.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

std::string type_of(const Value& e) {
  return base::js_string(e.get("type"));
}
std::string trim(const std::string& s) {
  const std::size_t a = s.find_first_not_of(" \t\n\r\f\v");
  if (a == std::string::npos) {
    return {};
  }
  const std::size_t b = s.find_last_not_of(" \t\n\r\f\v");
  return s.substr(a, b - a + 1);
}
/// The text a string prompt answered ('' for none).
std::string text_of(const Answer& a) {
  if (a.kind == Answer::Kind::text) {
    return a.text;
  }
  return a.kind == Answer::Kind::typed ? base::js_string(a.typed) : std::string();
}
bool has_bad_char(const std::string& s) {
  return s.find_first_of("<>/\\\":;?*|=`") != std::string::npos;
}
/// commands.js validBlockName(n)
bool valid_block_name(const std::string& n) {
  return !n.empty() && n[0] != '*' && !has_bad_char(n) && n.size() <= 255;
}
/// geom.js normTag(t): trimmed, upper case, runs of white space as _, at most 255 characters
std::string norm_tag(const std::string& t) {
  const std::string up = base::js_to_upper(trim(t));
  std::string out;
  bool space = false;
  for (const char ch : up) {
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v') {
      space = true;
      continue;
    }
    if (space) {
      out += '_';
      space = false;
    }
    out += ch;
  }
  return out.size() > 255 ? out.substr(0, 255) : out;
}
/// attrs.js defaultAttrs(blk): { TAG: default } of the definitions that are not constant
Value default_attrs(const Value* blk) {
  Value out = Value::object();
  if (blk == nullptr) {
    return out;
  }
  const Value ads = blk->get("attdefs");
  if (!ads.is_array()) {
    return out;
  }
  for (const Value& d : ads.as_array()) {
    if (!d.get("constant").truthy()) {
      const Value def = d.get("def");
      out.set(base::js_string(d.get("tag")), def.truthy() ? def : Value(""));
    }
  }
  return out;
}

/// commands.js nests(doc, inner, name): block `inner` holds a reference to `name`, at any depth
/// (the JS recursion, as a walk with the same depth limit of 16)
bool nests(const Ctx& c, const std::string& inner, const std::string& name) {
  std::vector<std::pair<std::string, int>> todo{{inner, 0}};
  while (!todo.empty()) {
    const auto [blk, depth] = todo.back();
    todo.pop_back();
    const Value* b = c.doc.blocks().get(blk);
    if (b == nullptr || depth > 16) {
      continue;
    }
    const Value ents = b->get("entities");
    if (!ents.is_array()) {
      continue;
    }
    for (const Value& e : ents.as_array()) {
      if (type_of(e) != "insert") {
        continue;
      }
      const std::string ref = base::js_string(e.get("block"));
      if (ref == name) {
        return true;
      }
      todo.emplace_back(ref, depth + 1);
    }
  }
  return false;
}

/// a.localeCompare(b, undefined, { numeric: true }) for block names: letters by their letter first (lower case
/// before upper on a tie), runs of digits by their value, other characters before digits and letters.
int locale_numeric(const std::string& a, const std::string& b) {
  const auto rank = [](unsigned char ch) {
    if (std::isdigit(ch) != 0) {
      return 1;
    }
    return std::isalpha(ch) != 0 ? 2 : 0;
  };
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() && j < b.size()) {
    const auto ca = static_cast<unsigned char>(a[i]);
    const auto cb = static_cast<unsigned char>(b[j]);
    if (std::isdigit(ca) != 0 && std::isdigit(cb) != 0) {
      std::size_t ie = i;
      std::size_t je = j;
      while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie])) != 0) {
        ++ie;
      }
      while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je])) != 0) {
        ++je;
      }
      std::string na = a.substr(i, ie - i);
      std::string nb = b.substr(j, je - j);
      na.erase(0, std::min(na.find_first_not_of('0'), na.size() - 1));
      nb.erase(0, std::min(nb.find_first_not_of('0'), nb.size() - 1));
      if (na.size() != nb.size()) {
        return na.size() < nb.size() ? -1 : 1;
      }
      if (const int c = na.compare(nb); c != 0) {
        return c < 0 ? -1 : 1;
      }
      i = ie;
      j = je;
      continue;
    }
    if (rank(ca) != rank(cb)) {
      return rank(ca) < rank(cb) ? -1 : 1;
    }
    const int la = std::tolower(ca);
    const int lb = std::tolower(cb);
    if (la != lb) {
      return la < lb ? -1 : 1;
    }
    ++i;
    ++j;
  }
  if (i < a.size() || j < b.size()) {
    return i < a.size() ? 1 : -1;
  }
  // the same letters: lower case first
  for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) {
    if (a[k] != b[k]) {
      return std::islower(static_cast<unsigned char>(a[k])) != 0 ? -1 : 1;
    }
  }
  return 0;
}
/// commands.js namedBlocks(doc): the blocks that are not anonymous, in name order
std::vector<std::string> named_blocks(const Ctx& c) {
  std::vector<std::string> out;
  c.doc.blocks().each([&](const std::string& n, const Value&) {
    if (n.empty() || n[0] != '*') {
      out.push_back(n);
    }
  });
  std::ranges::stable_sort(out, [](const std::string& a, const std::string& b) { return locale_numeric(a, b) < 0; });
  return out;
}
std::string list_of(const std::vector<std::string>& names, std::size_t max) {
  std::string s;
  for (std::size_t i = 0; i < names.size() && i < max; ++i) {
    s += (i != 0 ? ", " : "") + names[i];
  }
  return s;
}

/// commands.js isAttdefText / splitAttdefs: attribute-definition texts → block attdefs, the rest the block's objects
std::pair<base::Array, base::Array> split_attdefs(const std::vector<Value>& list) {
  base::Array entities;
  base::Array attdefs;
  for (const Value& e : list) {
    const Value a = e.get("attdef");
    if (type_of(e) != "text" || !a.truthy() || !a.get("tag").is_string()) {
      entities.push_back(e);
      continue;
    }
    const auto nn = [](const Value& v, const Value& d) { return v.is_undefined() || v.is_null() ? d : v; };
    const auto orv = [](const Value& v, const Value& d) { return v.truthy() ? v : d; };
    attdefs.push_back(base::make_object({{"tag", norm_tag(base::js_string(a.get("tag")))},
                                         {"prompt", nn(a.get("prompt"), "")},
                                         {"def", nn(a.get("def"), "")},
                                         {"x", e.get("x")},
                                         {"y", e.get("y")},
                                         {"h", e.get("h")},
                                         {"rot", orv(e.get("rot"), 0)},
                                         {"ha", orv(e.get("ha"), "left")},
                                         {"va", orv(e.get("va"), "baseline")},
                                         {"wf", orv(e.get("wf"), 1)},
                                         {"invisible", a.get("invisible").truthy()},
                                         {"constant", a.get("constant").truthy()},
                                         {"preset", a.get("preset").truthy()},
                                         {"layer", e.get("layer")},
                                         {"color", nn(e.get("color"), nullptr)}}));
  }
  return {std::move(entities), std::move(attdefs)};
}

Task<void> block_cmd(Ctx& c) {
  std::string name;
  for (;;) {
    name = trim(text_of(co_await c.string(c.t("cmd.block.enter_block_name"))));
    if (name.empty()) {
      co_return;
    }
    if (!valid_block_name(name)) {
      c.print(c.t("cmd.block.badname"));
      continue;
    }
    if (c.doc.blocks().has(name)) {
      Ask a;
      a.options = {c.opt("Y", c.t("opt.yes")), c.opt("N", c.t("opt.no"))};
      a.def = "N";
      const Answer k = co_await c.keyword(c.t("cmd.block.redefine", {{"name", name}}), std::move(a));
      if (k.key() != "Y") {
        continue;
      }
    }
    break;
  }
  const Pt base = (co_await c.point(c.t("cmd.block.specify_insertion_base_point"))).p;
  const Answer ids = co_await c.select();
  if (ids.kind != Answer::Kind::set || ids.set.empty()) {
    c.print(c.t("cmd.block.noobjects"));
    co_return;
  }
  std::vector<Value> src;
  for (const Id id : ids.set) {
    if (const Value* e = c.doc.entities().get(id)) {
      src.push_back(*e);
    }
  }
  if (std::ranges::any_of(src, [](const Value& e) { return type_of(e) == "viewport"; })) {
    c.print(c.t("cmd.block.viewport_cannot_go_block"));
    co_return;
  }
  if (std::ranges::any_of(src, [&](const Value& e) {
        const std::string b = base::js_string(e.get("block"));
        return type_of(e) == "insert" && (b == name || nests(c, b, name));
      })) {
    c.print(c.t("cmd.block.block_cannot_contain_itself"));
    co_return;
  }
  auto [plain, attdefs] = split_attdefs(src);
  const std::size_t n_att = attdefs.size();
  c.doc.change("BLOCK", [&] {
    const Value& blk =
        c.doc.define_block(base::make_object({{"name", name},
                                              {"base", base::make_object({{"x", base.x}, {"y", base.y}})},
                                              {"entities", plain},
                                              {"attdefs", attdefs}}));
    const Value attrs = default_attrs(&blk);
    for (const Value& e : src) {
      c.doc.remove(id_of(e));
    }
    Value ins = base::make_object(
        {{"type", "insert"}, {"block", name}, {"x", base.x}, {"y", base.y}, {"sx", 1}, {"sy", 1}, {"rot", 0}});
    if (n_att != 0) {
      ins.set("attrs", attrs);
    }
    (void)c.doc.add(c.ent(ins));
  });
  if (n_att != 0) {
    c.print(c.words.tn("cmd.attdef.inblock", static_cast<double>(n_att)));
  }
  c.engine.state.set("lastBlock", name);
  c.print(c.words.tn("cmd.block.made", static_cast<double>(src.size()), {{"name", name}}));
}

Task<void> attdef_cmd(Ctx& c) {
  Value m = c.engine.state.get("attModes");
  if (!m.is_object()) {
    m = base::make_object({{"invisible", false}, {"constant", false}, {"preset", false}});
  }
  const auto yn = [&c](const Value& b) { return c.t(b.truthy() ? "opt.yes" : "opt.no"); };
  for (;;) {
    c.engine.state.set("attModes", m);
    c.print(c.t("cmd.attdef.modes",
                {{"i", yn(m.get("invisible"))}, {"c", yn(m.get("constant"))}, {"p", yn(m.get("preset"))}}));
    Ask a;
    a.options = {c.opt("I", c.t("opt.invisible")), c.opt("C", c.t("opt.constant")), c.opt("P", c.t("opt.preset"))};
    a.none = true;
    const Answer k = co_await c.keyword(c.t("cmd.attdef.mode"), std::move(a));
    if (k.kind != Answer::Kind::option) {
      break;
    }
    const char* key = "preset";
    if (k.option == "I") {
      key = "invisible";
    } else if (k.option == "C") {
      key = "constant";
    }
    m.set(key, !m.get(key).truthy());
  }
  c.engine.state.set("attModes", m);
  std::string tag;
  for (;;) {
    tag = norm_tag(text_of(co_await c.string(c.t("cmd.attdef.tag"))));
    if (tag.empty()) {
      co_return;
    }
    if (has_bad_char(tag)) {
      c.print(c.t("cmd.attdef.badtag"));
      continue;
    }
    break;
  }
  const bool constant = m.get("constant").truthy();
  std::string prompt;
  if (!constant) {
    Ask ap;
    ap.raw = true;
    prompt = trim(text_of(co_await c.string(c.t("cmd.attdef.prompt"), std::move(ap))));
  }
  Ask ad;
  ad.raw = true;
  const std::string def = cad::one_line(
      trim(text_of(co_await c.string(c.t(constant ? "cmd.attdef.value" : "cmd.attdef.default"), std::move(ad)))));
  const Pt at = (co_await c.point(c.t("cmd.attdef.at"))).p;
  Ask ah;
  ah.base = at;
  ah.def = c.doc.current.get("textHeight");
  const Answer h = co_await c.distance(c.t("cmd.attdef.height"), std::move(ah));
  if (h.kind != Answer::Kind::number || !(h.number > 0)) {
    co_return;
  }
  Ask ar;
  ar.base = at;
  ar.def = c.doc.in_model() && c.doc.ucs.truthy() && c.doc.ucs.get("ang").truthy() ? c.doc.ucs.get("ang") : Value(0);
  ar.ucs = true;
  const Answer rot = co_await c.angle(c.t("cmd.attdef.rot"), std::move(ar));
  const double r = rot.kind == Answer::Kind::number && rot.number != 0 && !std::isnan(rot.number) ? rot.number : 0;
  (void)c.add(c.ent(base::make_object({{"type", "text"},
                                       {"x", at.x},
                                       {"y", at.y},
                                       {"h", h.number},
                                       {"rot", norm_angle(r)},
                                       {"s", tag},
                                       {"ha", "left"},
                                       {"va", "baseline"},
                                       {"wf", 1},
                                       {"attdef", base::make_object({{"tag", tag},
                                                                     {"prompt", prompt},
                                                                     {"def", def},
                                                                     {"invisible", m.get("invisible").truthy()},
                                                                     {"constant", constant},
                                                                     {"preset", m.get("preset").truthy()}})}})));
  c.print(c.t("cmd.attdef.made", {{"tag", tag}}));
}

/// commands.js placeBlock(c, name, { scale, attrs }): preview, S / R, then each attribute value the block asks for
Task<void> place_block(Ctx& c, const std::string& name) {
  const Value* bp = c.doc.blocks().get(name);
  const Value blk = bp != nullptr ? *bp : Value();
  const Value ls = c.engine.state.get("lastInsertScale");
  const Value lr = c.engine.state.get("lastInsertRot");
  double sc = ls.is_undefined() || ls.is_null() ? 1 : base::js_to_number(ls);
  double rot = lr.is_undefined() || lr.is_null() ? 0 : base::js_to_number(lr);
  Value values = default_attrs(bp);
  for (;;) {
    c.preview([&c, &name, &sc, &rot, &values](Pt cur) {
      return base::Array{base::make_object({{"type", "insert"},
                                            {"layer", c.doc.current.get("layer")},
                                            {"block", name},
                                            {"x", cur.x},
                                            {"y", cur.y},
                                            {"sx", sc},
                                            {"sy", sc},
                                            {"rot", rot},
                                            {"attrs", values}})};
    });
    Ask ap;
    ap.options = {c.opt("S", "Scale"), c.opt("R", "Rotate")};
    const Answer p = co_await c.point(c.t("cmd.pasteclip.specify_insertion_point"), std::move(ap));
    if (p.is("S")) {
      Ask as;
      as.def = sc;
      const Answer v = co_await c.number(c.t("cmd.scale.specify_scale_factor"), std::move(as));
      if (v.truthy()) {
        sc = v.number;
      }
      c.engine.state.set("lastInsertScale", sc);
      continue;
    }
    if (p.is("R")) {
      Ask ar;
      ar.def = rot;
      ar.ucs = true;
      const Answer a = co_await c.angle(c.t("cmd.mtext.rot"), std::move(ar));
      rot = norm_angle(a.kind == Answer::Kind::number ? a.number : 0);
      c.engine.state.set("lastInsertRot", rot);
      continue;
    }
    c.preview(nullptr);
    const Value ads = blk.is_object() ? blk.get("attdefs") : Value();
    if (ads.is_array()) {
      for (const Value& d : ads.as_array()) {
        if (d.get("constant").truthy() || d.get("preset").truthy()) {
          continue;
        }
        const std::string tag = base::js_string(d.get("tag"));
        std::string label = d.get("prompt").truthy() ? base::js_string(d.get("prompt")) : tag;
        while (!label.empty() && (label.back() == ':' || label.back() == ' ' || label.back() == '\t')) {
          label.pop_back();  // .replace(/[:\s]+$/, '')
        }
        Ask av;
        const Value cur = values.get(tag);
        if (cur.truthy()) {
          av.def = cur;
        }
        std::string ask = "Enter ";
        ask += label;
        ask += " (";
        ask += tag;
        ask += ")";
        const std::string v = trim(text_of(co_await c.string(ask, std::move(av))));
        std::string kept;  // values[tag] ?? ''
        if (!cur.is_undefined() && !cur.is_null()) {
          kept = base::js_string(cur);
        }
        values.set(tag, cad::one_line(v.empty() ? kept : v));
      }
    }
    Value e = c.ent(base::make_object(
        {{"type", "insert"}, {"block", name}, {"x", p.p.x}, {"y", p.p.y}, {"sx", sc}, {"sy", sc}, {"rot", rot}}));
    if ((ads.is_array() && !ads.as_array().empty()) || !values.as_object().empty()) {
      e.set("attrs", values);
    }
    (void)c.add(e);
    co_return;
  }
}

Task<void> insert_cmd(Ctx& c) {
  const std::vector<std::string> names = named_blocks(c);
  if (names.empty()) {
    c.print(c.t("cmd.insert.drawing_has_no_blocks"));
  }
  const std::string last = base::js_string(c.engine.state.get("lastBlock"));
  const bool has_last = c.engine.state.get("lastBlock").truthy();
  std::string name;
  for (;;) {
    Ask a;
    if (has_last && c.doc.blocks().has(last)) {
      a.def = last;
    }
    const std::string typed = trim(text_of(co_await c.string(c.t("cmd.insert.enter_block_name_file"), std::move(a))));
    name = typed;
    if (name.empty() && has_last) {
      name = last;
    }
    if (!name.empty() && c.doc.blocks().has(name)) {
      break;
    }
    if (name.empty()) {
      co_return;
    }
    const std::string low = base::js_to_lower(name);
    const auto near = std::ranges::find_if(names, [&low](const std::string& n) { return base::js_to_lower(n) == low; });
    if (near != names.end()) {
      name = *near;
      break;
    }
    if (low == "f" || low == "file") {  // the host opens another drawing (the browser app; not this host yet)
      c.print(c.t("cmd.library.opening_another_drawing_needs"));
      co_return;
    }
    if (low == "l" || low == "lib" || low == "library") {
      c.print(c.t("cmd.library.library_needs_browser"));
      co_return;
    }
    const std::string list = names.empty() ? c.t("cmd.none") : list_of(names, 30) + (names.size() > 30 ? " …" : "");
    c.print(c.t("cmd.insert.noblock", {{"name", name}, {"list", list}}));
  }
  c.engine.state.set("lastBlock", name);
  co_await place_block(c, name);
}

Task<void> library_cmd(Ctx& c) {
  c.print(c.t("cmd.library.library_needs_browser"));
  co_return;
}
Task<void> bparameter_cmd(Ctx& c) {
  c.print(c.t("cmd.bparameter.not_built"));
  co_return;
}

Task<void> purge_cmd(Ctx& c) {
  std::size_t n = 0;
  c.doc.change("PURGE", [&] {
    // a block used only by unused blocks goes too: repeat until nothing changes
    for (bool again = true; again;) {
      again = false;
      std::vector<std::string> keys;
      c.doc.blocks().each([&](const std::string& k, const Value&) { keys.push_back(k); });
      for (const std::string& k : keys) {
        if (c.doc.block_uses(k) == 0) {
          c.doc.remove_block(k);
          ++n;
          again = true;
        }
      }
    }
  });
  c.print(n != 0 ? c.words.tn("cmd.purge.removed", static_cast<double>(n)) : c.t("cmd.purge.none"));
  co_return;
}

}  // namespace

void add_blocks(std::vector<CommandDef>& t) {
  t.push_back(def("BLOCK", {"B", "BMAKE"}, block_cmd, "cmd.block.make_block_objects_they"));
  t.push_back(def("ATTDEF", {"ATT", "DDATTDEF", "-ATTDEF"}, attdef_cmd, "cmd.attdef.help"));
  t.push_back(def("BPARAMETER", {"BPARAM"}, bparameter_cmd, "cmd.bparameter.help"));
  t.push_back(def("INSERT", {"I", "DDINSERT"}, insert_cmd, "cmd.insert.help"));
  CommandDef lib = def("LIBRARY", {"LIB"}, library_cmd, "cmd.library.library_panel_place_items", true);
  lib.keep_selection = true;
  t.push_back(std::move(lib));
  CommandDef pu = def("PURGE", {"PU"}, purge_cmd, "cmd.purge.remove_block_definitions_nothing", true);
  pu.keep_selection = true;
  t.push_back(std::move(pu));
}

}  // namespace cc::cmd::detail
