// SPDX-License-Identifier: Apache-2.0
// cc::doc — L3. The drawing a CadCraft session edits: CadCraft 0.5's doc.js (Doc), ported so that loading a drawing,
// editing it and undoing give the same document as the JS (tools/parity/doc.mjs; M4.2).
//
//   cc::doc::Doc d;
//   d.load(cc::cad::import_dwg(db, tag, words), words);    // replace everything (not undoable)
//   d.change("MOVE", [&] { d.update(id, moved); });       // every edit: one undo step
//   d.undo();
//
// Entities are JS-shaped values (base::Value) in world units, with `id`, `layer` and `color` (null by layer, 0 by
// block, 1–255 ACI, '#rrggbb'); the types and their fields are doc.js's (line, circle, arc, poly, ellipse, text, point,
// insert, hatch, dim, viewport, mleader, xline, ray). Spaces: `model` holds the drawing, each layout its paper-space
// entities; `entities()` is whichever is open.
//
// Not ported yet (docs/gaps.md): constraints and parameters (a file's CADCRAFT_PARAMETRIC payload is kept as text),
// reading CadCraft's own copy of the 3D model back (a file's 3D faces come in as loose faces), Studio Live's hook,
// add-on edit hooks.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"
#include "cc/m3d/model.hpp"

namespace cc::doc {

using base::Value;

/// A JS Map: insertion order, `set` of an existing key keeps its place, a deleted key set again goes last.
template <class K>
class OrderedMap {
 public:
  [[nodiscard]] const Value* get(const K& k) const {
    const auto it = index_.find(k);
    if (it == index_.end()) {
      return nullptr;
    }
    const auto& slot = slots_[it->second];
    return slot ? &slot->second : nullptr;
  }
  [[nodiscard]] bool has(const K& k) const { return index_.contains(k); }
  void set(const K& k, Value v) {
    if (const auto it = index_.find(k); it != index_.end()) {
      if (auto& slot = slots_[it->second]) {
        slot->second = std::move(v);
        return;
      }
    }
    index_.emplace(k, slots_.size());
    slots_.emplace_back(std::in_place, k, std::move(v));
  }
  bool erase(const K& k) {
    const auto it = index_.find(k);
    if (it == index_.end()) {
      return false;
    }
    slots_[it->second].reset();
    index_.erase(it);
    if (slots_.size() > 64 && index_.size() * 2 < slots_.size()) {
      compact();
    }
    return true;
  }
  void clear() {
    slots_.clear();
    index_.clear();
  }
  [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
  /// Every entry in order: f(key, value).
  template <class F>
  void each(const F& f) const {
    for (const auto& s : slots_) {
      if (s) {
        f(s->first, s->second);
      }
    }
  }

 private:
  std::vector<std::optional<std::pair<K, Value>>> slots_;
  std::unordered_map<K, std::size_t> index_;
  void compact() {
    std::vector<std::optional<std::pair<K, Value>>> kept;
    kept.reserve(index_.size());
    for (auto& s : slots_) {
      if (s) {
        index_[s->first] = kept.size();
        kept.push_back(std::move(s));
      }
    }
    slots_ = std::move(kept);
  }
};

using Id = std::int64_t;
using EntityMap = OrderedMap<Id>;
using MapPtr = std::shared_ptr<EntityMap>;

struct Layout {
  std::string name;
  Value paper;      // { w, h, unit: 'mm' | 'in', x0, y0, key? }
  MapPtr entities;  // shared by the undo steps that list this layout
};

/// One recorded edit (doc.js `pending.ops` entries). before / after: undefined = absent.
struct Op {
  enum class Kind : std::uint8_t { entity, block, layouts, meta, views, layer_state, style, parametric };
  Kind kind = Kind::entity;
  MapPtr map;  // entity: the space it is in
  Id id = 0;
  std::string name;   // block / style name
  std::string table;  // style: "dimStyles" | "textStyles"
  Value before, after;
  std::vector<Layout> layouts_before, layouts_after;
};

struct Step {
  std::string label;
  std::vector<Op> ops;
};

class Doc {
 public:
  Doc();

  // ---- spaces
  [[nodiscard]] EntityMap& entities() noexcept { return *space_; }
  [[nodiscard]] const EntityMap& entities() const noexcept { return *space_; }
  [[nodiscard]] const MapPtr& space() const noexcept { return space_; }
  [[nodiscard]] const MapPtr& model() const noexcept { return model_; }
  [[nodiscard]] bool in_model() const noexcept { return space_ == model_; }
  [[nodiscard]] const std::vector<Layout>& layouts() const noexcept { return layouts_; }
  /// The open layout, or nullptr in model space.
  [[nodiscard]] const Layout* layout() const noexcept;
  /// Open the model (empty name) or a layout by name. Not an edit.
  void set_space(std::string_view layout_name = {});

  // ---- layers, blocks, styles
  [[nodiscard]] const OrderedMap<std::string>& layers() const noexcept { return layers_; }
  [[nodiscard]] const OrderedMap<std::string>& blocks() const noexcept { return blocks_; }
  /// addLayer(l): a layer with doc.js's defaults (not an edit).
  Value add_layer(const Value& l);
  /// layer(name): the layer, made when missing.
  const Value& layer(const std::string& name);
  /// setLayer(name, patch, {current}): one undo step (or part of the change it runs in); a new name moves the
  /// objects on the layer (model, sheets, blocks) in the same step.
  void set_layer(const std::string& name, const Value& patch, bool current = false);
  [[nodiscard]] bool visible(const Value& e) const;
  [[nodiscard]] bool editable(const Value& e) const;

  // ---- changes (inside change() only)
  /// change(label, fn): fn's edits are one undo step; a nested change is part of the outer one; an exception from
  /// fn rolls its edits back and goes on to the caller.
  void change(std::string label, const std::function<void()>& fn);
  /// add(e, map): e gets the next id and the current layer when it has none. Returns the stored entity.
  const Value& add(const Value& e, const MapPtr& map = nullptr);
  const Value* update(Id id, const Value& e, const MapPtr& map = nullptr);
  void remove(Id id, const MapPtr& map = nullptr);
  const Value& define_block(const Value& b);
  void remove_block(const std::string& name);
  void set_meta(const Value& patch);
  void set_style(std::string_view table, const std::string& name, const Value& value);
  void set_views(Value list);
  void set_layouts(std::vector<Layout> list);
  void set_layer_state(const Value& state);
  [[nodiscard]] Value layer_state() const;

  // ---- history
  [[nodiscard]] std::size_t mark() const noexcept { return undo_.size(); }
  void squash(std::size_t mark, std::string label);
  [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
  [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
  /// The label of the step undone / redone, or nullopt when there is none.
  std::optional<std::string> undo();
  std::optional<std::string> redo();
  /// The undo and redo steps' labels, oldest first (doc.js undoStack / redoStack).
  [[nodiscard]] std::vector<std::string> undo_labels() const;
  [[nodiscard]] std::vector<std::string> redo_labels() const;
  /// `redoStack.length = 0` (a command's own undo, commands.js undoInside).
  void clear_redo() noexcept { redo_.clear(); }
  /// The id the next added entity gets (doc.js nextId).
  [[nodiscard]] Id next_id() const noexcept { return next_id_; }
  [[nodiscard]] std::uint64_t version() const noexcept { return version_; }
  [[nodiscard]] bool dirty() const noexcept { return version_ != saved_version_; }
  void mark_saved() noexcept { saved_version_ = version_; }

  // ---- reading
  /// Every entity, model first then each layout's (for saving and statistics).
  void each_entity(const std::function<void(const Value&)>& f) const;
  /// Inserts (and dimensions) that use a block, in any space or inside other blocks.
  [[nodiscard]] std::size_t block_uses(std::string_view name) const;

  /// load(drawing): replace everything (open / new). Not undoable. `words` writes the notes it adds to the drawing's
  /// report (in place, as the JS does).
  void load(Value drawing, const i18n::Catalog& words);
  /// The document as tools/parity/doc.mjs compares it with the JS (maps as [key, value] lists).
  [[nodiscard]] Value dump() const;

  // the rest of doc.js's fields, as the JS keeps them
  Value meta;     // { name, units, source, version, … }
  Value current;  // { layer, color, textHeight, dim, linetype, lineweight }
  Value views = base::Value::array();
  Value vports;  // panes.js shape or null
  Value ucs;     // { x, y, ang } or null (the world)
  Value ucs_named = base::Value::array();
  double view_twist = 0;
  Value dyn_src;  // the file's dynamic-block objects, or null
  OrderedMap<std::string> linetypes, text_styles, dim_styles;
  Value constraints = base::Value::array();  // gcons.js (not ported: docs/gaps.md)
  Value params;                              // params.js { vars, links }
  std::string parametric_text;               // a file's CADCRAFT_PARAMETRIC payload, kept for saving
  m3d::Model model3d;                        // the 3D model (model3d.js): the file's 3D faces as one group
  std::string model3d_text;                  // a file's CADCRAFT_3D JSON (not read back yet: docs/gaps.md)

 private:
  MapPtr model_ = std::make_shared<EntityMap>();
  MapPtr space_ = model_;
  std::vector<Layout> layouts_;
  OrderedMap<std::string> layers_;
  OrderedMap<std::string> blocks_;
  Id next_id_ = 1;
  std::optional<Step> pending_;
  std::vector<Step> undo_, redo_;
  std::uint64_t version_ = 0, saved_version_ = 0;
  bool space_lost_ = false;

  void must_be_in_change() const;
  /// The ops of the change being made (throws outside change()).
  std::vector<Op>& pending_ops();
  void apply(const Op& op, bool forward);
  void rollback(const std::vector<Op>& ops);
  [[nodiscard]] MapPtr map_or_space(const MapPtr& m) const { return m ? m : space_; }
};

}  // namespace cc::doc
