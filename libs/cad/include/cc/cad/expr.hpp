// SPDX-License-Identifier: Apache-2.0
// cc::cad::expr — CadCraft 0.5's parameter expression language (expr.js): a tokenizer, a recursive-descent parser and
// a tree walker; never an eval. Lengths are millimetres and angles degrees: a number may carry a unit (mm cm m in ft
// ' " deg ° rad); trigonometry is in degrees; comparisons give 1 or 0 for if(c, a, b).
//
//   auto r = expr::compute("2*45", {}, words);   // r.ok, r.value | r.error
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cc/i18n/i18n.hpp"

namespace cc::cad::expr {

/// One node of a parsed expression.
struct Node {
  enum class Kind : std::uint8_t { num, var, neg, cmp, bin, call };
  Kind kind = Kind::num;
  double v = 0;
  std::string name;  // var: the variable; call: the function (lower case); num: a named constant (pi)
  std::string op;    // cmp / bin
  std::vector<std::shared_ptr<const Node>> args;  // neg: [a]; cmp / bin: [a, b]; call: its arguments
  std::size_t pos = 0;
};
using Ast = std::shared_ptr<const Node>;

struct Parsed {
  bool ok = false;
  Ast ast;
  std::vector<std::string> uses;  // the variables it names, first seen first
  std::string error;
  std::optional<std::size_t> pos;
};
/// expr.js parse(src, names): `names` are the variables it may use (nothing: any name, checked when evaluated).
[[nodiscard]] Parsed parse(std::string_view src, const std::optional<std::set<std::string>>& names,
                           const i18n::Catalog& words);

struct Result {
  bool ok = false;
  double value = 0;
  std::vector<std::string> uses;
  std::string error;
  std::optional<std::size_t> pos;
};
/// expr.js evaluate(ast, values): the value, or the reason it has none.
[[nodiscard]] Result evaluate(const Ast& ast, const std::map<std::string, double>& values, const i18n::Catalog& words);
/// expr.js compute(src, values): parse + evaluate.
[[nodiscard]] Result compute(std::string_view src, const std::map<std::string, double>& values,
                             const i18n::Catalog& words);

}  // namespace cc::cad::expr
