// SPDX-License-Identifier: Apache-2.0
// JS Map and Set semantics for the kernel's tables, because the kernel's results depend on them: entries in insertion
// order, a deleted key set again goes to the end, and a loop over the table sees what is added during the loop and
// not what is deleted (as `for (const [k, v] of map)` does). Ids are the kernel's integer ids (vertices, edges, faces,
// instances, guides).
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cc::m3d {

using Id = std::int64_t;

/// A JS Map keyed by K. Slots are kept in insertion order with holes where entries were deleted; `compact()` drops
/// the holes (callers do it between operations, never inside a loop over the map).
/// An optional the kernel's logic guarantees (a context path it just checked, a curve it just found): a missing one
/// is a kernel bug, reported as std::logic_error to the edit guard (which restores the model).
template <class T>
T& must(std::optional<T>& o) {
  if (!o.has_value()) {
    throw std::logic_error("m3d: a value the kernel needs is missing");
  }
  return *o;
}
template <class T>
const T& must(const std::optional<T>& o) {
  if (!o.has_value()) {
    throw std::logic_error("m3d: a value the kernel needs is missing");
  }
  return *o;
}

template <class K, class T>
class OrderedMap {
 public:
  [[nodiscard]] const T* get(const K& k) const {
    const auto it = index_.find(k);
    return it == index_.end() ? nullptr : &slots_[it->second].second;
  }
  [[nodiscard]] T* get(const K& k) {
    const auto it = index_.find(k);
    return it == index_.end() ? nullptr : &slots_[it->second].second;
  }
  [[nodiscard]] bool has(const K& k) const { return index_.contains(k); }
  /// map.set(k, v): an existing key keeps its place.
  T& set(const K& k, T v) {
    if (T* have = get(k)) {
      *have = std::move(v);
      return *have;
    }
    index_.emplace(k, slots_.size());
    slots_.emplace_back(k, std::move(v));
    alive_.push_back(1);
    return slots_.back().second;
  }
  /// map.delete(k) → whether it was there.
  bool erase(const K& k) {
    const auto it = index_.find(k);
    if (it == index_.end()) {
      return false;
    }
    slots_[it->second] = {};
    alive_[it->second] = 0;
    index_.erase(it);
    return true;
  }
  void clear() {
    slots_.clear();
    alive_.clear();
    index_.clear();
  }
  [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
  [[nodiscard]] bool empty() const noexcept { return index_.empty(); }
  void compact() {
    if (slots_.size() == index_.size()) {
      return;
    }
    std::vector<std::pair<K, T>> kept;
    kept.reserve(index_.size());
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (alive_[i] != 0) {
        index_[slots_[i].first] = kept.size();
        kept.push_back(std::move(slots_[i]));
      }
    }
    slots_ = std::move(kept);
    alive_.assign(slots_.size(), 1);
  }

  /// Slot-by-slot access for loops that see entries added during the loop: for (i = 0; i < slots(); ++i) if (at(i))…
  [[nodiscard]] std::size_t slots() const noexcept { return slots_.size(); }
  [[nodiscard]] std::pair<K, T>* at(std::size_t i) { return alive_[i] != 0 ? &slots_[i] : nullptr; }
  [[nodiscard]] const std::pair<K, T>* at(std::size_t i) const { return alive_[i] != 0 ? &slots_[i] : nullptr; }

  /// The keys now, in order (`[...map.keys()]`: a copy, safe to change the map while walking it).
  [[nodiscard]] std::vector<K> keys() const {
    std::vector<K> out;
    out.reserve(index_.size());
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (alive_[i] != 0) {
        out.push_back(slots_[i].first);
      }
    }
    return out;
  }

  // Iteration over the live entries in order (the slots as they are when the loop starts and grows).
  template <class Map, class Pair>
  class Iter {
   public:
    Iter(Map* m, std::size_t i) : m_(m), i_(i) { skip(); }
    Pair& operator*() const { return m_->slots_[i_]; }
    Pair* operator->() const { return &m_->slots_[i_]; }
    Iter& operator++() {
      ++i_;
      skip();
      return *this;
    }
    bool operator!=(const Iter& o) const { return i_ < m_->slots_.size() && i_ != o.i_; }

   private:
    void skip() {
      while (i_ < m_->slots_.size() && m_->alive_[i_] == 0) {
        ++i_;
      }
    }
    Map* m_;
    std::size_t i_;
  };
  using iterator = Iter<OrderedMap, std::pair<K, T>>;
  using const_iterator = Iter<const OrderedMap, const std::pair<K, T>>;
  iterator begin() { return {this, 0}; }
  iterator end() { return {this, static_cast<std::size_t>(-1)}; }
  [[nodiscard]] const_iterator begin() const { return {this, 0}; }
  [[nodiscard]] const_iterator end() const { return {this, static_cast<std::size_t>(-1)}; }

  // the existing kernel API (drawing3d, flatview): the live entries
  [[nodiscard]] const OrderedMap& items() const noexcept { return *this; }
  [[nodiscard]] OrderedMap& items() noexcept { return *this; }

  friend bool operator==(const OrderedMap& a, const OrderedMap& b) {
    if (a.size() != b.size()) {
      return false;
    }
    auto i = a.begin();
    auto j = b.begin();
    for (; i != a.end(); ++i, ++j) {
      if (!(i->first == j->first && i->second == j->second)) {
        return false;
      }
    }
    return true;
  }

 private:
  std::vector<std::pair<K, T>> slots_;  // a deleted entry's slot holds a default value, alive_ 0
  std::vector<char> alive_;
  std::unordered_map<K, std::size_t> index_;
};

template <class T>
using IdMap = OrderedMap<Id, T>;

/// A JS Set: insertion order, `add` of a present value keeps its place, a deleted value added again goes to the end,
/// and a loop sees values added during it.
template <class K>
class OrderedSet {
 public:
  OrderedSet() = default;
  OrderedSet(std::initializer_list<K> init) {
    for (const K& k : init) {
      add(k);
    }
  }
  template <class R>
  static OrderedSet of(const R& range) {
    OrderedSet s;
    for (const auto& k : range) {
      s.add(k);
    }
    return s;
  }
  bool add(const K& k) {
    if (!index_.emplace(k, items_.size()).second) {
      return false;
    }
    items_.push_back(k);
    alive_.push_back(1);
    return true;
  }
  [[nodiscard]] bool has(const K& k) const { return index_.contains(k); }
  [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
  [[nodiscard]] bool empty() const noexcept { return index_.empty(); }
  bool erase(const K& k) {
    const auto it = index_.find(k);
    if (it == index_.end()) {
      return false;
    }
    alive_[it->second] = 0;
    index_.erase(it);
    return true;
  }
  /// The values now, in order (a copy: `[...set]`).
  [[nodiscard]] std::vector<K> values() const {
    std::vector<K> out;
    out.reserve(index_.size());
    for (std::size_t i = 0; i < items_.size(); ++i) {
      if (alive_[i] != 0) {
        out.push_back(items_[i]);
      }
    }
    return out;
  }
  /// Slot access for loops that see values added while they run.
  [[nodiscard]] std::size_t slots() const noexcept { return items_.size(); }
  [[nodiscard]] const K* at(std::size_t i) const { return alive_[i] != 0 ? &items_[i] : nullptr; }
  [[nodiscard]] std::optional<K> first() const {
    for (std::size_t i = 0; i < items_.size(); ++i) {
      if (alive_[i] != 0) {
        return items_[i];
      }
    }
    return std::nullopt;
  }

  class Iter {
   public:
    Iter(const OrderedSet* s, std::size_t i) : s_(s), i_(i) { skip(); }
    const K& operator*() const { return s_->items_[i_]; }
    Iter& operator++() {
      ++i_;
      skip();
      return *this;
    }
    bool operator!=(const Iter& /*end*/) const { return i_ < s_->items_.size(); }

   private:
    void skip() {
      while (i_ < s_->items_.size() && s_->alive_[i_] == 0) {
        ++i_;
      }
    }
    const OrderedSet* s_;
    std::size_t i_;
  };
  [[nodiscard]] Iter begin() const { return {this, 0}; }
  [[nodiscard]] Iter end() const { return {this, static_cast<std::size_t>(-1)}; }

 private:
  std::vector<K> items_;
  std::vector<char> alive_;
  std::unordered_map<K, std::size_t> index_;
};

}  // namespace cc::m3d
