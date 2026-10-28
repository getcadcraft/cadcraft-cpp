// SPDX-License-Identifier: Apache-2.0
// A JS Map keyed by JSON values (ids from project files): SameValueZero keys (a string and a number are different
// keys; -0 is 0; NaN is one key), insertion order, `set` on a present key replaces its value in place.
#pragma once

#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/value.hpp"

namespace cc::civil {

/// The identity of a primitive JSON value as a Map key (objects and arrays by their JSON: never ids in practice).
[[nodiscard]] inline std::string map_key(const base::Value& v) {
  switch (v.kind()) {
    case base::Value::Kind::undefined: return "u";
    case base::Value::Kind::null: return "n";
    case base::Value::Kind::boolean: return v.as_bool() ? "b1" : "b0";
    case base::Value::Kind::number: {
      const double d = v.as_number();
      if (std::isnan(d)) {
        return "dNaN";
      }
      std::string s = "d";
      base::append_number(s, d == 0 ? 0.0 : d);
      return s;
    }
    case base::Value::Kind::string: return "s" + v.as_string();
    default: return "o" + base::to_json(v);
  }
}
/// JS `a === b` for JSON values (objects by identity in JS: here never equal).
[[nodiscard]] inline bool strict_equal(const base::Value& a, const base::Value& b) {
  if (a.kind() != b.kind()) {
    return false;
  }
  switch (a.kind()) {
    case base::Value::Kind::undefined:
    case base::Value::Kind::null: return true;
    case base::Value::Kind::boolean: return a.as_bool() == b.as_bool();
    case base::Value::Kind::number: return a.as_number() == b.as_number();
    case base::Value::Kind::string: return a.as_string() == b.as_string();
    default: return false;
  }
}
/// String(k) as a property key (Map → object in the views).
[[nodiscard]] inline std::string prop_key(const base::Value& k) {
  if (k.is_undefined()) {
    return "undefined";
  }
  if (k.is_null()) {
    return "null";
  }
  return base::js_string(k);
}

template <class T>
class JsMap {
 public:
  void set(const base::Value& key, T v) {
    const std::string k = map_key(key);
    if (const auto it = index_.find(k); it != index_.end()) {
      entries_[it->second].second = std::move(v);
      return;
    }
    index_.emplace(k, entries_.size());
    entries_.emplace_back(key, std::move(v));
  }
  [[nodiscard]] T* get(const base::Value& key) {
    const auto it = index_.find(map_key(key));
    return it == index_.end() ? nullptr : &entries_[it->second].second;
  }
  [[nodiscard]] const T* get(const base::Value& key) const {
    const auto it = index_.find(map_key(key));
    return it == index_.end() ? nullptr : &entries_[it->second].second;
  }
  [[nodiscard]] bool has(const base::Value& key) const { return index_.contains(map_key(key)); }
  [[nodiscard]] std::vector<std::pair<base::Value, T>>& entries() { return entries_; }
  [[nodiscard]] const std::vector<std::pair<base::Value, T>>& entries() const { return entries_; }
  [[nodiscard]] std::size_t size() const { return entries_.size(); }

 private:
  std::map<std::string, std::size_t> index_;
  std::vector<std::pair<base::Value, T>> entries_;
};

}  // namespace cc::civil
