// SPDX-License-Identifier: Apache-2.0
// expr.js (CadCraft 0.5) in C++: the parameter expression language. Errors are values, not exceptions: the parser and
// the walker stop at the first one and say why, in the reader's language where the JS does.
#include "cc/cad/expr.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"

namespace cc::cad::expr {

namespace {

constexpr double kEps = 1e-9;
constexpr double kD2r = std::numbers::pi / 180;

/// A UTF-8 code point at i (and its length)
std::pair<char32_t, std::size_t> cp_at(std::string_view s, std::size_t i) {
  const auto b = static_cast<unsigned char>(s[i]);
  std::size_t n = 1;
  char32_t cp = b;
  if (b >= 0xF0) {
    n = 4;
    cp = b & 0x07U;
  } else if (b >= 0xE0) {
    n = 3;
    cp = b & 0x0FU;
  } else if (b >= 0xC0) {
    n = 2;
    cp = b & 0x1FU;
  }
  for (std::size_t k = 1; k < n && i + k < s.size(); ++k) {
    cp = (cp << 6U) | (static_cast<unsigned char>(s[i + k]) & 0x3FU);
  }
  return {cp, std::min(n, s.size() - i)};
}
/// /\s/ (JavaScript's white space and line terminators)
bool js_space(char32_t c) {
  return c == U' ' || (c >= 0x09 && c <= 0x0D) || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
         c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}
bool digit(char c) {
  return c >= '0' && c <= '9';
}
bool word(char c) {
  return digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
/// clean(x): trigonometry kept to 15 digits (sin(30) is 0.49999999999999994 in floating point)
double clean(double x) {
  return std::abs(x) < 1e-15 ? 0 : base::js_number(base::js_to_exponential(x, 14));
}

struct Unit {
  std::string_view text;
  double factor;
};
// UNIT_RE's alternatives in their order (mm before m)
constexpr std::array<Unit, 10> kUnits{{{"mm", 1},
                                       {"cm", 10},
                                       {"m", 1000},
                                       {"in", 25.4},
                                       {"ft", 304.8},
                                       {"deg", 1},
                                       {"rad", 180 / std::numbers::pi},
                                       {"\xC2\xB0", 1},  // °
                                       {"\"", 25.4},
                                       {"'", 304.8}}};

struct Token {
  std::string t;  // num, id, end, or the operator
  double v = 0;
  std::string id;
  std::size_t pos = 0;
};

struct Err {
  std::string message;
  std::optional<std::size_t> pos;
};

using Fill = std::initializer_list<std::pair<std::string_view, std::string>>;

class Lexer {
 public:
  Lexer(std::string_view s, const i18n::Catalog& w) : s_(s), w_(w) {}
  std::optional<Err> run(std::vector<Token>& out) {
    std::size_t i = 0;
    while (i < s_.size()) {
      const auto [c, len] = cp_at(s_, i);
      if (js_space(c)) {
        i += len;
        continue;
      }
      if (digit(s_[i]) || (s_[i] == '.' && i + 1 < s_.size() && digit(s_[i + 1]))) {
        number(i, out);
        continue;
      }
      if ((s_[i] >= 'A' && s_[i] <= 'Z') || (s_[i] >= 'a' && s_[i] <= 'z') || s_[i] == '_') {
        std::size_t j = i;
        while (j < s_.size() && word(s_[j])) {
          ++j;
        }
        out.push_back({.t = "id", .v = 0, .id = std::string(s_.substr(i, j - i)), .pos = i});
        i = j;
        continue;
      }
      const std::string_view two = s_.substr(i, 2);
      if (two == "<=" || two == ">=" || two == "==" || two == "!=") {
        out.push_back({.t = std::string(two), .v = 0, .id = {}, .pos = i});
        i += 2;
        continue;
      }
      if (const char* op = single(c)) {
        out.push_back({.t = op, .v = 0, .id = {}, .pos = i});
        i += len;
        continue;
      }
      return Err{w_.t("expr.not_allowed_expression", {{"c", std::string(s_.substr(i, len))}}), i};
    }
    out.push_back({.t = "end", .v = 0, .id = {}, .pos = s_.size()});
    return std::nullopt;
  }

 private:
  std::string_view s_;
  const i18n::Catalog& w_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) — lives for one call

  static const char* single(char32_t c) {
    switch (c) {
      case U'+': return "+";
      case U'-':
      case 0x2212:
      case 0x2013: return "-";  // - − –
      case U'*':
      case 0xD7:
      case 0xB7: return "*";  // * × ·
      case U'/':
      case 0xF7: return "/";  // / ÷
      case U'%': return "%";
      case U'^': return "^";
      case U'(': return "(";
      case U')': return ")";
      case U',': return ",";
      case U'<': return "<";
      case U'>': return ">";
      case 0x2264: return "<=";  // ≤
      case 0x2265: return ">=";  // ≥
      case 0x2260: return "!=";  // ≠
      default: return nullptr;
    }
  }

  /// /^(\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?/, then a unit right after it
  void number(std::size_t& i, std::vector<Token>& out) const {
    const std::size_t at = i;
    std::size_t j = i;
    while (j < s_.size() && digit(s_[j])) {
      ++j;
    }
    if (j < s_.size() && s_[j] == '.') {
      ++j;
      while (j < s_.size() && digit(s_[j])) {
        ++j;
      }
    }
    if (j < s_.size() && (s_[j] == 'e' || s_[j] == 'E')) {
      std::size_t k = j + 1;
      if (k < s_.size() && (s_[k] == '+' || s_[k] == '-')) {
        ++k;
      }
      if (k < s_.size() && digit(s_[k])) {
        while (k < s_.size() && digit(s_[k])) {
          ++k;
        }
        j = k;
      }
    }
    double v = base::js_number(s_.substr(at, j - at));
    i = j;
    // a single space before ° " ' is skipped: 90 °, 3 '
    std::size_t u = i;
    if (u < s_.size() && s_[u] == ' ' && u + 1 < s_.size() &&
        (s_[u + 1] == '"' || s_[u + 1] == '\'' || s_.substr(u + 1, 2) == "\xC2\xB0")) {
      ++u;
    }
    for (const Unit& unit : kUnits) {
      if (s_.substr(u, unit.text.size()) != unit.text) {
        continue;
      }
      const std::size_t after = u + unit.text.size();
      if (after < s_.size() && word(s_[after])) {
        continue;
      }
      v *= unit.factor;
      i = after;
      break;
    }
    out.push_back({.t = "num", .v = v, .id = {}, .pos = at});
  }
};

struct Func {
  const char* name;
  int min, max;
};
constexpr std::array<Func, 21> kFuncs{
    {{"sin", 1, 1},  {"cos", 1, 1},   {"tan", 1, 1}, {"asin", 1, 1},  {"acos", 1, 1}, {"atan", 1, 1}, {"atan2", 2, 2},
     {"sqrt", 1, 1}, {"pow", 2, 2},   {"exp", 1, 1}, {"ln", 1, 1},    {"log", 1, 1},  {"abs", 1, 1},  {"sign", 1, 1},
     {"ceil", 1, 1}, {"floor", 1, 1}, {"int", 1, 1}, {"round", 1, 2}, {"min", 1, 99}, {"max", 1, 99}, {"if", 3, 3}}};
const Func* func(const std::string& name) {
  for (const Func& f : kFuncs) {
    if (name == f.name) {
      return &f;
    }
  }
  return nullptr;
}
std::string function_names() {
  std::string s;
  for (const Func& f : kFuncs) {
    s += (s.empty() ? "" : ", ") + std::string(f.name);
  }
  return s;
}

class Parser {
 public:
  Parser(std::vector<Token> tk, const std::optional<std::set<std::string>>& names, const i18n::Catalog& w)
      : tk_(std::move(tk)), names_(names), w_(w) {}
  std::optional<Err> err;
  std::vector<std::string> uses;

  Ast top() {
    Ast a = cond();
    if (!err && peek().t != "end") {
      const Token& p = peek();
      fail(p.t == ")" ? w_.t("expr.there_without")
                      : w_.t("expr.unexpected_operator_missing", {{"v", p.t == "id" ? p.id : show(p)}}),
           p.pos);
    }
    return err ? nullptr : a;
  }

 private:
  std::vector<Token> tk_;
  const std::optional<std::set<std::string>>&
      names_;               // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) — lives for one call
  const i18n::Catalog& w_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) — lives for one call
  std::size_t i_ = 0;
  int depth_ = 0;

  [[nodiscard]] const Token& peek() const { return tk_[std::min(i_, tk_.size() - 1)]; }
  void fail(std::string m, std::size_t pos) {
    if (!err) {
      err = Err{std::move(m), pos};
    }
  }
  static std::string show(const Token& t) {
    if (t.t == "num") {
      return base::js_string(base::Value(t.v));
    }
    return t.t;
  }
  static Ast node(Node n) { return std::make_shared<const Node>(std::move(n)); }
  bool take(const std::string& t) {
    if (peek().t != t) {
      const Token& got = peek();
      std::string m;
      if (t == ")") {
        m = got.t == "end" ? w_.t("expr.a_is_missing")
                           : w_.t("expr.expected_before", {{"v", got.t == "id" ? got.id : show(got)}});
      } else {
        m = w_.t("expr.expected", {{"t", t}});
      }
      fail(std::move(m), got.pos);
      return false;
    }
    ++i_;
    return true;
  }
  bool deeper() {
    if (++depth_ > 100) {
      fail(w_.t("expr.expression_nested_too_deeply"), peek().pos);
      return false;
    }
    return true;
  }

  // NOLINTBEGIN(misc-no-recursion) — the grammar recurses; depth_ caps it at 100 levels, as the JS does
  Ast cond() {
    Ast a = sum();
    if (err) {
      return nullptr;
    }
    const std::string& t = peek().t;
    if (t == "<" || t == "<=" || t == ">" || t == ">=" || t == "==" || t == "!=") {
      const Token op = tk_[i_++];
      Ast b = sum();
      return node({.kind = Node::Kind::cmp, .v = 0, .name = {}, .op = op.t, .args = {a, b}, .pos = op.pos});
    }
    return a;
  }
  Ast sum() {
    Ast n = term();
    while (!err && (peek().t == "+" || peek().t == "-")) {
      const Token op = tk_[i_++];
      Ast b = term();
      n = node({.kind = Node::Kind::bin, .v = 0, .name = {}, .op = op.t, .args = {n, b}, .pos = op.pos});
    }
    return n;
  }
  Ast term() {
    Ast n = power();
    while (!err && (peek().t == "*" || peek().t == "/" || peek().t == "%")) {
      const Token op = tk_[i_++];
      Ast b = power();
      n = node({.kind = Node::Kind::bin, .v = 0, .name = {}, .op = op.t, .args = {n, b}, .pos = op.pos});
    }
    return n;
  }
  Ast power() {
    Ast a = unary();
    if (err || peek().t != "^") {
      return a;
    }
    const Token op = tk_[i_++];
    if (!deeper()) {
      return nullptr;
    }
    Ast b = power();
    --depth_;
    return node({.kind = Node::Kind::bin, .v = 0, .name = {}, .op = "^", .args = {a, b}, .pos = op.pos});
  }
  Ast unary() {
    if (!deeper()) {
      return nullptr;
    }
    Ast r;
    if (peek().t == "-") {
      ++i_;
      Ast a = unary_pow();
      r = node({.kind = Node::Kind::neg, .v = 0, .name = {}, .op = {}, .args = {a}, .pos = 0});
    } else if (peek().t == "+") {
      ++i_;
      r = unary_pow();
    } else {
      r = primary();
    }
    --depth_;
    return r;
  }
  // -2^2 = -(2^2) = -4, as in maths
  Ast unary_pow() {
    Ast a = unary();
    if (err || peek().t != "^") {
      return a;
    }
    const Token op = tk_[i_++];
    Ast b = power();
    return node({.kind = Node::Kind::bin, .v = 0, .name = {}, .op = "^", .args = {a, b}, .pos = op.pos});
  }
  Ast primary() {
    const Token t = peek();
    if (t.t == "num") {
      ++i_;
      return node({.kind = Node::Kind::num, .v = t.v, .name = {}, .op = {}, .args = {}, .pos = t.pos});
    }
    if (t.t == "(") {
      ++i_;
      Ast n = cond();
      if (err || !take(")")) {
        return nullptr;
      }
      return n;
    }
    if (t.t == "id") {
      ++i_;
      if (peek().t == "(") {
        const std::string name = base::js_to_lower(t.id);
        const Func* f = func(name);
        if (f == nullptr) {
          fail(w_.t("expr.not_function_use", {{"v", t.id}, {"v2", function_names()}}), t.pos);
          return nullptr;
        }
        ++i_;
        std::vector<Ast> args{cond()};
        while (!err && peek().t == ",") {
          ++i_;
          args.push_back(cond());
        }
        if (err || !take(")")) {
          return nullptr;
        }
        const auto n = static_cast<int>(args.size());
        if (n < f->min || n > f->max) {
          std::string m;
          if (f->min == f->max) {
            m = w_.tn("expr.args.exact", static_cast<double>(f->min), {{"name", name}});
          } else if (f->max > 9) {
            m = w_.t("expr.args.atleast", {{"name", name}, {"min", std::to_string(f->min)}});
          } else {
            m = w_.t("expr.args.range",
                     {{"name", name}, {"min", std::to_string(f->min)}, {"max", std::to_string(f->max)}});
          }
          fail(std::move(m), t.pos);
          return nullptr;
        }
        return node({.kind = Node::Kind::call, .v = 0, .name = name, .op = {}, .args = std::move(args), .pos = t.pos});
      }
      if (t.id == "pi") {
        return node({.kind = Node::Kind::num, .v = std::numbers::pi, .name = "pi", .op = {}, .args = {}, .pos = t.pos});
      }
      if (names_ && !names_->contains(t.id)) {
        fail(w_.t("expr.there_no_parameter_called", {{"v", t.id}}), t.pos);
        return nullptr;
      }
      if (std::ranges::find(uses, t.id) == uses.end()) {
        uses.push_back(t.id);
      }
      return node({.kind = Node::Kind::var, .v = 0, .name = t.id, .op = {}, .args = {}, .pos = t.pos});
    }
    if (t.t == ")" || t.t == ",") {
      fail(w_.t("expr.value_missing_before", {{"t", t.t}}), t.pos);
      return nullptr;
    }
    fail(t.t == "end" ? w_.t("expr.expression_ends_too_soon") : w_.t("expr.unexpected", {{"v", show(t)}}), t.pos);
    return nullptr;
  }
  // NOLINTEND(misc-no-recursion)
};

class Walker {
 public:
  Walker(const std::map<std::string, double>& values, const i18n::Catalog& w) : values_(values), w_(w) {}
  std::optional<Err> err;

  // NOLINTBEGIN(misc-no-recursion) — over a tree the parser built at most 100 levels deep
  double ev(const Node& n) {
    if (err) {
      return 0;
    }
    switch (n.kind) {
      case Node::Kind::num: return n.v;
      case Node::Kind::var: {
        const auto it = values_.find(n.name);
        if (it == values_.end() || !std::isfinite(it->second)) {
          fail(w_.t("expr.has_no_value", {{"name", n.name}}), n.pos);
          return 0;
        }
        return it->second;
      }
      case Node::Kind::neg: return -ev(*n.args[0]);
      case Node::Kind::cmp: {
        const double a = ev(*n.args[0]);
        const double b = ev(*n.args[1]);
        const double tol = kEps * std::max({1.0, std::abs(a), std::abs(b)});
        if (n.op == "<") {
          return a < b - tol ? 1 : 0;
        }
        if (n.op == "<=") {
          return a <= b + tol ? 1 : 0;
        }
        if (n.op == ">") {
          return a > b + tol ? 1 : 0;
        }
        if (n.op == ">=") {
          return a >= b - tol ? 1 : 0;
        }
        if (n.op == "==") {
          return std::abs(a - b) <= tol ? 1 : 0;
        }
        return std::abs(a - b) > tol ? 1 : 0;
      }
      case Node::Kind::bin: {
        const double a = ev(*n.args[0]);
        const double b = ev(*n.args[1]);
        if (n.op == "+") {
          return a + b;
        }
        if (n.op == "-") {
          return a - b;
        }
        if (n.op == "*") {
          return a * b;
        }
        if (n.op == "^") {
          const double r = std::pow(a, b);
          if (!std::isfinite(r)) {
            fail(w_.t("expr.power_has_no_value"), n.pos);
          }
          return r;
        }
        if (b == 0) {
          fail(w_.t("expr.expression_divides_zero"), n.pos);
          return 0;
        }
        return n.op == "/" ? a / b : a - b * std::floor(a / b);
      }
      case Node::Kind::call: return call(n);
    }
    return 0;
  }
  // NOLINTEND(misc-no-recursion)

 private:
  const std::map<std::string, double>&
      values_;              // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) — lives for one call
  const i18n::Catalog& w_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) — lives for one call
  void fail(std::string m, std::optional<std::size_t> pos = std::nullopt) {
    if (!err) {
      err = Err{std::move(m), pos};
    }
  }
  double fin(double x, const char* name) {
    if (!std::isfinite(x)) {
      fail(w_.t("expr.does_not_give_number", {{"name", name}}));
    }
    return x;
  }
  double call(const Node& n) {  // NOLINT(misc-no-recursion)
    const std::string& f = n.name;
    if (f == "if") {
      return ev(*n.args[0]) != 0 ? ev(*n.args[1]) : ev(*n.args[2]);
    }
    std::vector<double> a;
    a.reserve(n.args.size());
    for (const Ast& x : n.args) {
      a.push_back(ev(*x));
    }
    if (err) {
      return 0;
    }
    const double x = a[0];
    const double big = kEps * std::max(1.0, std::abs(x));
    if (f == "sin") {
      return clean(base::js_sin(x * kD2r));
    }
    if (f == "cos") {
      return clean(base::js_cos(x * kD2r));
    }
    if (f == "tan") {
      if (std::abs(base::js_cos(x * kD2r)) < 1e-12) {
        fail("tan() of 90° has no value.");
        return 0;
      }
      return clean(std::tan(x * kD2r));
    }
    if (f == "asin" || f == "acos") {
      if (x < -1 - kEps || x > 1 + kEps) {
        fail(f + "() takes a value from -1 to 1.");
        return 0;
      }
      const double c = std::max(-1.0, std::min(1.0, x));
      return (f == "asin" ? std::asin(c) : std::acos(c)) / kD2r;
    }
    if (f == "atan") {
      return base::js_atan(x) / kD2r;
    }
    if (f == "atan2") {
      return base::js_atan2(x, a[1]) / kD2r;
    }
    if (f == "sqrt") {
      if (x < -kEps) {
        fail("sqrt() of a negative number has no value.");
        return 0;
      }
      return std::sqrt(std::max(0.0, x));
    }
    if (f == "pow") {
      return fin(std::pow(x, a[1]), "pow");
    }
    if (f == "exp") {
      return fin(std::exp(x), "exp");
    }
    if (f == "ln" || f == "log") {
      if (x <= 0) {
        fail(f + "() takes a number above zero.");
        return 0;
      }
      return f == "ln" ? std::log(x) : std::log10(x);
    }
    if (f == "abs") {
      return std::abs(x);
    }
    if (f == "sign") {
      if (std::isnan(x) || x == 0) {
        return x;
      }
      return x > 0 ? 1 : -1;
    }
    if (f == "ceil") {
      return std::ceil(x - big);
    }
    if (f == "floor") {
      return std::floor(x + big);
    }
    if (f == "int") {
      double s = 0;
      if (x > 0) {
        s = 1;
      } else if (x < 0) {
        s = -1;
      }
      return std::trunc(x + s * big);
    }
    if (f == "round") {
      const double d = a.size() > 1 ? a[1] : 0;
      if (std::floor(d) != d || d < 0 || d > 10) {
        fail("round(x, digits) needs whole digits from 0 to 10.");
        return 0;
      }
      const double p = std::pow(10.0, d);
      const double y = std::abs(x) * p;
      const double r = std::floor(y + 0.5 + kEps * std::max(1.0, y));
      return (x < 0 ? -r : r) / p;
    }
    // min / max: Math.min / Math.max (NaN wins)
    double r = x;
    for (const double v : a) {
      if (std::isnan(v) || std::isnan(r)) {
        r = std::numeric_limits<double>::quiet_NaN();
      } else {
        r = f == "min" ? std::min(r, v) : std::max(r, v);
      }
    }
    return r;
  }
};

}  // namespace

Parsed parse(std::string_view src, const std::optional<std::set<std::string>>& names, const i18n::Catalog& words) {
  Parsed out;
  if (base::js_blank(src)) {
    out.error = words.t("expr.type_value_expression_like");
    out.pos = 0;
    return out;
  }
  if (base::utf16_length(src) > 1000) {
    out.error = words.t("expr.expression_too_long_1000");
    out.pos = 0;
    return out;
  }
  std::vector<Token> tk;
  if (auto e = Lexer(src, words).run(tk)) {
    out.error = std::move(e->message);
    out.pos = e->pos;
    return out;
  }
  Parser p(std::move(tk), names, words);
  Ast ast = p.top();
  if (p.err) {
    out.error = std::move(p.err->message);
    out.pos = p.err->pos;
    return out;
  }
  out.ok = true;
  out.ast = std::move(ast);
  out.uses = std::move(p.uses);
  return out;
}

Result evaluate(const Ast& ast, const std::map<std::string, double>& values, const i18n::Catalog& words) {
  Result out;
  if (!ast) {
    out.error = words.t("expr.bad_expression");
    return out;
  }
  Walker w(values, words);
  const double r = w.ev(*ast);
  if (w.err) {
    out.error = std::move(w.err->message);
    out.pos = w.err->pos;
    return out;
  }
  if (!std::isfinite(r)) {
    out.error = words.t("expr.expression_does_not_give");
    return out;
  }
  out.ok = true;
  out.value = r;
  return out;
}

Result compute(std::string_view src, const std::map<std::string, double>& values, const i18n::Catalog& words) {
  Parsed p = parse(src, std::nullopt, words);
  if (!p.ok) {
    return {.ok = false, .value = 0, .uses = {}, .error = std::move(p.error), .pos = p.pos};
  }
  Result r = evaluate(p.ast, values, words);
  r.uses = std::move(p.uses);
  return r;
}

// ---------------------------------------------------------------- names, literals, numbers (params.js uses them)

bool valid_name(std::string_view s) {
  // NAME_RE /^[A-Za-z_][A-Za-z0-9_]{0,39}$/
  return !s.empty() && s.size() <= 40 && !digit(s[0]) && std::ranges::all_of(s, word);
}

bool reserved(std::string_view s) {
  if (func(std::string(s)) != nullptr || s == "pi") {
    return true;
  }
  return s == "mm" || s == "cm" || s == "m" || s == "in" || s == "ft" || s == "deg" || s == "rad";
}

std::optional<double> literal(std::string_view src, const i18n::Catalog& words) {
  const Parsed p = parse(src, std::set<std::string>{}, words);
  if (!p.ok || !p.ast) {
    return std::nullopt;
  }
  const Node& n = *p.ast;
  if (n.kind == Node::Kind::neg && !n.args.empty() && n.args[0]->kind == Node::Kind::num && n.args[0]->name.empty()) {
    return -n.args[0]->v;
  }
  if (n.kind == Node::Kind::num && n.name.empty()) {
    return n.v;
  }
  return std::nullopt;
}

std::string format_number(double v) {
  if (!std::isfinite(v)) {
    return base::js_string(base::Value(v));
  }
  const double r = base::js_number(base::js_to_exponential(v, 11));  // Number(v.toPrecision(12))
  if (r == 0) {
    return "0";  // -0 too
  }
  std::string s;
  base::append_number(s, r);
  return s;
}

std::string rename_in(std::string_view src, std::string_view from, std::string_view to) {
  // /[A-Za-z_][A-Za-z0-9_]*/g, a match right after a digit or '.' left alone (a unit: 25mm)
  std::string out;
  std::size_t i = 0;
  while (i < src.size()) {
    const char c = src[i];
    const bool start = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    if (!start) {
      out += c;
      ++i;
      continue;
    }
    std::size_t j = i + 1;
    while (j < src.size() && word(src[j])) {
      ++j;
    }
    const std::string_view m = src.substr(i, j - i);
    const bool after_number = i > 0 && (digit(src[i - 1]) || src[i - 1] == '.');
    out += !after_number && m == from ? to : m;
    i = j;
  }
  return out;
}

}  // namespace cc::cad::expr
