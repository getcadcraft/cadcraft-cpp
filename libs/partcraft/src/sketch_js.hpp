// SPDX-License-Identifier: Apache-2.0
// What sketch.js and solver.js share inside libs/partcraft: the JS's TypeErrors as an exception caught at each public
// function, entity lookup, the relation table, points, radii, spline curves and equations.
#pragma once

#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"
#include "cc/partcraft/sketch.hpp"
#include "cc/partcraft/spline.hpp"

namespace cc::partcraft::sketch::detail {

using base::Value;
using P2 = std::array<double, 2>;
inline constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

/// A JS throw (its Error message, or the TypeError V8 gives): caught at the public functions.
struct Thrown : std::runtime_error {
  using std::runtime_error::runtime_error;
};
[[noreturn]] inline void type_error(const Value& of, std::string_view key) {
  throw Thrown(std::string("Cannot read properties of ") + (of.is_null() ? "null" : "undefined") + " (reading '" +
               std::string(key) + "')");
}
/// o[key] where the JS reads a property: null / undefined throw.
inline Value prop(const Value& o, std::string_view key) {
  if (o.is_null() || o.is_undefined()) {
    type_error(o, key);
  }
  return o.is_object() ? o.get(key) : Value{};
}

template <class F>
auto guard(F&& f) -> base::Result<decltype(f())> {
  try {
    return std::forward<F>(f)();
  } catch (const Thrown& e) {
    return base::fail(base::Errc::invalid, e.what());
  }
}

struct Def {
  std::string_view type, label_key;
  std::vector<std::string_view> sigs;
  bool dim = false, autom = false;
};
/// CONSTRAINTS in Object.keys order.
const std::vector<Def>& defs();
/// CONSTRAINTS[type]: the def, a prototype member (truthy, no fields), or nothing.
struct DefRef {
  const Def* d = nullptr;
  bool proto = false;
  [[nodiscard]] bool found() const { return d != nullptr || proto; }
};
DefRef def_of(const Value& type);
/// The sig list of a def; a prototype member has none (`def.sigs.includes` throws).
const std::vector<std::string_view>& sigs_of(const DefRef& d);

Value* entities(Value& sk);
const base::Array& entities_of(const Value& sk);
const base::Array& constraints_of(const Value& sk);
/// sk.entities.find((e) => e.id === id): a pointer into sk (nullptr when absent).
Value* find_entity(Value& sk, const Value& id);
const Value* find_entity(const Value& sk, const Value& id);
Value origin();
/// byId (a copy).
Value ent_by_id(const Value& sk, const Value& id);
/// pointXY: [x, y], or nothing (the JS's null).
std::optional<P2> point_xy(const Value& sk, const Value& id);
/// p[0] / p[1] of pointXY's answer: null throws.
inline P2 xy(const std::optional<P2>& p) {
  if (!p) {
    type_error(Value(nullptr), "0");
  }
  return *p;
}
double num(const Value& v);
char kind_of(const Value& e);  // P L S R, or 0 for null
double radius_of(const Value& sk, const Value& e);
std::string next_id(Value& sk, std::string_view prefix);
double signed_dist(P2 p, const std::array<P2, 2>& line);
/// The spline's curve at the given point positions (pointXY by default).
spline::Curve spline_curve(const Value& sk, const Value& e, const std::function<std::optional<P2>(const Value&)>& pget,
                           const i18n::Catalog& w);
P2 spline_end_tangent(const Value& sk, const Value& e, char end,
                      const std::function<std::optional<P2>(const Value&)>& pget,
                      const std::function<Value(const Value&, std::string_view)>& aget, const i18n::Catalog& w);
std::optional<std::array<Value, 2>> meet_of(const Value& sk, const Value& a, const Value& b);
std::optional<Value> shared_end(const Value& sk, const Value& a, const Value& b);
/// matchSig: {sig, refs}; throws where the JS does.
std::optional<std::pair<std::string, std::vector<Value>>> match(const Value& sk, const Value& type, const Value& refs);
Value measure(const Value& sk, const Value& type, const std::vector<Value>& refs,
              const std::optional<std::string>& sig);

struct Eq {
  std::string cid;
  std::function<double()> f;
  std::vector<std::size_t> deps;
};
std::vector<Eq> equations(const Value& sk, std::vector<double>& x, const Vars& vars, const i18n::Catalog& w);

}  // namespace cc::partcraft::sketch::detail
