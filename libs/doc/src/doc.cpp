// SPDX-License-Identifier: Apache-2.0
// doc.js (CadCraft 0.5) in C++. The JS keeps spaces as Maps and undo as before/after per op; so does this.
#include "cc/doc/doc.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>
#include <span>
#include <stdexcept>

#include "cc/base/js.hpp"
#include "cc/cad/files.hpp"
#include "cc/m3d/drawing3d.hpp"

namespace cc::doc {

namespace {

using base::Array;

const Value& member(const Value& o, std::string_view k) {
  static const Value kUndef;
  return o.is_object() && !o.is_point() ? o[k] : kUndef;
}
Value or_(const Value& v, const Value& d) {
  return v.truthy() ? v : d;
}
bool nullish(const Value& v) {
  return v.is_undefined() || v.is_null();
}
Value nn(const Value& v, const Value& d) {
  return nullish(v) ? d : v;
}
bool finite(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number());
}
std::string str(const Value& v) {
  return base::js_string(v);
}
/// String#slice(0, n) in UTF-16 units
std::string slice16(const std::string& s, std::size_t n) {
  if (s.size() <= n) {
    return s;
  }
  const auto u = base::to_utf16(s);
  if (u.size() <= n) {
    return s;
  }
  std::string out;
  base::append_utf16(out, std::span<const std::uint16_t>(u).first(n));
  return out;
}
/// { ...a, ...b }
Value spread(Value a, const Value& b) {
  if (b.is_object()) {
    Value bb = b;
    for (const base::Member& m : bb.as_object()) {
      a.set(m.key, m.value);
    }
  }
  return a;
}
double norm_angle(double a) {
  constexpr double kTau = 2 * std::numbers::pi;
  a = std::fmod(a, kTau);
  return a < 0 ? a + kTau : a;
}

Value clean_ucs(const Value& v) {  // ucs.js cleanUcs
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
  constexpr double kTau = 2 * std::numbers::pi;
  if (std::abs(x) < 1e-12 && std::abs(y) < 1e-12 && std::min(a, kTau - a) < 1e-12) {
    return {nullptr};  // the world
  }
  return base::make_object({{"x", x}, {"y", y}, {"ang", a}});
}
Value ucs_data(const Value& v) {  // ucs.js ucsData
  if (!v.is_object()) {
    return {nullptr};
  }
  const double x = base::js_to_number(v.get("x"));
  const double y = base::js_to_number(v.get("y"));
  const double ang = base::js_to_number(v.get("ang"));
  if (std::isfinite(x) && std::isfinite(y) && std::isfinite(ang) && std::abs(x) <= 1e15 && std::abs(y) <= 1e15) {
    return base::make_object({{"x", x}, {"y", y}, {"ang", norm_angle(ang)}});
  }
  return {nullptr};
}
bool ucs_name_ok(const std::string& n) {  // /^[^<>/\\":;?*|,=`]{1,255}$/
  const std::size_t len = base::utf16_length(n);
  if (len < 1 || len > 255) {
    return false;
  }
  return n.find_first_of("<>/\\\":;?*|,=`") == std::string::npos;
}

}  // namespace

Doc::Doc() {
  meta = base::make_object({{"name", "Drawing1"}, {"units", ""}, {"source", ""}, {"version", ""}});
  current = base::make_object({{"layer", "0"},
                               {"color", Value(nullptr)},
                               {"textHeight", 2.5},
                               {"dim", cad::dim_defaults()},
                               {"linetype", Value(nullptr)},
                               {"lineweight", Value(nullptr)}});
  params = base::make_object({{"vars", Array{}}, {"links", Array{}}});
  vports = Value(nullptr);
  ucs = Value(nullptr);
  dyn_src = Value(nullptr);
  (void)add_layer(base::make_object({{"name", "0"}, {"color", 7}}));
}

const Layout* Doc::layout() const noexcept {
  for (const Layout& l : layouts_) {
    if (l.entities == space_) {
      return &l;
    }
  }
  return nullptr;
}

void Doc::set_space(std::string_view layout_name) {
  MapPtr next = model_;
  if (!layout_name.empty()) {
    for (const Layout& l : layouts_) {
      if (l.name == layout_name) {
        next = l.entities;
        break;
      }
    }
  }
  space_ = next;
}

// ---------------------------------------------------------------- layers

Value Doc::add_layer(const Value& l) {
  const Value lt = l.get("linetype");
  const Value lw = l.get("lineweight");
  Value layer = base::make_object({{"name", l.get("name")},
                                   {"color", nn(l.get("color"), 7)},
                                   {"on", nn(l.get("on"), true)},
                                   {"frozen", l.get("frozen").truthy()},
                                   {"locked", l.get("locked").truthy()},
                                   {"plot", nn(l.get("plot"), true)},
                                   {"linetype", lt.is_string() && !lt.as_string().empty() ? lt : Value("Continuous")},
                                   {"lineweight", finite(lw) ? lw : Value(-3)}});
  if (l.get("newVpFrozen").truthy()) {
    layer.set("newVpFrozen", true);
  }
  layers_.set(str(l.get("name")), layer);
  return layer;
}

const Value& Doc::layer(const std::string& name) {
  if (const Value* l = layers_.get(name)) {
    return *l;
  }
  (void)add_layer(base::make_object({{"name", name}}));
  return *layers_.get(name);
}

bool Doc::visible(const Value& e) const {
  const Value* l = layers_.get(str(e.get("layer")));
  return l == nullptr || (l->get("on").truthy() && !l->get("frozen").truthy());
}
bool Doc::editable(const Value& e) const {
  const Value* l = layers_.get(str(e.get("layer")));
  return visible(e) && (l == nullptr || !l->get("locked").truthy());
}

Value Doc::layer_state() const {
  Array list;
  layers_.each([&](const std::string&, const Value& l) { list.push_back(l); });
  return base::make_object({{"list", std::move(list)}, {"current", current.get("layer")}});
}

void Doc::set_layer_state(const Value& state) {
  must_be_in_change();
  Op op;
  op.kind = Op::Kind::layer_state;
  op.before = layer_state();
  layers_.clear();
  for (const Value& l : member(state, "list").as_array()) {
    layers_.set(str(l.get("name")), l);
  }
  if (!layers_.has("0")) {
    (void)add_layer(base::make_object({{"name", "0"}, {"color", 7}}));
  }
  const Value cur = state.get("current");
  current.set("layer", layers_.has(str(cur)) ? cur : Value("0"));
  op.after = layer_state();
  pending_ops().push_back(std::move(op));
}

void Doc::set_layer(const std::string& name, const Value& patch, bool make_current) {
  const Value pn = patch.get("name");
  const bool rename = pn.truthy() && pn != Value(name);
  change(rename ? "RENAME LAYER" : "LAYER", [&] {
    Value state = layer_state();
    Array& list = state.find("list")->as_array();
    Value* l = nullptr;
    for (Value& x : list) {
      if (x.get("name") == Value(name)) {
        l = &x;
        break;
      }
    }
    if (l == nullptr) {
      list.push_back(base::make_object(
          {{"name", name}, {"color", 7}, {"on", true}, {"frozen", false}, {"locked", false}, {"plot", true}}));
      l = &list.back();
    }
    for (const base::Member* m : base::js_members(patch.as_object())) {
      l->set(m->key, m->value);
    }
    if (make_current) {
      l->set("on", true);
      l->set("frozen", false);
      state.set("current", l->get("name"));
    }
    if (rename) {
      const std::string to = str(pn);
      if (state.get("current") == Value(name)) {
        state.set("current", to);
      }
      std::vector<MapPtr> maps = {model_};
      for (const Layout& lo : layouts_) {
        maps.push_back(lo.entities);
      }
      for (const MapPtr& map : maps) {
        std::vector<std::pair<Id, Value>> todo;
        map->each([&](const Id& id, const Value& e) {
          const Value frz = e.get("frz");
          bool in_frz = false;
          if (frz.is_array()) {
            in_frz = std::ranges::find(frz.as_array(), Value(name)) != frz.as_array().end();
          }
          const bool on_layer = e.get("layer") == Value(name);
          if (on_layer || in_frz) {
            Value next = e;
            if (on_layer) {
              next.set("layer", to);
            }
            if (in_frz) {
              Array f;
              for (const Value& n : frz.as_array()) {
                f.push_back(n == Value(name) ? Value(to) : n);
              }
              next.set("frz", std::move(f));
            }
            todo.emplace_back(id, std::move(next));
          }
        });
        for (auto& [id, v] : todo) {
          (void)update(id, v, map);
        }
      }
      std::vector<Value> blocks;
      blocks_.each([&](const std::string&, const Value& b) {
        bool any = false;
        for (const Value& e : member(b, "entities").as_array()) {
          any = any || e.get("layer") == Value(name);
        }
        if (any) {
          Value nb = b;
          for (Value& e : nb.find("entities")->as_array()) {
            if (e.get("layer") == Value(name)) {
              e.set("layer", to);
            }
          }
          blocks.push_back(std::move(nb));
        }
      });
      for (const Value& b : blocks) {
        (void)define_block(b);
      }
    }
    set_layer_state(state);
  });
}

// ---------------------------------------------------------------- changes

void Doc::must_be_in_change() const {
  if (!pending_) {
    throw std::logic_error("Edits go through doc.change()");
  }
}

std::vector<Op>& Doc::pending_ops() {
  if (!pending_) {
    throw std::logic_error("Edits go through doc.change()");
  }
  return pending_->ops;
}

void Doc::change(std::string label, const std::function<void()>& fn) {
  if (pending_) {
    fn();  // nested: part of the outer change
    return;
  }
  pending_ = Step{std::move(label), {}};
  try {
    fn();
  } catch (...) {
    rollback(pending_ops());
    pending_.reset();
    throw;
  }
  if (!pending_->ops.empty() && !edit_hooks.empty()) {
    // add-ons' follow-up edits (doc.js editHooks): inside this step; a hook that fails is skipped, as in 0.5
    const std::vector<Op> ops = pending_->ops;
    const std::vector<EditHook> hooks = edit_hooks;
    for (const EditHook& h : hooks) {
      try {
        h.fn(*this, ops);
      } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch) — console.error in 0.5
      }
    }
  }
  Step step = std::move(*pending_);
  pending_.reset();
  if (!step.ops.empty()) {
    undo_.push_back(std::move(step));
    if (undo_.size() > 500) {
      undo_.erase(undo_.begin());
    }
    redo_.clear();
    ++version_;
  }
}

const Value& Doc::add(const Value& e, const MapPtr& map_in) {
  must_be_in_change();
  const MapPtr map = map_or_space(map_in);
  Value ent = spread(base::make_object({{"color", Value(nullptr)}}), e);
  ent.set("layer", nn(e.get("layer"), current.get("layer")));
  const Id id = next_id_++;
  ent.set("id", static_cast<double>(id));
  (void)layer(str(ent.get("layer")));
  map->set(id, ent);
  Op op;
  op.kind = Op::Kind::entity;
  op.map = map;
  op.id = id;
  op.after = ent;
  pending_ops().push_back(std::move(op));
  return *map->get(id);
}

const Value* Doc::update(Id id, const Value& e, const MapPtr& map_in) {
  must_be_in_change();
  const MapPtr map = map_or_space(map_in);
  const Value* before = map->get(id);
  if (before == nullptr) {
    return nullptr;
  }
  Op op;
  op.kind = Op::Kind::entity;
  op.map = map;
  op.id = id;
  op.before = *before;
  Value after = e;
  after.set("id", static_cast<double>(id));
  op.after = after;
  map->set(id, std::move(after));
  pending_ops().push_back(std::move(op));
  return map->get(id);
}

void Doc::remove(Id id, const MapPtr& map_in) {
  must_be_in_change();
  const MapPtr map = map_or_space(map_in);
  const Value* before = map->get(id);
  if (before == nullptr) {
    return;
  }
  Op op;
  op.kind = Op::Kind::entity;
  op.map = map;
  op.id = id;
  op.before = *before;
  map->erase(id);
  pending_ops().push_back(std::move(op));
}

const Value& Doc::define_block(const Value& b) {
  must_be_in_change();
  const std::string name = str(b.get("name"));
  Op op;
  op.kind = Op::Kind::block;
  op.name = name;
  if (const Value* before = blocks_.get(name)) {
    op.before = *before;
  }
  const Value base = b.get("base");
  Array ents;
  for (const Value& e : member(b, "entities").as_array()) {
    Value c = e;
    c.erase("id");
    ents.push_back(std::move(c));
  }
  Value after =
      base::make_object({{"name", b.get("name")},
                         {"base", base::make_object({{"x", or_(base.truthy() ? base.get("x") : Value(), 0)},
                                                     {"y", or_(base.truthy() ? base.get("y") : Value(), 0)}})},
                         {"entities", std::move(ents)}});
  if (const Value ad = b.get("attdefs"); ad.is_array() && !ad.as_array().empty()) {
    after.set("attdefs", ad);
  }
  if (const Value x = b.get("xref"); x.is_object()) {
    after.set("xref", x);
  }
  if (const Value d = b.get("dyn"); d.is_object()) {
    after.set("dyn", d);
  }
  op.after = after;
  blocks_.set(name, std::move(after));
  pending_ops().push_back(std::move(op));
  return *blocks_.get(name);
}

void Doc::remove_block(const std::string& name) {
  must_be_in_change();
  const Value* before = blocks_.get(name);
  if (before == nullptr) {
    return;
  }
  Op op;
  op.kind = Op::Kind::block;
  op.name = name;
  op.before = *before;
  blocks_.erase(name);
  pending_ops().push_back(std::move(op));
}

void Doc::set_meta(const Value& patch) {
  must_be_in_change();
  Op op;
  op.kind = Op::Kind::meta;
  Value before = Value::object();
  for (const base::Member* m : base::js_members(patch.as_object())) {
    before.set(m->key, meta.get(m->key));
  }
  op.before = std::move(before);
  op.after = patch;
  for (const base::Member* m : base::js_members(patch.as_object())) {
    meta.set(m->key, m->value);
  }
  pending_ops().push_back(std::move(op));
}

void Doc::set_style(std::string_view table, const std::string& name, const Value& value) {
  must_be_in_change();
  if (table != "dimStyles" && table != "textStyles") {
    throw std::logic_error("setStyle: dimStyles or textStyles");
  }
  OrderedMap<std::string>& map = table == "dimStyles" ? dim_styles : text_styles;
  Op op;
  op.kind = Op::Kind::style;
  op.table = std::string(table);
  op.name = name;
  if (const Value* b = map.get(name)) {
    op.before = *b;
  }
  if (value.truthy()) {
    op.after = value;
    map.set(name, value);
  } else {
    map.erase(name);
  }
  pending_ops().push_back(std::move(op));
}

void Doc::set_views(Value list) {
  must_be_in_change();
  Op op;
  op.kind = Op::Kind::views;
  op.before = views;
  op.after = list;
  views = std::move(list);
  pending_ops().push_back(std::move(op));
}

void Doc::set_layouts(std::vector<Layout> list) {
  must_be_in_change();
  Op op;
  op.kind = Op::Kind::layouts;
  op.layouts_before = layouts_;
  op.layouts_after = list;
  layouts_ = std::move(list);
  if (!in_model() && std::ranges::none_of(layouts_, [&](const Layout& l) { return l.entities == space_; })) {
    space_ = model_;
  }
  pending_ops().push_back(std::move(op));
}

void Doc::apply(const Op& op, bool forward) {
  const Value& v = forward ? op.after : op.before;
  switch (op.kind) {
    case Op::Kind::layouts: {
      layouts_ = forward ? op.layouts_after : op.layouts_before;
      if (!in_model() && std::ranges::none_of(layouts_, [&](const Layout& l) { return l.entities == space_; })) {
        space_ = model_;
        space_lost_ = true;
      }
      break;
    }
    case Op::Kind::parametric:
      constraints = v.get("constraints");
      params = v.get("params");
      break;
    case Op::Kind::meta:
      for (const base::Member* m : base::js_members(v.as_object())) {
        meta.set(m->key, m->value);
      }
      break;
    case Op::Kind::views: views = v; break;
    case Op::Kind::layer_state:
      layers_.clear();
      for (const Value& l : member(v, "list").as_array()) {
        layers_.set(str(l.get("name")), l);
      }
      current.set("layer", v.get("current"));
      break;
    case Op::Kind::style: {
      OrderedMap<std::string>& map = op.table == "dimStyles" ? dim_styles : text_styles;
      if (v.truthy()) {
        map.set(op.name, v);
      } else {
        map.erase(op.name);
      }
      break;
    }
    case Op::Kind::block:
      if (!v.is_undefined() && !v.is_null()) {
        blocks_.set(op.name, v);
      } else {
        blocks_.erase(op.name);
      }
      break;
    case Op::Kind::entity:
      if (!v.is_undefined() && !v.is_null()) {
        op.map->set(op.id, v);
      } else {
        op.map->erase(op.id);
      }
      break;
  }
}

void Doc::rollback(const std::vector<Op>& ops) {
  for (const auto& op : std::views::reverse(ops)) {
    apply(op, false);
  }
}

void Doc::squash(std::size_t mark, std::string label) {
  if (undo_.size() < mark + 2) {
    if (undo_.size() > mark) {
      undo_[mark].label = std::move(label);
    }
    return;
  }
  Step merged{std::move(label), {}};
  for (std::size_t i = mark; i < undo_.size(); ++i) {
    for (Op& op : undo_[i].ops) {
      merged.ops.push_back(std::move(op));
    }
  }
  undo_.resize(mark);
  undo_.push_back(std::move(merged));
}

std::vector<std::string> Doc::undo_labels() const {
  std::vector<std::string> out;
  out.reserve(undo_.size());
  for (const Step& s : undo_) {
    out.push_back(s.label);
  }
  return out;
}

std::vector<std::string> Doc::redo_labels() const {
  std::vector<std::string> out;
  out.reserve(redo_.size());
  for (const Step& s : redo_) {
    out.push_back(s.label);
  }
  return out;
}

std::optional<std::string> Doc::undo() {
  if (undo_.empty()) {
    return std::nullopt;
  }
  Step step = std::move(undo_.back());
  undo_.pop_back();
  rollback(step.ops);
  std::string label = step.label;
  redo_.push_back(std::move(step));
  ++version_;
  space_lost_ = false;
  return label;
}

std::optional<std::string> Doc::redo() {
  if (redo_.empty()) {
    return std::nullopt;
  }
  Step step = std::move(redo_.back());
  redo_.pop_back();
  for (const Op& op : step.ops) {
    apply(op, true);
  }
  std::string label = step.label;
  undo_.push_back(std::move(step));
  ++version_;
  space_lost_ = false;
  return label;
}

void Doc::each_entity(const std::function<void(const Value&)>& f) const {
  model_->each([&](const Id&, const Value& e) { f(e); });
  for (const Layout& l : layouts_) {
    l.entities->each([&](const Id&, const Value& e) { f(e); });
  }
}

std::size_t Doc::block_uses(std::string_view name) const {
  std::size_t n = 0;
  const Value want(std::string{name});
  each_entity([&](const Value& e) { n += e.get("block") == want ? std::size_t{1} : std::size_t{0}; });
  blocks_.each([&](const std::string&, const Value& b) {
    for (const Value& e : member(b, "entities").as_array()) {
      n += e.get("block") == want ? std::size_t{1} : std::size_t{0};
    }
  });
  return n;
}

// ---------------------------------------------------------------- load

void Doc::load(Value drawing, const i18n::Catalog& words) {
  const Value tw = drawing.get("viewTwist");
  view_twist = finite(tw) && std::abs(tw.as_number()) <= 100 ? tw.as_number() : 0;
  const Value u = drawing.get("ucs");
  ucs = clean_ucs(u.truthy() ? u.get("current") : Value());
  ucs_named = Array{};
  if (const Value named = u.truthy() ? u.get("named") : Value(); named.is_array()) {
    for (const Value& n : named.as_array()) {
      if (ucs_named.as_array().size() >= 1000) {
        break;
      }
      const Value nm = n.truthy() ? n.get("name") : Value();
      const Value data = ucs_data(n);
      if (nm.is_string() && ucs_name_ok(nm.as_string()) && !data.is_null()) {
        ucs_named.push(spread(base::make_object({{"name", nm}}), data));
      }
    }
  }
  // 3DFACEs and CadCraft's 3D model are not 2D objects (model3d.js split3d; kept for the 3D model, M5)
  const Value in3d = cad::split3d(drawing, words);
  model3d_text = in3d.get("text").is_string() ? in3d.get("text").as_string() : std::string();
  vports = cad::clean_panes(drawing.get("vports"));
  model_->clear();
  layers_.clear();
  blocks_.clear();
  layouts_.clear();
  space_ = model_;
  next_id_ = 1;
  (void)add_layer(base::make_object({{"name", "0"}, {"color", 7}}));
  if (const Value ls = drawing.get("layers"); ls.is_array()) {
    for (const Value& l : ls.as_array()) {
      (void)add_layer(l);
    }
  }
  if (const Value bs = drawing.get("blocks"); bs.is_array()) {
    for (const Value& b : bs.as_array()) {
      Value o = base::make_object(
          {{"name", b.get("name")}, {"base", or_(b.get("base"), base::pt2(0, 0))}, {"entities", b.get("entities")}});
      if (const Value ad = b.get("attdefs"); ad.is_array() && !ad.as_array().empty()) {
        o.set("attdefs", ad);
      }
      if (const Value x = b.get("xref"); x.is_object()) {
        const Value p = x.get("path");
        o.set("xref", base::make_object({{"path", slice16(p.truthy() ? str(p) : std::string(), 1024)}}));
      }
      if (const Value d = b.get("dyn"); d.is_object()) {
        o.set("dyn", d);
      }
      blocks_.set(str(b.get("name")), std::move(o));
    }
  }
  const Value ds = drawing.get("dynSrc");
  dyn_src = ds.truthy() && ds.get("objects").is_array() ? ds : Value(nullptr);
  const auto fill = [&](EntityMap& map, const Value& list) {
    if (!list.is_array()) {
      return;
    }
    for (const Value& e : list.as_array()) {
      Value ent = spread(base::make_object({{"color", Value(nullptr)}}), e);
      ent.set("layer", or_(e.get("layer"), "0"));
      const Id id = next_id_++;
      ent.set("id", static_cast<double>(id));
      (void)layer(str(ent.get("layer")));
      map.set(id, std::move(ent));
    }
  };
  fill(*model_, drawing.get("entities"));
  // CadCraft's constraint keys: dropped from the entities (resolving them is gcons.js, not ported: docs/gaps.md)
  {
    std::vector<std::pair<Id, Value>> todo;
    model_->each([&](const Id& id, const Value& e) {
      if (e.has("ccref")) {
        Value c = e;
        c.erase("ccref");
        todo.emplace_back(id, std::move(c));
      }
    });
    for (auto& [id, v] : todo) {
      model_->set(id, std::move(v));
    }
  }
  constraints = Array{};
  params = base::make_object({{"vars", Array{}}, {"links", Array{}}});
  const Value pt = drawing.get("parametric");
  parametric_text = pt.is_string() ? pt.as_string() : std::string();
  if (const Value lo = drawing.get("layouts"); lo.is_array()) {
    for (const Value& l : lo.as_array()) {
      auto map = std::make_shared<EntityMap>();
      fill(*map, or_(l.get("entities"), Array{}));
      layouts_.push_back(Layout{str(l.get("name")), spread(base::make_object({{"x0", 0}, {"y0", 0}}), l.get("paper")),
                                std::move(map)});
    }
  }
  Array vs;
  if (const Value vl = drawing.get("views"); vl.is_array()) {
    for (const Value& v : vl.as_array()) {
      const Value name = v.truthy() ? v.get("name") : Value();
      const Value cx = v.get("cx");
      const Value cy = v.get("cy");
      const Value w = v.get("w");
      const Value h = v.get("h");
      if (!name.is_string() || name.as_string().empty() || !finite(cx) || !finite(cy) || !finite(w) || !finite(h) ||
          !(w.as_number() > 0) || !(h.as_number() > 0)) {
        continue;
      }
      Value o =
          base::make_object({{"name", slice16(name.as_string(), 255)}, {"cx", cx}, {"cy", cy}, {"w", w}, {"h", h}});
      if (const Value lay = v.get("layout"); lay.is_string()) {
        o.set("layout", lay);
      }
      vs.push_back(std::move(o));
    }
  }
  views = std::move(vs);
  meta = spread(base::make_object({{"name", "Drawing1"}, {"units", ""}, {"source", ""}, {"version", ""}}),
                drawing.get("meta"));
  {  // this.model3d.fromFile(in3d, this.meta.units, report)
    const Value mu = meta.get("units");
    const auto got =
        m3d::from_file(model3d, in3d, mu.is_string() ? mu.as_string() : std::string(), words.t("model3d.faces_file"));
    if (Value* rep = drawing.find("report"); rep != nullptr && rep->is_object()) {
      if (Value* notes = rep->find("notes"); notes != nullptr && notes->is_array()) {
        if (got.loose > 0) {
          notes->push(words.tn("model3d.read.loose_faces", static_cast<double>(got.loose)));
        }
        if (got.left_out > 0) {
          notes->push(words.tn("model3d.read.faces_left_out", static_cast<double>(got.left_out)));
        }
      }
    }
  }
  current.set("layer", "0");
  current.set("linetype", Value(nullptr));
  current.set("lineweight", Value(nullptr));
  linetypes.clear();
  if (const Value lts = drawing.get("linetypes"); lts.is_array()) {
    for (const Value& l : lts.as_array()) {
      const Value name = l.truthy() ? l.get("name") : Value();
      const Value pat = l.get("pattern");
      if (!name.is_string() || name.as_string().empty() || !pat.is_array() || pat.as_array().size() > 64 ||
          !std::ranges::all_of(pat.as_array(), [](const Value& x) { return finite(x); })) {
        continue;
      }
      const Value d = l.get("description");
      linetypes.set(
          name.as_string(),
          base::make_object(
              {{"name", name}, {"description", slice16(d.truthy() ? str(d) : std::string(), 255)}, {"pattern", pat}}));
    }
  }
  current.set("dim", spread(cad::dim_defaults(), or_(drawing.get("dimStyle"), Value::object())));
  // a file's text and dimension styles (untrusted: names and plain values only)
  const auto named = [](const Value& m, const std::function<void(const std::string&, const Value&)>& f) {
    if (!m.is_array()) {
      return;
    }
    std::size_t n = 0;
    for (const Value& pair : m.as_array()) {
      const Value k = pair.is_array() && !pair.as_array().empty() ? pair.as_array()[0] : Value();
      const Value v = pair.is_array() && pair.as_array().size() > 1 ? pair.as_array()[1] : Value();
      if (!k.is_string() || k.as_string().empty() || base::utf16_length(k.as_string()) > 255 || !v.truthy() ||
          !(v.is_object() || v.is_array())) {
        continue;
      }
      if (n++ >= 1000) {
        break;
      }
      f(k.as_string(), v);
    }
  };
  text_styles.clear();
  named(drawing.get("textStyles"), [&](const std::string& n, const Value& v) {
    const Value font = v.get("font");
    const Value h = v.get("h");
    const Value wf = v.get("wf");
    const Value obl = v.get("obl");
    text_styles.set(n, base::make_object({{"font", slice16(font.truthy() ? str(font) : std::string(), 255)},
                                          {"h", finite(h) && h.as_number() >= 0 ? h : Value(0)},
                                          {"wf", finite(wf) && wf.as_number() > 0 ? wf : Value(1)},
                                          {"obl", finite(obl) ? obl : Value(0)}}));
  });
  dim_styles.clear();
  named(drawing.get("dimStyles"), [&](const std::string& n, const Value& v) {
    Value o = Value::object();
    if (v.is_object()) {
      for (const base::Member* m : base::js_members(v.as_object())) {
        const Value& x = m->value;
        const bool plain = x.is_null() || x.is_string() || x.is_bool() || finite(x);
        if (plain) {
          o.set(m->key, x);
        }
      }
    }
    dim_styles.set(n, std::move(o));
  });
  undo_.clear();
  redo_.clear();
  ++version_;
  saved_version_ = version_;
}

Value Doc::dump() const {
  const auto entries_num = [](const EntityMap& m) {
    Array out;
    m.each([&](const Id& id, const Value& e) { out.emplace_back(Array{static_cast<double>(id), e}); });
    return out;
  };
  const auto entries_str = [](const OrderedMap<std::string>& m) {
    Array out;
    m.each([&](const std::string& k, const Value& v) { out.emplace_back(Array{k, v}); });
    return out;
  };
  Array lays;
  for (const Layout& l : layouts_) {
    lays.push_back(base::make_object({{"name", l.name}, {"paper", l.paper}, {"entities", entries_num(*l.entities)}}));
  }
  return base::make_object({{"model", entries_num(*model_)},
                            {"layers", entries_str(layers_)},
                            {"blocks", entries_str(blocks_)},
                            {"layouts", std::move(lays)},
                            {"views", views},
                            {"meta", meta},
                            {"current", current},
                            {"linetypes", entries_str(linetypes)},
                            {"textStyles", entries_str(text_styles)},
                            {"dimStyles", entries_str(dim_styles)},
                            {"vports", vports},
                            {"ucs", ucs},
                            {"ucsNamed", ucs_named},
                            {"viewTwist", view_twist},
                            {"dynSrc", dyn_src},
                            {"nextId", static_cast<double>(next_id_)}});
}

}  // namespace cc::doc
