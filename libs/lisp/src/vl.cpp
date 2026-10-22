// SPDX-License-Identifier: Apache-2.0
// vl.js: the vl- functions that need no drawing — lists (vl-sort … vl-consp), strings (vl-string-search …
// vl-prin1-to-string), symbols and the blackboard, errors (vl-catch-all-apply …), files and the registry (none here).
// Strings are indexed in UTF-16 units and iterated by code point where the JS spreads them ([...s]).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/lisp/lisp.hpp"
#include "internal.hpp"

namespace cc::lisp::detail {

namespace {

constexpr auto kNpos = std::u16string_view::npos;

Task<bool> test(Interp& L, const Value& f, const Value& x) {
  co_return (co_await L.apply(f, Args{x})).truthy();
}

// ---- lists
struct Item {
  Value v;
  std::size_t i = 0;
};

/// The JS's stable merge sort with the routine's comparison, comparison for comparison (a comparison may print).
// NOLINTNEXTLINE(misc-no-recursion) — log2(n) deep
Task<std::vector<Item>> merge(const std::vector<Item>& a, std::size_t lo, std::size_t hi, const Value& f, Interp& L) {
  if (hi - lo < 2) {
    co_return std::vector<Item>(a.begin() + static_cast<std::ptrdiff_t>(lo),
                                a.begin() + static_cast<std::ptrdiff_t>(hi));
  }
  const std::size_t mid = (lo + hi) >> 1U;
  const std::vector<Item> left = co_await merge(a, lo, mid, f, L);
  const std::vector<Item> right = co_await merge(a, mid, hi, f, L);
  std::vector<Item> out;
  out.reserve(hi - lo);
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left.size() && j < right.size()) {
    if ((co_await L.apply(f, Args{right[j].v, left[i].v})).truthy()) {
      out.push_back(right[j++]);
    } else {
      out.push_back(left[i++]);
    }
  }
  out.insert(out.end(), left.begin() + static_cast<std::ptrdiff_t>(i), left.end());
  out.insert(out.end(), right.begin() + static_cast<std::ptrdiff_t>(j), right.end());
  co_return out;
}
Task<std::vector<Item>> sort_by(const Value& l, const Value& f, Interp& L) {
  std::vector<Item> a;
  std::size_t k = 0;
  for (Value& v : to_vector(l)) {
    a.push_back(Item{.v = std::move(v), .i = k++});
  }
  L.charge(a.size());
  co_return co_await merge(a, 0, a.size(), f, L);
}
Task<Value> f_sort(Args a, Interp& L) {
  arity(a, 2);
  const Value& l = lst(a[0]);
  const Value f = L.fn_of(a[1]);
  std::vector<Value> out;
  for (Item& x : co_await sort_by(l, f, L)) {
    out.push_back(std::move(x.v));
  }
  co_return list(std::move(out));
}
Task<Value> f_sort_i(Args a, Interp& L) {
  arity(a, 2);
  const Value& l = lst(a[0]);
  const Value f = L.fn_of(a[1]);
  std::vector<Value> out;
  for (const Item& x : co_await sort_by(l, f, L)) {
    out.push_back(int_of(static_cast<double>(x.i)));
  }
  co_return list(std::move(out));
}
Value f_remove(Args& a, Interp& L) {
  arity(a, 2);
  std::vector<Value> out;
  for (Value& x : to_vector(lst(a[1]))) {
    L.charge(1);
    if (!equal(L, x, a[0])) {
      out.push_back(std::move(x));
    }
  }
  return list(std::move(out));
}
Task<Value> remove_if(Args a, Interp& L, bool keep) {
  arity(a, 2);
  const Value f = L.fn_of(a[0]);
  std::vector<Value> out;
  for (Value& x : to_vector(lst(a[1]))) {
    L.charge(1);
    if (co_await test(L, f, x) == keep) {
      out.push_back(std::move(x));
    }
  }
  co_return list(std::move(out));
}
Task<Value> f_remove_if(Args a, Interp& L) {
  co_return co_await remove_if(std::move(a), L, false);
}
Task<Value> f_remove_if_not(Args a, Interp& L) {
  co_return co_await remove_if(std::move(a), L, true);
}
Task<Value> member_if(Args a, Interp& L, bool want) {
  arity(a, 2);
  const Value f = L.fn_of(a[0]);
  for (Value x = lst(a[1]); x.is_cons(); x = Value(x.cons()->cdr)) {
    if (co_await test(L, f, x.cons()->car) == want) {
      co_return x;
    }
  }
  co_return Value();
}
Task<Value> f_member_if(Args a, Interp& L) {
  co_return co_await member_if(std::move(a), L, true);
}
Task<Value> f_member_if_not(Args a, Interp& L) {
  co_return co_await member_if(std::move(a), L, false);
}
struct Lists {
  std::vector<std::vector<Value>> ls;
  std::size_t n = 0;
};
Lists lists_of(const Args& a) {
  Lists r;
  for (std::size_t k = 1; k < a.size(); ++k) {
    r.ls.push_back(to_vector(lst(a[k])));
  }
  check_spread(r.ls.size());  // Math.min(...ls.map(…))
  r.n = std::numeric_limits<std::size_t>::max();
  for (const auto& x : r.ls) {
    r.n = std::min(r.n, x.size());
  }
  return r;
}
Args column(const Lists& l, std::size_t i) {
  Args out;
  out.reserve(l.ls.size());
  for (const auto& x : l.ls) {
    out.push_back(x[i]);
  }
  return out;
}
Task<Value> f_some(Args a, Interp& L) {
  arity(a, 2, kMany);
  const Value f = L.fn_of(a[0]);
  const Lists l = lists_of(a);
  for (std::size_t i = 0; i < l.n; ++i) {
    Value r = co_await L.apply(f, column(l, i));
    if (r.truthy()) {
      co_return r;
    }
  }
  co_return Value();
}
Task<Value> f_every(Args a, Interp& L) {
  arity(a, 2, kMany);
  const Value f = L.fn_of(a[0]);
  const Lists l = lists_of(a);
  for (std::size_t i = 0; i < l.n; ++i) {
    if (!(co_await L.apply(f, column(l, i))).truthy()) {
      co_return Value();
    }
  }
  co_return Value(kT());
}
Value f_position(Args& a, Interp& L) {
  arity(a, 2);
  double i = 0;
  for (const Value* x = &lst(a[1]); x->is_cons(); x = &x->cons()->cdr, ++i) {
    if (equal(L, x->cons()->car, a[0])) {
      return int_of(i);
    }
  }
  return {};
}
Value f_list_star(Args& a, Interp& /*L*/) {
  arity(a, 1, kMany);
  Value tail = std::move(a.back());
  a.pop_back();
  return list(std::move(a), std::move(tail));
}
Value f_list_length(Args& a, Interp& /*L*/) {
  arity(a, 1);
  (void)lst(a[0]);
  if (!is_proper(a[0])) {
    return {};
  }
  double n = 0;
  const Value& first = a[0];
  for (const Value* x = &first; x->is_cons(); x = &x->cons()->cdr) {
    ++n;
  }
  return int_of(n);
}
Value f_consp(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return boolv(a[0].is_cons());
}

// ---- strings
/// String#indexOf(pat, from)
std::size_t index_of(std::u16string_view s, std::u16string_view pat, double from) {
  const double f = std::min(std::max(from, 0.0), static_cast<double>(s.size()));
  return s.find(pat, static_cast<std::size_t>(f));
}
Value units_value(Interp& L, std::u16string_view u) {
  L.check_string(u.size());
  L.charge(u.size());
  return {u8(u)};
}
Value f_search(Args& a, Interp& /*L*/) {
  arity(a, 2, 3);
  const std::u16string s = u16(str(a[1]));
  const std::u16string pat = u16(str(a[0]));
  const double from = a.size() > 2 ? intv(a[2]) : 0;
  const std::size_t i = index_of(s, pat, from);
  return i == kNpos ? Value() : int_of(static_cast<double>(i));
}
Value f_subst(Args& a, Interp& L) {
  arity(a, 3, 4);
  const std::u16string nw = u16(str(a[0]));
  const std::u16string pat = u16(str(a[1]));
  const std::u16string s = u16(str(a[2]));
  const double from = a.size() > 3 ? intv(a[3]) : 0;
  const std::size_t i = index_of(s, pat, from);
  if (i == kNpos) {
    return a[2];
  }
  const std::u16string r = s.substr(0, i) + nw + s.substr(i + pat.size());
  return units_value(L, r);
}
Value trim_set(Interp& L, const Value& set_v, const Value& s_v, bool left, bool right) {
  const std::u16string set = u16(str(set_v));
  const std::u16string s = u16(str(s_v));
  std::size_t i = 0;
  std::size_t j = s.size();
  if (left) {
    while (i < j && set.find(s[i]) != kNpos) {
      ++i;
    }
  }
  if (right) {
    while (j > i && set.find(s[j - 1]) != kNpos) {
      --j;
    }
  }
  return units_value(L, std::u16string_view(s).substr(i, j - i));
}
Value f_trim(Args& a, Interp& L) {
  arity(a, 2);
  return trim_set(L, a[0], a[1], true, true);
}
Value f_left_trim(Args& a, Interp& L) {
  arity(a, 2);
  return trim_set(L, a[0], a[1], true, false);
}
Value f_right_trim(Args& a, Interp& L) {
  arity(a, 2);
  return trim_set(L, a[0], a[1], false, true);
}
/// [...s]: the code points of s as their UTF-16 units (a pair stays together; a lone surrogate is one)
std::vector<std::u16string_view> code_points(std::u16string_view s) {
  std::vector<std::u16string_view> out;
  for (std::size_t i = 0; i < s.size();) {
    const bool pair = s[i] >= 0xD800 && s[i] < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000;
    const std::size_t n = pair ? 2 : 1;
    out.push_back(s.substr(i, n));
    i += n;
  }
  return out;
}
Value f_translate(Args& a, Interp& L) {
  arity(a, 3);
  const std::u16string src = u16(str(a[0]));
  const std::u16string dst = u16(str(a[1]));
  const std::u16string s = u16(str(a[2]));
  std::u16string out;
  for (const std::u16string_view c : code_points(s)) {
    const std::size_t k = src.find(c);
    if (k != kNpos && !dst.empty()) {
      out += k < dst.size() ? dst[k] : dst.back();  // dst[k], else dst's last, else c itself
    } else {
      out += c;
    }
  }
  return units_value(L, out);
}
Value f_position_char(Args& a, Interp& /*L*/) {
  arity(a, 2, 4);
  const auto ch = static_cast<char16_t>(static_cast<std::uint32_t>(intv(a[0])) & 0xFFFFU);
  const std::u16string s = u16(str(a[1]));
  std::optional<double> start;
  if (a.size() > 2 && !a[2].nil()) {
    start = intv(a[2]);
  }
  const bool back = a.size() > 3 && a[3].truthy();
  std::size_t i = kNpos;
  if (back) {
    // lastIndexOf(ch, from): the last match that starts at or before from (clamped to 0 … length)
    const double from =
        std::min(std::max(start.value_or(static_cast<double>(s.size())), 0.0), static_cast<double>(s.size()));
    if (!s.empty()) {
      i = s.rfind(ch, static_cast<std::size_t>(from));
    }
  } else {
    i = index_of(s, std::u16string(1, ch), start.value_or(0));
  }
  return i == kNpos ? Value() : int_of(static_cast<double>(i));
}
Value f_mismatch(Args& a, Interp& /*L*/) {
  arity(a, 2, 5);
  const std::u16string s1 = u16(str(a[0]));
  const std::u16string s2 = u16(str(a[1]));
  const double p1 = a.size() > 2 && js_truthy(a[2]) ? intv(a[2]) : 0;
  const double p2 = a.size() > 3 && js_truthy(a[3]) ? intv(a[3]) : 0;
  const bool ic = a.size() > 4 && a[4].truthy();
  // s[k] is undefined outside 0 … length - 1; undefined === undefined, and undefined.toUpperCase() throws
  const auto at = [](const std::u16string& s, double k) -> std::optional<char16_t> {
    return k >= 0 && k < static_cast<double>(s.size()) ? std::optional<char16_t>(s[static_cast<std::size_t>(k)])
                                                       : std::nullopt;
  };
  const auto upper = [](std::optional<char16_t> c) {
    if (!c) {
      fail("Cannot read properties of undefined (reading 'toUpperCase')");
    }
    return base::js_to_upper(u8(std::u16string(1, *c)));
  };
  double n = 0;
  while (p1 + n < static_cast<double>(s1.size()) && p2 + n < static_cast<double>(s2.size())) {
    const std::optional<char16_t> c1 = at(s1, p1 + n);
    const std::optional<char16_t> c2 = at(s2, p2 + n);
    if (ic) {
      const std::string u1 = upper(c1);
      if (upper(c2) != u1) {
        break;
      }
    } else if (c1 != c2) {
      break;
    }
    ++n;
  }
  return int_of(n);
}
Value f_elt(Args& a, Interp& /*L*/) {
  arity(a, 2);
  const std::u16string s = u16(str(a[0]));
  const std::int32_t i = intv(a[1]);
  if (i < 0 || static_cast<std::size_t>(i) >= s.size()) {
    fail("bad argument value: " + show(a[1]));
  }
  return Value(Int{static_cast<std::int32_t>(s[static_cast<std::size_t>(i)])});
}
Value f_string_to_list(Args& a, Interp& L) {
  arity(a, 1);
  const std::u16string s = u16(str(a[0]));
  L.charge(s.size());
  std::vector<Value> out;
  for (const std::u16string_view c : code_points(s)) {
    out.emplace_back(Int{static_cast<std::int32_t>(c[0])});  // charCodeAt(0): a pair's high surrogate
  }
  return list(std::move(out));
}
Value f_list_to_string(Args& a, Interp& L) {
  arity(a, 1);
  std::u16string out;
  for (const Value& c : to_vector(lst(a[0]))) {
    out += static_cast<char16_t>(static_cast<std::uint32_t>(intv(c)) & 0xFFFFU);
  }
  return units_value(L, out);
}
Value f_princ_to_string(Args& a, Interp& L) {
  arity(a, 1);
  return {show_for(L, a[0], false)};
}
Value f_prin1_to_string(Args& a, Interp& L) {
  arity(a, 1);
  return {show_for(L, a[0], true)};
}

// ---- symbols
Value f_symbolp(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return boolv(a[0].is_sym());
}
Value f_symbol_name(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return {symv(a[0])->name};
}
Value f_symbol_value(Args& a, Interp& L) {
  arity(a, 1);
  return L.get(symv(a[0]));
}
Value f_bb_set(Args& a, Interp& L) {
  arity(a, 2);
  L.bb[symv(a[0])] = a[1];
  return a[1];
}
Value f_bb_ref(Args& a, Interp& L) {
  arity(a, 1);
  const auto it = L.bb.find(symv(a[0]));
  return it == L.bb.end() ? Value() : it->second;
}
Value f_doc_set(Args& a, Interp& L) {
  arity(a, 2);
  return L.set(symv(a[0]), a[1]);
}

// ---- errors
struct Catching {
  int& n;
  explicit Catching(int& at) : n(at) { ++n; }
  Catching(const Catching&) = delete;
  Catching& operator=(const Catching&) = delete;
  Catching(Catching&&) = delete;
  Catching& operator=(Catching&&) = delete;
  ~Catching() { --n; }
};
Task<Value> f_catch_all_apply(Args a, Interp& L) {
  arity(a, 2);
  const Catching guard(L.catching);
  std::optional<LispError> err;
  Value r;
  try {
    Value f = L.fn_of(a[0]);
    r = co_await L.apply(std::move(f), to_vector(lst(a[1])));
  } catch (...) {
    err = to_lisp_error(std::current_exception());
  }
  if (!err) {
    co_return r;
  }
  if (err->cancel) {
    throw LispError(*err);  // Esc still stops the routine
  }
  co_return Value(ObjRef(std::make_shared<CatchError>(err->message())));
}
Value f_catch_error_p(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return boolv(a[0].as<CatchError>() != nullptr);
}
Value f_catch_error_message(Args& a, Interp& /*L*/) {
  arity(a, 1);
  const auto* e = a[0].as<CatchError>();
  if (e == nullptr) {
    fail("bad argument type: catch-all-apply-error: " + show(a[0]));
  }
  return {e->message};
}
Value f_exit_with_value(Args& a, Interp& /*L*/) {
  arity(a, 1);
  LispError e{std::string(kMsgQuit)};
  e.value = std::make_shared<const Value>(a[0]);
  throw LispError(e);
}
Value f_exit_with_error(Args& a, Interp& /*L*/) {
  arity(a, 1);
  fail(str(a[0]));
}

// ---- files and the registry
Value f_directory_files(Args& a, Interp& L) {
  const Value pv = a.size() > 1 ? a[1] : Value();
  const std::string pat = js_truthy(pv) ? str(pv) : std::string("*.*");
  // new RegExp('^' + escaped, 'i') with * → .*, ? → . (neither crosses a line terminator)
  std::vector<GlobEl> g;
  for (const char16_t c : u16(pat)) {
    if (c == u'*') {
      g.push_back(GlobEl{.k = GlobEl::K::star, .c = 0, .ranges = {}, .inv = false, .nolt = true});
    } else if (c == u'?') {
      g.push_back(GlobEl{.k = GlobEl::K::any, .c = 0, .ranges = {}, .inv = false, .nolt = true});
    } else {
      g.push_back(GlobEl{.k = GlobEl::K::lit, .c = c, .ranges = {}, .inv = false, .nolt = false});
    }
  }
  std::vector<Value> out;
  for (const Interp::File& f : L.files()) {
    std::string p = f.name;
    std::ranges::replace(p, '\\', '/');
    const std::size_t s = p.rfind('/');
    std::string n = s == std::string::npos ? p : p.substr(s + 1);
    if (glob_match(g, u16(n), true)) {
      out.emplace_back(std::move(n));
    }
  }
  return list(std::move(out));
}
Value f_file_copy(Args& a, Interp& L) {
  arity(a, 2, 3);
  const Interp::File* f = L.find_file(str(a[0]));
  if (f == nullptr) {
    return {};
  }
  const std::string text = f->text;
  const std::string& dst = str(a[1]);
  std::string all;
  if (a.size() > 2 && a[2].truthy()) {
    if (const Interp::File* old = L.find_file(dst); old != nullptr) {
      all = old->text;
    }
  }
  L.check_string(base::utf16_length(all) + base::utf16_length(text));
  append_js(all, text);
  L.put_file(dst, std::move(all));
  return int_of(static_cast<double>(base::utf16_length(text)));
}
Value f_t(Args& /*a*/, Interp& /*L*/) {
  return {kT()};
}
Value f_nil(Args& /*a*/, Interp& /*L*/) {
  return {};
}

}  // namespace

void install_vl(Interp& L) {
  L.def("VL-SORT", &f_sort);
  L.def("VL-SORT-I", &f_sort_i);
  L.def("VL-REMOVE", &f_remove);
  L.def("VL-REMOVE-IF", &f_remove_if);
  L.def("VL-REMOVE-IF-NOT", &f_remove_if_not);
  L.def("VL-MEMBER-IF", &f_member_if);
  L.def("VL-MEMBER-IF-NOT", &f_member_if_not);
  L.def("VL-SOME", &f_some);
  L.def("VL-EVERY", &f_every);
  L.def("VL-POSITION", &f_position);
  L.def("VL-LIST*", &f_list_star);
  L.def("VL-LIST-LENGTH", &f_list_length);
  L.def("VL-CONSP", &f_consp);
  L.def("VL-STRING-SEARCH", &f_search);
  L.def("VL-STRING-SUBST", &f_subst);
  L.def("VL-STRING-TRIM", &f_trim);
  L.def("VL-STRING-LEFT-TRIM", &f_left_trim);
  L.def("VL-STRING-RIGHT-TRIM", &f_right_trim);
  L.def("VL-STRING-TRANSLATE", &f_translate);
  L.def("VL-STRING-POSITION", &f_position_char);
  L.def("VL-STRING-MISMATCH", &f_mismatch);
  L.def("VL-STRING-ELT", &f_elt);
  L.def("VL-STRING->LIST", &f_string_to_list);
  L.def("VL-LIST->STRING", &f_list_to_string);
  L.def("VL-PRINC-TO-STRING", &f_princ_to_string);
  L.def("VL-PRIN1-TO-STRING", &f_prin1_to_string);
  L.def("VL-SYMBOLP", &f_symbolp);
  L.def("VL-SYMBOL-NAME", &f_symbol_name);
  L.def("VL-SYMBOL-VALUE", &f_symbol_value);
  L.def("VL-BB-SET", &f_bb_set);
  L.def("VL-BB-REF", &f_bb_ref);
  L.def("VL-DOC-SET", &f_doc_set);
  L.def("VL-DOC-REF", &f_symbol_value);
  L.def("VL-CATCH-ALL-APPLY", &f_catch_all_apply);
  L.def("VL-CATCH-ALL-ERROR-P", &f_catch_error_p);
  L.def("VL-CATCH-ALL-ERROR-MESSAGE", &f_catch_error_message);
  L.def("VL-EXIT-WITH-VALUE", &f_exit_with_value);
  L.def("VL-EXIT-WITH-ERROR", &f_exit_with_error);
  L.def("VL-DIRECTORY-FILES", &f_directory_files);
  L.def("VL-FILE-COPY", &f_file_copy);
  L.def("VL-MKDIR", &f_t);
  L.def("VL-REGISTRY-READ", &f_nil);
  L.def("VL-REGISTRY-WRITE", &f_nil);
  L.def("VL-REGISTRY-DESCENDENTS", &f_nil);
  L.def("VL-GET-RESOURCE", &f_nil);
}

}  // namespace cc::lisp::detail
