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

/// NAME_RE: a letter or _, then letters, digits, _ (40 at most)
[[nodiscard]] bool valid_name(std::string_view s);
/// RESERVED: a function, a constant or a unit
[[nodiscard]] bool reserved(std::string_view s);
/// literal(src): the number a plain number (with or without a unit) is, or nothing
[[nodiscard]] std::optional<double> literal(std::string_view src, const i18n::Catalog& words);
/// formatNumber(v): the shortest text that reads back to v to 12 significant digits
[[nodiscard]] std::string format_number(double v);
/// renameIn(src, from, to): whole names only, never a unit after a number
[[nodiscard]] std::string rename_in(std::string_view src, std::string_view from, std::string_view to);

}  // namespace cc::cad::expr
