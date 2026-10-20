// SPDX-License-Identifier: Apache-2.0
// clipboard.js and blockcopy.js (CadCraft 0.5) in C++.
#include "cc/cmd/clipboard.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/geom.hpp"

namespace cc::cmd {

namespace {

using base::Value;

std::string str(const Value& v) {
  return base::js_string(v);
}
double num(const Value& v) {
  return base::js_to_number(v);
}

/// refsOf(list): the blocks inserts and dimensions use directly, first seen first
std::vector<std::string> refs_of(const Value& list) {
  std::vector<std::string> out;
  if (!list.is_array()) {
    return out;
  }
  for (const Value& e : list.as_array()) {
    const Value t = e.get("type");
    const Value b = e.get("block");
    if ((t == Value("insert") || t == Value("dim")) && b.is_string() && !b.as_string().empty() &&
        std::ranges::find(out, b.as_string()) == out.end()) {
      out.push_back(b.as_string());
    }
  }
  return out;
}

/// JSON.parse(JSON.stringify(v)): undefined members dropped, NaN / ±Infinity as null, undefined in arrays as null
// NOLINTNEXTLINE(misc-no-recursion) — as deep as the value (an entity: a few levels)
Value json_copy(const Value& v) {
  if (v.is_number()) {
    return std::isfinite(v.as_number()) ? v : Value(nullptr);
  }
  if (v.is_point()) {
    return base::make_object({{"x", json_copy(v.get("x"))}, {"y", json_copy(v.get("y"))}});
  }
  if (v.is_array()) {
    base::Array a;
    a.reserve(v.as_array().size());
    for (const Value& x : v.as_array()) {
      a.push_back(x.is_undefined() ? Value(nullptr) : json_copy(x));
    }
    return {std::move(a)};
  }
  if (v.is_object()) {
    Value o = Value::object();
    for (const base::Member* m : base::js_members(v.as_object())) {
      if (!m->value.is_undefined()) {
        o.set(m->key, json_copy(m->value));
      }
    }
    return o;
  }
  return v;
}
/// plain(e): a copy without the id, a view label's link and the readers' private fields
Value plain(const Value& e) {
  Value c = json_copy(e);
  c.erase("id");
  c.erase("field");
  std::vector<std::string> priv;
  for (const base::Member* m : base::js_members(c.as_object())) {
    if (m->key.starts_with("_")) {
      priv.push_back(m->key);
    }
  }
  for (const std::string& k : priv) {
    c.erase(k);
  }
  return c;
}

cad::Bounds clip_extents_of(const Value& clip, bool& ok) {
  std::map<std::string, const Value*> blocks;
  const Value block_list = clip.get("blocks");  // held: the map points into it
  for (const Value& b : block_list.as_array()) {
    blocks[str(b.get("name"))] = &b;
  }
  const cad::BlockLookup lookup = [&blocks](const std::string& n) -> const Value* {
    const auto it = blocks.find(n);
    return it == blocks.end() ? nullptr : it->second;
  };
  cad::Bounds b;
  const auto grow = [&b](double x, double y) {
    b.min_x = std::min(b.min_x, x);
    b.min_y = std::min(b.min_y, y);
    b.max_x = std::max(b.max_x, x);
    b.max_y = std::max(b.max_y, y);
  };
  for (const Value& e : clip.get("entities").as_array()) {
    const cad::Bounds eb = cad::entity_bounds(e, lookup);
    if (eb.valid()) {
      grow(eb.min_x, eb.min_y);
      grow(eb.max_x, eb.max_y);
    } else if (std::isfinite(num(e.get("x"))) && std::isfinite(num(e.get("y")))) {
      grow(num(e.get("x")), num(e.get("y")));
    }
  }
  ok = b.valid();
  return b;
}

const std::map<std::string, double, std::less<>>& mm_per() {
  static const std::map<std::string, double, std::less<>> kMm{
      {"millimeters", 1}, {"centimeters", 10}, {"meters", 1000},   {"kilometers", 1e6}, {"inches", 25.4},
      {"feet", 304.8},    {"yards", 914.4},    {"miles", 1609344}, {"mils", 0.0254},    {"microinches", 0.0000254}};
  return kMm;
}
std::optional<double> mm_per_unit(const Value& units) {
  const auto it = mm_per().find(str(units));
  return units.is_string() && it != mm_per().end() ? std::optional(it->second) : std::nullopt;
}

/// blockNameFromFile(file)
std::string block_name_from_file(std::string_view file) {
  std::string base(file.empty() ? std::string_view("Drawing") : file);
  if (const std::size_t slash = base.find_last_of("\\/"); slash != std::string::npos) {
    base = base.substr(slash + 1);
  }
  if (const std::size_t dot = base.find_last_of('.'); dot != std::string::npos) {
    base = base.substr(0, dot);
  }
  std::string n;
  for (const char ch : base) {
    n += std::string_view("<>/\\\":;?*|=`").contains(ch) ? '_' : ch;
  }
  std::size_t k = 0;
  while (k < n.size() && n[k] == '*') {
    ++k;
  }
  n = std::string(base::js_trim(std::string_view(n).substr(k)));
  n = n.substr(0, 255);
  return n.empty() ? std::string("Drawing") : n;
}

}  // namespace

std::optional<Value> make_clip(const doc::Doc& doc, const std::vector<doc::Id>& ids,
                               const std::optional<std::pair<double, double>>& base_pt, double now_ms) {
  base::Array ents;
  std::size_t skipped = 0;
  for (const doc::Id id : ids) {
    const Value* e = doc.entities().get(id);
    if (e == nullptr) {
      continue;
    }
    if (e->get("type") == Value("viewport")) {
      ++skipped;
      continue;
    }
    ents.push_back(plain(*e));
  }
  if (ents.empty()) {
    return std::nullopt;
  }
  // every block reachable from them, nested ones first
  base::Array blocks;
  std::set<std::string> seen;
  // NOLINTNEXTLINE(misc-no-recursion) — nested blocks, at most 33 deep
  const auto visit = [&](const auto& self, const std::string& name, int depth) -> void {
    if (seen.contains(name) || depth > 32) {
      return;
    }
    seen.insert(name);
    const Value* b = doc.blocks().get(name);
    if (b == nullptr) {
      return;
    }
    for (const std::string& r : refs_of(b->get("entities"))) {
      self(self, r, depth + 1);
    }
    base::Array list;
    for (const Value& x : b->get("entities").as_array()) {
      list.push_back(plain(x));
    }
    const Value base = b->get("base");
    Value out = base::make_object(
        {{"name", b->get("name")},
         {"base", base::make_object(
                      {{"x", num(base.get("x")) != 0 && !std::isnan(num(base.get("x"))) ? num(base.get("x")) : 0},
                       {"y", num(base.get("y")) != 0 && !std::isnan(num(base.get("y"))) ? num(base.get("y")) : 0}})},
         {"entities", Value(std::move(list))}});
    if (const Value ad = b->get("attdefs"); ad.is_array() && !ad.as_array().empty()) {
      out.set("attdefs", json_copy(ad));
    }
    blocks.push_back(std::move(out));
  };
  for (const std::string& r : refs_of(Value(ents))) {
    visit(visit, r, 0);
  }
  // drop references to blocks the drawing does not have (a reader may leave one): they draw nothing anyway
  std::set<std::string> have;
  for (const Value& b : blocks) {
    have.insert(str(b.get("name")));
  }
  const auto keep = [&have](const Value& e) {
    return e.get("type") != Value("insert") || have.contains(str(e.get("block")));
  };
  const auto clean = [&have](Value e) {
    if (e.get("type") == Value("dim") && e.get("block").truthy() && !have.contains(str(e.get("block")))) {
      e.erase("block");
    }
    return e;
  };
  base::Array top;
  for (const Value& e : ents) {
    if (keep(e)) {
      top.push_back(clean(e));
    }
  }
  for (Value& b : blocks) {
    base::Array kept;
    for (const Value& e : b.get("entities").as_array()) {
      if (keep(e)) {
        kept.push_back(clean(e));
      }
    }
    b.set("entities", Value(std::move(kept)));
  }
  if (top.empty()) {
    return std::nullopt;
  }
  std::vector<std::string> used{"0"};
  const auto use = [&used](const Value& l) {
    const std::string n = l.truthy() ? str(l) : std::string("0");
    if (std::ranges::find(used, n) == used.end()) {
      used.push_back(n);
    }
  };
  for (const Value& e : top) {
    use(e.get("layer"));
  }
  for (const Value& b : blocks) {
    for (const Value& e : b.get("entities").as_array()) {
      use(e.get("layer"));
    }
    if (const Value ad = b.get("attdefs"); ad.is_array()) {
      for (const Value& d : ad.as_array()) {
        use(d.get("layer"));
      }
    }
  }
  base::Array layers;
  for (const std::string& n : used) {
    if (const Value* l = doc.layers().get(n)) {
      layers.push_back(base::make_object({{"name", l->get("name")},
                                          {"color", l->get("color")},
                                          {"on", l->get("on")},
                                          {"frozen", l->get("frozen")},
                                          {"locked", l->get("locked")},
                                          {"plot", l->get("plot")}}));
    }
  }
  static std::uint64_t counter = 0;  // the clip's id: unique in this process (0.5: random + time)
  ++counter;
  const Value units = doc.meta.get("units");
  const Value name = doc.meta.get("name");
  Value clip = base::make_object({{"format", "cadcraft-clipboard"},
                                  {"version", 1},
                                  {"id", "c" + std::to_string(counter)},
                                  {"time", now_ms},
                                  {"units", units.truthy() ? units : Value("")},
                                  {"space", doc.in_model() ? "model" : "paper"},
                                  {"from", name.truthy() ? name : Value("")},
                                  {"entities", Value(std::move(top))},
                                  {"blocks", Value(std::move(blocks))},
                                  {"layers", Value(std::move(layers))}});
  if (base_pt) {
    clip.set("base", base::make_object({{"x", base_pt->first}, {"y", base_pt->second}}));
  }
  if (skipped > 0) {
    clip.set("skipped", static_cast<double>(skipped));
  }
  return clip;
}

std::pair<double, double> clip_base_point(const Value& clip, std::string_view mode) {
  if (const Value b = clip.get("base"); b.is_object() || b.is_point()) {
    return {num(b.get("x")), num(b.get("y"))};
  }
  bool ok = false;
  const cad::Bounds b = clip_extents_of(clip, ok);
  if (!ok) {
    return {0, 0};
  }
  if (mode == "center") {
    return {(b.min_x + b.max_x) / 2, (b.min_y + b.max_y) / 2};
  }
  return {b.min_x, b.min_y};
}

base::Array paste_preview(const Value& clip, std::size_t max) {
  std::map<std::string, const Value*> blocks;
  const Value block_list = clip.get("blocks");  // held: the map points into it
  for (const Value& b : block_list.as_array()) {
    blocks[str(b.get("name"))] = &b;
  }
  const cad::BlockLookup lookup = [&blocks](const std::string& n) -> const Value* {
    const auto it = blocks.find(n);
    return it == blocks.end() ? nullptr : it->second;
  };
  base::Array out;
  std::size_t n = 0;
  for (const Value& e : clip.get("entities").as_array()) {
    if (n > max) {
      break;
    }
    if (e.get("type") == Value("insert") || (e.get("type") == Value("dim") && e.get("block").truthy())) {
      for (const cad::Prim& p : cad::entity_prims(e, lookup)) {
        if (++n > max) {
          break;
        }
        if (!p.arc) {
          out.push_back(base::make_object({{"type", "line"}, {"x1", p.x1}, {"y1", p.y1}, {"x2", p.x2}, {"y2", p.y2}}));
        } else {
          out.push_back(
              base::make_object({{"type", "arc"}, {"cx", p.cx}, {"cy", p.cy}, {"r", p.r}, {"a0", p.a0}, {"a1", p.a1}}));
        }
      }
    } else {
      out.push_back(e);
      ++n;
    }
  }
  return out;
}

CopyPlan plan_copy(const doc::Doc& doc, const Value& src, const std::optional<std::string>& block,
                   const std::optional<std::string>& whole, bool refs, const i18n::Catalog& words) {
  std::map<std::string, const Value*> src_blocks;
  const Value src_list = src.get("blocks");  // held: the map points into it
  if (src_list.is_array()) {
    for (const Value& b : src_list.as_array()) {
      src_blocks[str(b.get("name"))] = &b;  // a later block of the same name wins, as the JS's Map
    }
  }
  CopyPlan plan;
  std::vector<std::string> root_refs;
  if (refs) {
    root_refs = refs_of(src.get("entities"));
  } else if (whole) {
    plan.root = block_name_from_file(*whole);
    root_refs = refs_of(src.get("entities"));
  } else {
    if (!block || !src_blocks.contains(*block)) {
      throw std::runtime_error(words.t("blockcopy.noblock", {{"name", block.value_or("")}}));
    }
    plan.root = *block;
    root_refs = {*block};
  }
  std::set<std::string> seen;
  // NOLINTNEXTLINE(misc-no-recursion) — nested blocks, at most 33 deep
  const auto visit = [&](const auto& self, const std::string& name, int depth) -> void {
    if (seen.contains(name) || depth > 32) {
      return;
    }
    seen.insert(name);
    const auto it = src_blocks.find(name);
    if (it == src_blocks.end()) {
      return;
    }
    for (const std::string& r : refs_of(it->second->get("entities"))) {
      self(self, r, depth + 1);
    }
    plan.needed.push_back(name);
  };
  for (const std::string& r : root_refs) {
    visit(visit, r, 0);
  }
  for (const std::string& n : plan.needed) {
    if (!n.starts_with("*") && doc.blocks().get(n) != nullptr) {
      plan.clashes.push_back(n);
    }
  }
  if (whole && plan.root && doc.blocks().get(*plan.root) != nullptr) {
    plan.clashes.push_back(*plan.root);
  }
  const auto a = mm_per_unit(src.get("meta").get("units"));
  const auto b = mm_per_unit(doc.meta.get("units"));
  plan.scale = a && b ? *a / *b : 1;
  if (a && b && *a != *b) {
    std::string sc;
    base::append_number(sc, base::js_number(base::js_to_exponential(plan.scale, 5)));
    plan.note =
        words.t("blockcopy.units",
                {{"from", str(src.get("meta").get("units"))}, {"to", str(doc.meta.get("units"))}, {"scale", sc}});
  }
  plan.whole = whole.has_value();
  if (whole) {
    const Value ib = src.get("meta").get("insbase");
    plan.base = ib.is_object() ? std::pair{num(ib.get("x")), num(ib.get("y"))} : std::pair{0.0, 0.0};
  }
  return plan;
}

CopyResult apply_copy(doc::Doc& doc, const Value& src, const CopyPlan& plan, bool keep_on_clash) {
  std::map<std::string, const Value*> src_blocks;
  const Value src_list = src.get("blocks");  // held: the map points into it
  if (src_list.is_array()) {
    for (const Value& b : src_list.as_array()) {
      src_blocks[str(b.get("name"))] = &b;
    }
  }
  CopyResult r;
  std::set<std::string> keep;
  const auto renamed_to = [&r](const std::string& n) {
    return std::ranges::any_of(r.renamed, [&n](const auto& p) { return p.second == n; });
  };
  const auto has = [&doc](const std::string& n) { return doc.blocks().get(n) != nullptr; };
  const auto free = [&](const std::string& name) -> std::string {
    if (!has(name) && !renamed_to(name)) {
      return name;
    }
    if (name.starts_with("*")) {
      std::size_t k = 1;
      while (k < name.size() && std::isalpha(static_cast<unsigned char>(name[k])) != 0) {
        ++k;
      }
      const std::string p = name.substr(0, k);
      for (int i = 1;; ++i) {
        const std::string c = p + std::to_string(i);
        if (!has(c) && !renamed_to(c)) {
          return c;
        }
      }
    }
    for (int i = 2;; ++i) {
      const std::string c = name + " (" + std::to_string(i) + ")";
      if (!has(c) && !renamed_to(c)) {
        return c;
      }
    }
  };
  const auto renamed_of = [&r](const std::string& n) -> std::optional<std::string> {
    for (const auto& [a, b] : r.renamed) {
      if (a == n) {
        return b;
      }
    }
    return std::nullopt;
  };
  for (const std::string& n : plan.needed) {
    if (has(n) && !n.starts_with("*") && keep_on_clash) {
      keep.insert(n);
      continue;
    }
    const std::string to = free(n);
    if (to != n) {
      r.renamed.emplace_back(n, to);
    }
  }
  const auto rename = [&](Value e) {
    if (e.get("block").truthy()) {
      if (const auto to = renamed_of(str(e.get("block")))) {
        e.set("block", *to);
      }
    }
    return e;
  };
  std::vector<std::string> layers;
  const auto use = [&layers](const std::string& n) {
    if (std::ranges::find(layers, n) == layers.end()) {
      layers.push_back(n);
    }
  };
  for (const std::string& n : plan.needed) {
    if (keep.contains(n)) {
      continue;
    }
    const Value& b = *src_blocks.at(n);
    base::Array ents;
    for (const Value& e : b.get("entities").as_array()) {
      ents.push_back(rename(e));
      if (e.get("layer").truthy()) {
        use(str(e.get("layer")));
      }
    }
    if (const Value ad = b.get("attdefs"); ad.is_array()) {
      for (const Value& d : ad.as_array()) {
        use(d.get("layer").truthy() ? str(d.get("layer")) : std::string("0"));
      }
    }
    Value def = base::make_object(
        {{"name", Value(renamed_of(n).value_or(n))}, {"base", b.get("base")}, {"entities", Value(std::move(ents))}});
    if (!b.get("attdefs").is_undefined()) {
      def.set("attdefs", b.get("attdefs"));
    }
    (void)doc.define_block(def);
    ++r.copied;
  }
  r.name = plan.root;
  if (plan.whole && plan.root) {
    if (!keep_on_clash || !has(*plan.root)) {
      const std::string name = free(*plan.root);
      base::Array ents;
      if (src.get("entities").is_array()) {
        for (const Value& e : src.get("entities").as_array()) {
          if (!(e.get("type") == Value("viewport"))) {
            ents.push_back(rename(e));
            if (e.get("layer").truthy()) {
              use(str(e.get("layer")));
            }
          }
        }
      }
      const auto [bx, by] = plan.base.value_or(std::pair{0.0, 0.0});
      (void)doc.define_block(base::make_object(
          {{"name", name}, {"base", base::make_object({{"x", bx}, {"y", by}})}, {"entities", Value(std::move(ents))}}));
      ++r.copied;
      r.name = name;
    }
  } else if (r.name) {
    r.name = renamed_of(*r.name).value_or(*r.name);
  }
  // layers the copied objects use, with the other drawing's settings
  std::map<std::string, Value> src_layers;
  if (src.get("layers").is_array()) {
    for (const Value& l : src.get("layers").as_array()) {
      src_layers[str(l.get("name"))] = l;
    }
  }
  for (const std::string& l : layers) {
    if (doc.layers().get(l) == nullptr) {
      const auto it = src_layers.find(l);
      (void)doc.add_layer(it != src_layers.end() ? it->second : base::make_object({{"name", l}}));
      r.layers.push_back(l);
    }
  }
  return r;
}

PasteResult paste_clip(doc::Doc& doc, const Value& clip, double dx, double dy, const i18n::Catalog& words) {
  const Value src = base::make_object({{"entities", clip.get("entities")},
                                       {"blocks", clip.get("blocks")},
                                       {"layers", clip.get("layers")},
                                       {"meta", base::make_object({{"units", clip.get("units")}})}});
  const CopyPlan plan = plan_copy(doc, src, std::nullopt, std::nullopt, true, words);
  const CopyResult r = apply_copy(doc, src, plan, true);
  // layers the top-level objects use (applyCopy adds the ones inside blocks)
  PasteResult out;
  out.renamed = r.renamed;
  out.layers = r.layers;
  std::map<std::string, Value> src_layers;
  for (const Value& l : clip.get("layers").as_array()) {
    src_layers[str(l.get("name"))] = l;
  }
  for (const Value& e : clip.get("entities").as_array()) {
    const std::string name = e.get("layer").truthy() ? str(e.get("layer")) : std::string("0");
    if (doc.layers().get(name) == nullptr) {
      const auto it = src_layers.find(name);
      (void)doc.add_layer(it != src_layers.end() ? it->second : base::make_object({{"name", name}}));
      out.layers.push_back(name);
    }
  }
  const bool moved = std::abs(dx) > 0 || std::abs(dy) > 0;
  const cad::Mat2 m{1, 0, 0, 1, dx, dy};
  for (const Value& e0 : clip.get("entities").as_array()) {
    Value e = e0;
    if (e0.get("block").truthy()) {
      for (const auto& [a, b] : r.renamed) {
        if (a == str(e0.get("block"))) {
          e.set("block", b);
          break;
        }
      }
    }
    Value t = moved ? cad::tx_entity(e, m) : json_copy(e);
    t.erase("id");
    out.ids.push_back(static_cast<doc::Id>(num(doc.add(t).get("id"))));
  }
  const std::string a = str(clip.get("units"));
  const std::string b = doc.meta.get("units").truthy() ? str(doc.meta.get("units")) : std::string();
  if (!a.empty() && !b.empty() && a != b) {
    out.note = words.t("clip.units_differ", {{"from", a}, {"to", b}});
  }
  return out;
}

}  // namespace cc::cmd
