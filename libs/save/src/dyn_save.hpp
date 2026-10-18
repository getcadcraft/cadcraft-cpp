// SPDX-License-Identifier: Apache-2.0
// dyn-blocks.js dynSave (CadCraft 0.5): what keeps a drawing's dynamic blocks dynamic when it is saved as DWG.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/doc/doc.hpp"

namespace cc::save::detail {

class DynSave {
 public:
  /// dynSave(doc). `blocks` keeps doc.blocks' order (a JS Map).
  explicit DynSave(const doc::Doc& d);

  [[nodiscard]] bool active() const noexcept { return active_; }
  /// out.blocks.get(name): { …block.dyn, all } or nullptr
  [[nodiscard]] const base::Value* block(const std::string& name) const;
  [[nodiscard]] bool has_blocks() const noexcept { return !blocks_.empty(); }
  /// out.ref(insert): { handle, extDict? } or null; adds the objects the reference needs.
  base::Value ref(const base::Value& e);
  /// out.record(name): the record handle to write the block under, or null.
  [[nodiscard]] base::Value record(const std::string& name) const;
  /// out.objects (call after every reference went through ref)
  [[nodiscard]] base::Array objects() const;
  [[nodiscard]] const base::Value& classes() const noexcept { return classes_; }
  [[nodiscard]] const std::string& version() const noexcept { return version_; }
  [[nodiscard]] const base::Value& maintenance() const noexcept { return maintenance_; }
  [[nodiscard]] double dropped() const noexcept { return dropped_; }

 private:
  bool active_ = false;
  base::Value src_;
  std::vector<std::pair<std::string, base::Value>> blocks_;  // name → dyn (with `all`), in insertion order
  std::map<std::string, base::Value> src_objs_;              // hexKey → object
  std::map<std::string, base::Value> replaced_;
  std::map<std::string, std::string> rec_for_;
  base::Array extra_;
  std::set<std::string> used_;
  base::Value classes_ = base::Value::array();
  base::Value maintenance_;
  std::string version_;
  double dropped_ = 0;
  double fake_ = 0;
  int ver_num_ = 0;
  base::Value rep_class_;  // the ACDB_BLOCKREPRESENTATION_DATA class, or undefined

  std::string new_h();
  base::Value* block_mut(const std::string& name);
  std::string clone_tree(const base::Value& root, const std::string& owner, const base::Value& state,
                         const base::Value& vis_node);
};

/// dyn-blocks.js hexKey(h): '' for nullish; upper case, leading zeros dropped (one digit kept).
[[nodiscard]] std::string hex_key(const base::Value& h);

}  // namespace cc::save::detail
