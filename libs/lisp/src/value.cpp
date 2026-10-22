// SPDX-License-Identifier: Apache-2.0
// types.js: symbols, conses, objects, printing.
#include "cc/base/value.hpp"

#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/lisp/lisp.hpp"
#include "internal.hpp"

namespace cc::lisp {

// ---------------------------------------------------------------- symbols
namespace {

/// The interned symbols (lock "lisp.symbols": a leaf, held only inside sym()). A deque never moves its elements, so
/// a Sym stays valid for the program's life.
struct SymbolTable {
  std::mutex lock;
  std::deque<Symbol> store;
  std::unordered_map<std::string, Sym> by_name;
};
SymbolTable& symbols() {
  static SymbolTable t;
  return t;
}

/// The JS keeps these in module scope (PickSet.count, builtins.js tempN): one each for the program.
std::atomic<int>& pickset_count() {
  static std::atomic<int> n{0};
  return n;
}
std::atomic<int>& temp_count() {
  static std::atomic<int> n{0};
  return n;
}

}  // namespace

Sym sym(std::string_view name) {
  std::string u = base::js_to_upper(name);
  SymbolTable& t = symbols();
  const std::scoped_lock hold(t.lock);
  if (const auto it = t.by_name.find(u); it != t.by_name.end()) {
    return it->second;
  }
  const Symbol& s = t.store.emplace_back(Symbol{u});
  t.by_name.emplace(std::move(u), &s);
  return &s;
}

int next_pickset_number() noexcept {
  return ++pickset_count();
}
int next_temp_number() noexcept {
  return ++temp_count();
}
void reset_counters() noexcept {
  pickset_count() = 0;
  temp_count() = 0;
}

// ---------------------------------------------------------------- values
Value::Value(std::string s) : v_(std::make_shared<const std::string>(std::move(s))) {}
Value::Value(ConsRef c) noexcept {
  if (c) {
    v_ = std::move(c);
  }
}
Value::Value(ObjRef o) noexcept {
  if (o) {
    v_ = std::move(o);
  }
}
Value Value::str_ref(StrRef s) noexcept {
  Value v;
  v.v_ = std::move(s);
  return v;
}

bool Value::same(const Value& o) const {
  if (v_.index() != o.v_.index()) {
    return false;
  }
  if (nil()) {
    return true;
  }
  if (is_int()) {
    return int_v() == o.int_v();
  }
  if (is_real()) {
    return real() == o.real();  // NaN is not itself, 0 is -0, as ===
  }
  if (is_str()) {
    return str_ref() == o.str_ref() || str() == o.str();
  }
  if (is_sym()) {
    return sym() == o.sym();
  }
  if (is_cons()) {
    return cons() == o.cons();
  }
  return obj() == o.obj();
}

namespace detail {

void release(std::vector<Value>& pending) {
  // What only these values hold is freed in a loop: a list a million long or nested a million deep, or functions whose
  // bodies hold functions, never recurse on the native stack.
  while (!pending.empty()) {
    Value v = std::move(pending.back());
    pending.pop_back();
    if (v.is_cons()) {
      if (v.cons().use_count() == 1) {
        Cons& c = *v.cons();
        pending.push_back(std::exchange(c.car, Value()));
        pending.push_back(std::exchange(c.cdr, Value()));
      }
    } else if (v.is_obj() && v.obj().use_count() == 1) {
      if (auto* f = v.as<Lambda>(); f != nullptr) {
        for (Value& b : f->body) {
          pending.push_back(std::exchange(b, Value()));
        }
      }
    }
  }
}

}  // namespace detail

Cons::~Cons() {
  if (!car.is_cons() && !car.is_obj() && !cdr.is_cons() && !cdr.is_obj()) {
    return;
  }
  try {
    std::vector<Value> pending;
    pending.push_back(std::exchange(car, Value()));
    pending.push_back(std::exchange(cdr, Value()));
    detail::release(pending);
  } catch (...) {  // NOLINT(bugprone-empty-catch) out of memory for the list of pending cells: what is left is freed
                   // the ordinary way, by recursion
  }
}

Lambda::~Lambda() {
  try {
    std::vector<Value> pending;
    pending.reserve(body.size());
    for (Value& b : body) {
      pending.push_back(std::exchange(b, Value()));
    }
    detail::release(pending);
  } catch (...) {  // NOLINT(bugprone-empty-catch) as in ~Cons
  }
}

Value cons(Value a, Value d) {
  return {std::make_shared<Cons>(std::move(a), std::move(d))};
}

Value list(std::vector<Value> xs, Value tail) {
  Value r = std::move(tail);
  for (auto& x : std::views::reverse(xs)) {
    r = cons(std::move(x), std::move(r));
  }
  return r;
}

std::vector<Value> to_vector(const Value& l) {
  std::vector<Value> out;
  for (const Value* x = &l; x->is_cons(); x = &x->cons()->cdr) {
    out.push_back(x->cons()->car);
  }
  return out;
}

bool is_proper(const Value& l) {
  const Value* x = &l;
  while (x->is_cons()) {
    x = &x->cons()->cdr;
  }
  return x->nil();
}

// ---------------------------------------------------------------- objects
std::string Ename::show(bool /*readable*/) const {
  if (id_.is_number()) {
    // (0x7ff700000000 + id * 16).toString(16): ids are whole numbers
    const double n = 140698833649664.0 + id_.as_number() * 16;  // 0x7ff700000000
    if (n >= 0 && n < 9007199254740992.0 && n == std::floor(n)) {
      return "<Entity name: " + base::hex_lower(static_cast<std::uint64_t>(n)) + ">";
    }
    return "<Entity name: " + base::js_string(base::Value(n)) + ">";
  }
  return "<Entity name: " + (id_.is_string() ? id_.as_string() : std::string()) + ">";
}
bool Ename::same(const Object& o) const noexcept {
  const auto* e = dynamic_cast<const Ename*>(&o);
  return e != nullptr && id_ == e->id_;
}
base::Value Ename::plain() const {
  base::Value v = base::Value::object();
  v.set("e", id_);
  return v;
}

PickSet::PickSet(std::vector<base::Value> v) : ids(std::move(v)), n_(next_pickset_number()) {}
std::string PickSet::show(bool /*readable*/) const {
  return "<Selection set: " + std::to_string(n_) + ">";
}
base::Value PickSet::plain() const {
  base::Value v = base::Value::object();
  v.set("ids", base::Value(base::Array(ids)));
  return v;
}

std::string Lambda::show(bool /*readable*/) const {
  return "#<USUBR @" + (name.empty() ? std::string("lambda") : name) + ">";
}
std::string Builtin::show(bool /*readable*/) const {
  return "#<SUBR @" + name + ">";
}
std::string LFile::show(bool /*readable*/) const {
  return "#<file \"" + path + "\">";
}

// ---------------------------------------------------------------- printing
std::string real_text(double x) {
  if (!std::isfinite(x)) {
    if (std::isnan(x)) {
      return "-1.#IND";
    }
    return x > 0 ? "1.#INF" : "-1.#INF";
  }
  if (x == 0) {
    return "0.0";
  }
  const auto strip = [](std::string s) {
    while (!s.empty() && s.back() == '0') {
      s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
      s += '0';
    }
    return s;
  };
  // C's %g with 6 significant digits, then a decimal point always: 3.0, 1.12346, 1.23457e+006, 1.0e-005
  const std::string e = base::js_to_exponential(x, 5);
  const std::size_t at = e.find('e');
  const std::string m0 = e.substr(0, at);
  int ex = 0;  // "e+5" / "e-12": always a sign and digits
  {
    const std::string_view digits = std::string_view(e).substr(at + 2);
    (void)std::from_chars(std::to_address(digits.begin()), std::to_address(digits.end()), ex);
    if (e[at + 1] == '-') {
      ex = -ex;
    }
  }
  if (ex < -4 || ex >= 6) {
    std::string exp = std::to_string(std::abs(ex));
    while (exp.size() < 3) {
      exp.insert(exp.begin(), '0');
    }
    return strip(m0) + 'e' + (ex < 0 ? '-' : '+') + exp;
  }
  std::string s = base::js_to_fixed(x, std::max(0, 5 - ex));
  if (s.contains('.')) {
    return strip(std::move(s));
  }
  return s + ".0";
}

namespace {

void escape_into(std::string& out, const std::string& s) {
  out += '"';
  for (const char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\x1b': out += "\\e"; break;
      default: out += c;
    }
  }
  out += '"';
}

struct Printer {
  std::string out;
  std::size_t max_bytes;
  bool full = false;  // the text passed max_bytes

  [[nodiscard]] bool over() {
    if (out.size() > max_bytes) {
      full = true;
    }
    return full;
  }

  // NOLINTNEXTLINE(misc-no-recursion) — depth-capped (200, as types.js show)
  void put(const Value& x, bool readable, int depth) {
    if (over()) {
      return;
    }
    if (depth > 200) {
      out += "...";
      return;
    }
    if (x.nil()) {
      out += "nil";
    } else if (x.is_real()) {
      out += real_text(x.real());
    } else if (x.is_int()) {
      out += std::to_string(x.int_v());
    } else if (x.is_str()) {
      if (readable) {
        escape_into(out, x.str());
      } else {
        out += x.str();
      }
    } else if (x.is_sym()) {
      out += x.sym()->name;
    } else if (x.is_cons()) {
      const Cons& c = *x.cons();
      if (c.car.is_sym() && c.car.sym() == detail::kQuote() && c.cdr.is_cons() && c.cdr.cons()->cdr.nil()) {
        out += '\'';
        put(c.cdr.cons()->car, readable, depth + 1);
        return;
      }
      out += '(';
      const Value* l = &x;
      int n = 0;
      bool cut = false;
      while (l->is_cons()) {
        if (n > 0) {
          out += ' ';
        }
        put(l->cons()->car, readable, depth + 1);
        if (over()) {
          return;
        }
        l = &l->cons()->cdr;
        if (++n > 100000) {
          out += " ...";
          cut = true;
          break;
        }
      }
      if (!cut && !l->nil()) {
        out += " . ";
        put(*l, readable, depth + 1);
      }
      out += ')';
    } else {
      out += x.obj()->show(readable);
    }
  }
};

}  // namespace

std::string show(const Value& x, bool readable) {
  Printer p{.out = {}, .max_bytes = 3 * kMaxStringUnits};
  p.put(x, readable, 0);
  if (p.full) {
    p.out.resize(3 * kMaxStringUnits);
  }
  return std::move(p.out);
}

namespace detail {

std::string show_limited(const Value& x, bool readable, std::size_t max_units) {
  // a WTF-8 byte is at most one UTF-16 unit, a unit at most three bytes
  const std::size_t cap = max_units > kMaxStringUnits ? 3 * kMaxStringUnits : 3 * max_units;
  Printer p{.out = {}, .max_bytes = cap};
  p.put(x, readable, 0);
  if (p.full || base::utf16_length(p.out) > max_units) {
    throw LispError("Invalid string length");
  }
  return std::move(p.out);
}

}  // namespace detail

std::string type_name(const Value& x) {
  if (x.nil()) {
    return {};
  }
  if (x.is_int()) {
    return "INT";
  }
  if (x.is_real()) {
    return "REAL";
  }
  if (x.is_str()) {
    return "STR";
  }
  if (x.is_sym()) {
    return "SYM";
  }
  if (x.is_cons()) {
    return "LIST";
  }
  return x.obj()->type_name();
}

// ---------------------------------------------------------------- shared helpers
namespace detail {

Sym kQuote() {
  static const Sym s = sym("QUOTE");
  return s;
}
Sym kT() {
  static const Sym s = sym("T");
  return s;
}

std::u16string u16(std::string_view s) {
  std::u16string out;
  out.reserve(s.size());
  const std::size_t n = s.size();
  const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
  const auto cont = [&](std::size_t k) { return k < n && (b(k) & 0xC0U) == 0x80U; };
  std::size_t i = 0;
  while (i < n) {
    const unsigned c = b(i);
    if (c < 0x80U) {
      out += static_cast<char16_t>(c);
      ++i;
    } else if (c >= 0xC2U && c < 0xE0U && cont(i + 1)) {
      out += static_cast<char16_t>(((c & 0x1FU) << 6U) | (b(i + 1) & 0x3FU));
      i += 2;
    } else if (c >= 0xE0U && c < 0xF0U && cont(i + 1) && cont(i + 2)) {
      out += static_cast<char16_t>(((c & 0x0FU) << 12U) | ((b(i + 1) & 0x3FU) << 6U) | (b(i + 2) & 0x3FU));
      i += 3;
    } else if (c >= 0xF0U && c < 0xF5U && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
      const std::uint32_t cp =
          ((c & 0x07U) << 18U) | ((b(i + 1) & 0x3FU) << 12U) | ((b(i + 2) & 0x3FU) << 6U) | (b(i + 3) & 0x3FU);
      const std::uint32_t v = cp - 0x10000U;
      out += static_cast<char16_t>(0xD800U + (v >> 10U));
      out += static_cast<char16_t>(0xDC00U + (v & 0x3FFU));
      i += 4;
    } else {
      out += u'\uFFFD';
      ++i;
    }
  }
  return out;
}

std::string u8(std::u16string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    const std::uint32_t c = s[i];
    if (c >= 0xD800U && c < 0xDC00U && i + 1 < s.size() && s[i + 1] >= 0xDC00U && s[i + 1] < 0xE000U) {
      base::append_code_point(out, 0x10000U + ((c - 0xD800U) << 10U) + (s[i + 1] - 0xDC00U));
      ++i;
    } else {
      base::append_code_point(out, c);
    }
  }
  return out;
}

void append_js(std::string& a, std::string_view b) {
  // a lone high surrogate (ED A0–AF xx) at the end of a and a lone low one (ED B0–BF xx) at the start of b join
  const auto byte = [](char ch) { return static_cast<unsigned char>(ch); };
  if (a.size() >= 3 && b.size() >= 3 && byte(a[a.size() - 3]) == 0xEDU && byte(a[a.size() - 2]) >= 0xA0U &&
      byte(a[a.size() - 2]) < 0xB0U && byte(b[0]) == 0xEDU && byte(b[1]) >= 0xB0U && byte(b[1]) < 0xC0U) {
    const std::u16string hi = u16(std::string_view(a).substr(a.size() - 3));
    const std::u16string lo = u16(b.substr(0, 3));
    a.resize(a.size() - 3);
    a += u8(hi + lo);
    a.append(b.substr(3));
    return;
  }
  a.append(b);
}

}  // namespace detail

// ---------------------------------------------------------------- host data
// NOLINTNEXTLINE(misc-no-recursion) — host data is a parsed JSON-shaped value, depth-capped where it is made
Value to_lisp(const base::Value& v) {
  if (v.is_undefined() || v.is_null() || (v.is_bool() && !v.as_bool())) {
    return {};
  }
  if (v.is_bool()) {
    return {detail::kT()};
  }
  if (v.is_number()) {
    return {v.as_number()};
  }
  if (v.is_string()) {
    return {v.as_string()};
  }
  if (v.is_array()) {
    std::vector<Value> items;
    items.reserve(v.as_array().size());
    for (const base::Value& x : v.as_array()) {
      items.push_back(to_lisp(x));
    }
    return list(std::move(items));
  }
  if (v.is_object()) {
    if (v.has("i")) {
      return Value(Int{base::js_to_int32(base::js_to_number(v.get("i")))});
    }
    if (v.has("e")) {
      return {ObjRef(std::make_shared<Ename>(v.get("e")))};
    }
    if (v.has("ids")) {
      const base::Value ids = v.get("ids");
      std::vector<base::Value> all;
      if (ids.is_array()) {
        all = ids.as_array();
      }
      return {ObjRef(std::make_shared<PickSet>(std::move(all)))};
    }
    if (v.has("sym")) {
      return {sym(base::js_string(v.get("sym")))};
    }
  }
  return {};
}

}  // namespace cc::lisp
