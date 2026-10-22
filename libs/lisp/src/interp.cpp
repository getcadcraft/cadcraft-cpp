// SPDX-License-Identifier: Apache-2.0
// interp.js: the evaluator (dynamic scope, *error*, the special forms), the in-memory files, runtime.js and missing.js.
#include <algorithm>
#include <array>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/i18n/i18n.hpp"
#include "cc/lisp/lisp.hpp"
#include "internal.hpp"

namespace cc::lisp {

using detail::kT;
using detail::to_lisp_error;

namespace {

LispError cancel_error() {
  LispError e{std::string(kMsgCancel)};
  e.cancel = true;
  return e;
}

Result ok_result(Value v) {
  Result r;
  r.value = std::move(v);
  return r;
}
Result fail_result(std::string error, bool cancel = false) {
  Result r;
  r.ok = false;
  r.error = std::move(error);
  r.cancel = cancel;
  return r;
}

Sym k_slash() {
  static const Sym s = sym("/");
  return s;
}
Sym k_lambda() {
  static const Sym s = sym("LAMBDA");
  return s;
}
Sym k_error() {
  static const Sym s = sym("*ERROR*");
  return s;
}

bool callable(const Value& v) {
  return v.as<Lambda>() != nullptr || v.as<Builtin>() != nullptr || v.is_cons();
}

}  // namespace

/// Any thrown thing as the LispError it becomes (interp.js lispError), copied.
LispError detail::to_lisp_error(const std::exception_ptr& ep) {
  try {
    std::rethrow_exception(ep);
  } catch (const LispError& e) {
    return e;
  } catch (const std::bad_alloc&) {
    return LispError("out of memory");
  } catch (const std::exception& e) {
    return LispError(e.what());
  } catch (...) {
    return LispError("unknown error");
  }
}

namespace {

/// missing.js: AutoLISP / Visual LISP functions 0.5 does not have yet, said once after "no function definition".
bool known_missing(std::string_view n) {
  static constexpr std::array<std::string_view, 50> kNames = {"GRREAD",
                                                              "DICTADD",
                                                              "DICTNEXT",
                                                              "DICTREMOVE",
                                                              "DICTRENAME",
                                                              "DICTSEARCH",
                                                              "NAMEDOBJDICT",
                                                              "XDROOM",
                                                              "XDSIZE",
                                                              "SNVALID",
                                                              "XLOAD",
                                                              "XUNLOAD",
                                                              "ARX",
                                                              "ARXLOAD",
                                                              "ARXUNLOAD",
                                                              "AUTOARXLOAD",
                                                              "AUTOLOAD",
                                                              "AUTOXLOAD",
                                                              "VLARTS-INIT",
                                                              "ACAD_HELPDLG",
                                                              "INITDIA",
                                                              "TABLET",
                                                              "SETCFG",
                                                              "GETCFG",
                                                              "VLAX-ADD-CMD",
                                                              "VLAX-REMOVE-CMD",
                                                              "VLAX-GET-OR-CREATE-OBJECT",
                                                              "VLAX-CREATE-OBJECT",
                                                              "VLAX-GET-OBJECT",
                                                              "VLAX-IMPORT-TYPE-LIBRARY",
                                                              "VLAX-LDATA-PUT",
                                                              "VLAX-LDATA-GET",
                                                              "VLAX-LDATA-DELETE",
                                                              "VLAX-LDATA-LIST",
                                                              "VLAX-LDATA-TEST",
                                                              "VLAX-DUMP-OBJECT",
                                                              "VLAX-TMATRIX",
                                                              "VLAX-MAP-COLLECTION",
                                                              "VLAX-PRODUCT-KEY",
                                                              "VLAX-MACHINE-PRODUCT-KEY",
                                                              "VLAX-USER-PRODUCT-KEY",
                                                              "VL-VBALOAD",
                                                              "VL-VBARUN",
                                                              "VL-VLX-LOADED-P",
                                                              "VL-LIST-LOADED-VLX",
                                                              "VL-UNLOAD-VLX",
                                                              "VL-LIST-EXPORTED-FUNCTIONS",
                                                              "VL-ANNOTATIVE-GET",
                                                              "VL-ANNOTATIVE-SET",
                                                              ""};
  static constexpr std::array<std::string_view, 5> kPrefixes = {"VLR-", "VLAX-CURVE-", "ACET-", "DOS_", "VLAX-LDATA-"};
  return (!n.empty() && std::ranges::find(kNames, n) != kNames.end()) ||
         std::ranges::any_of(kPrefixes, [&](std::string_view p) { return n.starts_with(p); });
}

}  // namespace

// ---------------------------------------------------------------- the global Map
const Value* Globals::find(Sym s) const {
  const auto it = index_.find(s);
  return it == index_.end() ? nullptr : &it->second.v;
}
void Globals::set(Sym s, Value v) {
  if (const auto it = index_.find(s); it != index_.end()) {
    it->second.v = std::move(v);
    return;
  }
  index_.emplace(s, Slot{std::move(v), next_++});
}
void Globals::erase(Sym s) {
  index_.erase(s);
}
std::vector<std::pair<Sym, Value>> Globals::entries() const {
  std::vector<std::pair<std::uint64_t, std::pair<Sym, Value>>> all;
  all.reserve(index_.size());
  for (const auto& [k, slot] : index_) {
    all.push_back({slot.order, {k, slot.v}});
  }
  std::ranges::sort(all, {}, [](const auto& e) { return e.first; });
  std::vector<std::pair<Sym, Value>> out;
  out.reserve(all.size());
  for (auto& e : all) {
    out.push_back(std::move(e.second));
  }
  return out;
}

// ---------------------------------------------------------------- the interpreter
Interp::Interp(Options o) : opt_(std::move(o)) {
  set(kT(), Value(kT()));
  detail::install_specials(*this);
  detail::install_builtins(*this);
  detail::install_vl(*this);
}
Interp::~Interp() = default;

Value Interp::get(Sym s) const {
  const Value* v = globals_.find(s);
  return v == nullptr ? Value() : *v;
}
Value Interp::set(Sym s, Value v) {
  if (v.nil()) {
    globals_.erase(s);
  } else {
    globals_.set(s, v);
  }
  return v;
}
std::optional<Value> Interp::saved(Sym s) const {
  const Value* v = globals_.find(s);
  return v == nullptr ? std::nullopt : std::optional<Value>(*v);
}
void Interp::restore(Sym s, std::optional<Value> v) {
  if (!v) {
    globals_.erase(s);
  } else {
    globals_.set(s, std::move(*v));
  }
}
void Interp::def(std::string_view name, SyncFn f) {
  globals_.set(sym(name), Value(ObjRef(std::make_shared<Builtin>(base::js_to_upper(name), f))));
}
void Interp::def(std::string_view name, AsyncFn f) {
  globals_.set(sym(name), Value(ObjRef(std::make_shared<Builtin>(base::js_to_upper(name), f))));
}
void Interp::def(std::string_view name, SpecialFn f) {
  globals_.set(sym(name), Value(ObjRef(std::make_shared<Builtin>(base::js_to_upper(name), f))));
}
void Interp::out(const std::string& text, bool piece) const {
  if (opt_.out) {
    opt_.out(text, piece);
  }
}

void Interp::tick() const {
  if (stopped_ || (opt_.max_steps != 0 && steps_ > opt_.max_steps)) {
    throw cancel_error();
  }
}
void Interp::charge(std::size_t n) {
  steps_ += n;
  if (opt_.max_steps != 0 && steps_ > opt_.max_steps) {
    tick();
  }
}
void Interp::step() {
  ++steps_;
  if ((steps_ & 4095U) == 0) {
    tick();
  }
}

LispError Interp::no_function(Sym s) {
  LispError e("no function definition: " + s->name);
  if (missing_said_.insert(s->name).second && known_missing(s->name)) {
    e.note = std::make_shared<const std::string>(opt_.words != nullptr
                                                     ? opt_.words->t("lisp.missing.known", {{"name", s->name}})
                                                     : std::string("lisp.missing.known"));
  }
  return e;
}

Value Interp::auto_fn_of(Sym s) {
  Value f = auto_fn ? auto_fn(s->name) : Value();
  if (f.nil()) {
    throw no_function(s);
  }
  globals_.set(s, f);
  return f;
}

Value Interp::fn_of(const Value& f0) {
  Value f = f0;
  for (int hops = 0; f.is_sym(); ++hops) {  // fnOf calls itself for a symbol's value
    if (hops >= kMaxIndirection) {
      throw LispError(std::string(kMsgDeep));
    }
    const Value* v = globals_.find(f.sym());
    f = v == nullptr ? auto_fn_of(f.sym()) : *v;
  }
  if (f.is_cons()) {
    if (f.cons()->car.is_sym() && f.cons()->car.sym() == k_lambda()) {
      return make_lambda({}, f.cons()->cdr);
    }
    return make_lambda({}, f);  // defun-q style: ((args / locals) body…)
  }
  if (f.as<Lambda>() != nullptr || f.as<Builtin>() != nullptr) {
    return f;
  }
  throw LispError("bad function: " + show(f));
}

Value Interp::make_lambda(std::string name, const Value& rest) const {
  if (!rest.is_cons()) {
    throw LispError("bad argument list");
  }
  std::vector<Sym> params;
  std::vector<Sym> locals;
  bool in_locals = false;
  for (const Value& p : to_vector(rest.cons()->car)) {
    if (p.is_sym() && p.sym() == k_slash()) {
      in_locals = true;
      continue;
    }
    if (!p.is_sym()) {
      throw LispError("bad argument list: " + show(rest.cons()->car));
    }
    (in_locals ? locals : params).push_back(p.sym());
  }
  return {ObjRef(
      std::make_shared<Lambda>(std::move(name), std::move(params), std::move(locals), to_vector(rest.cons()->cdr)))};
}

namespace {

/// One level of nesting (kMaxDepth): V8 has no such limit for the awaiting evaluator, only memory.
struct Depth {
  int& d;
  explicit Depth(int& at) : d(at) {
    if (d >= Interp::kMaxDepth) {
      throw LispError(std::string(kMsgDeep));
    }
    ++d;
  }
  Depth(const Depth&) = delete;
  Depth& operator=(const Depth&) = delete;
  Depth(Depth&&) = delete;
  Depth& operator=(Depth&&) = delete;
  ~Depth() { --d; }
};

/// A function's arguments and locals bound while it runs; the values before restored when it ends, however it ends
/// (a return, an error, or its frame destroyed by Esc).
class Bindings {
 public:
  Bindings(Interp& L, const Lambda& f, Args& args) : L_(L), f_(f) {
    saved_.reserve(f.params.size() + f.locals.size());
    for (const Sym s : f.params) {
      saved_.push_back(L.saved(s));
    }
    for (const Sym s : f.locals) {
      saved_.push_back(L.saved(s));
    }
    for (std::size_t i = 0; i < f.params.size(); ++i) {
      L.set(f.params[i], std::move(args[i]));
    }
    for (const Sym s : f.locals) {
      L.restore(s, std::nullopt);
    }
  }
  Bindings(const Bindings&) = delete;
  Bindings& operator=(const Bindings&) = delete;
  Bindings(Bindings&&) = delete;
  Bindings& operator=(Bindings&&) = delete;
  ~Bindings() {
    std::size_t k = 0;
    for (const Sym s : f_.params) {
      restore(s, k++);
    }
    for (const Sym s : f_.locals) {
      restore(s, k++);
    }
  }

 private:
  Interp& L_;
  const Lambda& f_;
  std::vector<std::optional<Value>> saved_;

  void restore(Sym s, std::size_t k) noexcept {
    try {
      L_.restore(s, std::move(saved_[k]));
    } catch (...) {  // NOLINT(bugprone-empty-catch) out of memory putting a global back: it stays unbound
    }
  }
};

}  // namespace

// NOLINTNEXTLINE(misc-no-recursion) — depth-capped (kMaxDepth); the frames are on the heap
Task<Value> Interp::eval(Value x) {
  if (x.is_sym()) {
    co_return get(x.sym());
  }
  if (!x.is_cons()) {
    co_return x;
  }
  const Depth depth_guard(depth);
  step();
  const Value head = x.cons()->car;
  Value f;
  if (head.is_sym()) {
    const Value* g = globals_.find(head.sym());
    f = g == nullptr ? auto_fn_of(head.sym()) : *g;
  } else if (head.is_cons()) {
    f = co_await eval(head);
  } else {
    throw LispError("bad function: " + show(head));
  }
  if (const auto* b = f.as<Builtin>(); b != nullptr && b->special != nullptr) {
    co_return co_await b->special(x.cons()->cdr, *this);
  }
  Args args;
  for (const Value* l = &x.cons()->cdr; l->is_cons(); l = &l->cons()->cdr) {
    const Value& a = l->cons()->car;
    if (a.is_sym()) {
      args.push_back(get(a.sym()));
    } else if (a.is_cons()) {
      args.push_back(co_await eval(a));
    } else {
      args.push_back(a);
    }
  }
  co_return co_await apply(std::move(f), std::move(args), head);
}

// NOLINTNEXTLINE(misc-no-recursion) — depth-capped (kMaxDepth)
Task<Value> Interp::apply(Value f, Args args, Value head) {
  if (f.as<Lambda>() == nullptr && f.as<Builtin>() == nullptr) {
    if (!f.is_sym() && !f.is_cons()) {
      const Value& shown = head.nil() ? f : head;
      throw LispError("bad function: " + show(shown));
    }
    f = fn_of(f);
  }
  if (const auto* b = f.as<Builtin>()) {
    if (b->special != nullptr) {
      throw LispError("bad function: " + b->name);
    }
    if (b->sync != nullptr) {
      co_return b->sync(args, *this);
    }
    co_return co_await b->async(std::move(args), *this);
  }
  const Lambda* lp = f.as<Lambda>();
  if (lp == nullptr) {  // fn_of gives a Lambda or a Builtin
    throw LispError("bad function: " + show(f));
  }
  const Lambda& fn = *lp;
  if (args.size() < fn.params.size()) {
    throw LispError("too few arguments");
  }
  if (args.size() > fn.params.size()) {
    throw LispError("too many arguments");
  }
  const Depth depth_guard(depth);
  const Bindings bound(*this, fn, args);
  std::optional<LispError> err;
  Value r;
  try {
    for (const Value& form : fn.body) {
      if (form.is_cons()) {
        r = co_await eval(form);
      } else {
        r = form.is_sym() ? get(form.sym()) : form;
      }
    }
  } catch (...) {
    err = to_lisp_error(std::current_exception());
  }
  if (err) {
    if (!err->handled && catching == 0) {
      co_await handle_error(*err);  // while this function's locals are still bound: a local *error* hears it
    }
    throw LispError(*err);
  }
  co_return r;
}

void Interp::check_string(std::size_t units) const {
  if (units > opt_.max_string) {
    throw LispError("Invalid string length");
  }
}

void detail::check_spread(std::size_t n) {
  if (n > kMaxSpread) {
    throw LispError(std::string(kMsgDeep));
  }
}

// NOLINTNEXTLINE(misc-no-recursion) — a handler's own error is not handed to it again (in_error_)
Task<void> Interp::handle_error(LispError& e) {
  e.handled = true;
  if (e.note) {
    out(*e.note);
  }
  const Value* h = globals_.find(k_error());
  if (in_error_ || h == nullptr || !callable(*h)) {
    out("; error: " + e.message());
    co_return;
  }
  const Value handler = *h;
  in_error_ = true;
  struct Reset {
    bool& b;
    explicit Reset(bool& at) : b(at) {}
    Reset(const Reset&) = delete;
    Reset& operator=(const Reset&) = delete;
    Reset(Reset&&) = delete;
    Reset& operator=(Reset&&) = delete;
    ~Reset() { b = false; }
  } const reset(in_error_);
  std::optional<LispError> again;
  try {
    (void)co_await apply(handler, Args{Value(e.message())});
  } catch (...) {
    again = to_lisp_error(std::current_exception());
  }
  if (again && again->message() != e.message()) {
    out("; error: " + again->message());
  }
}

Task<Result> Interp::top(std::vector<Value> forms) {
  Value r;
  std::optional<LispError> err;
  try {
    for (Value& f : forms) {
      r = co_await eval(std::move(f));
    }
  } catch (...) {
    err = to_lisp_error(std::current_exception());
  }
  if (!err) {
    co_return ok_result(std::move(r));
  }
  if (!err->handled) {
    co_await handle_error(*err);
  }
  co_return fail_result(err->message(), err->cancel || err->message() == kMsgQuit);
}

Task<Result> Interp::load_text(std::string text, std::string name) {
  std::vector<Value> forms;
  try {
    forms = detail::read_all(text);
  } catch (...) {
    const LispError e = to_lisp_error(std::current_exception());
    out("; error: " + e.message() + " (" + name + ")");
    co_return fail_result(e.message());
  }
  co_return co_await top(std::move(forms));
}

Task<Result> Interp::eval_text(std::string text) {
  std::vector<Value> forms;
  try {
    forms = detail::read_all(text);
  } catch (...) {
    const LispError e = to_lisp_error(std::current_exception());
    out("; error: " + e.message());
    co_return fail_result(e.message());
  }
  co_return co_await top(std::move(forms));
}

Task<Result> Interp::call_command(std::string name) {
  const std::string upper = base::js_to_upper(name);
  const Sym s = sym(upper.starts_with("C:") ? name : "C:" + name);
  const Value* f = globals_.find(s);
  if (f == nullptr || !callable(*f)) {
    co_return fail_result("no function definition: " + s->name);
  }
  co_return co_await top({list({Value(s)})});
}

std::vector<std::string> Interp::commands() const {
  std::vector<std::string> out;
  for (const auto& [s, v] : globals_.entries()) {
    if (s->name.starts_with("C:") && s->name.size() > 2 && (v.as<Lambda>() != nullptr || v.is_cons())) {
      out.push_back(s->name.substr(2));
    }
  }
  return out;
}

// ---------------------------------------------------------------- in-memory files
std::string Interp::norm(std::string_view path) {
  std::string p(path);
  std::ranges::replace(p, '\\', '/');
  return base::js_to_lower(p);
}
void Interp::put_file(std::string name, std::string text) {
  std::string key = norm(name);
  for (auto& [k, f] : files_) {
    if (k == key) {
      f = File{std::move(name), std::move(text)};
      return;
    }
  }
  files_.emplace_back(std::move(key), File{std::move(name), std::move(text)});
}
const Interp::File* Interp::find_file(std::string_view name) const {
  const std::string n = norm(name);
  for (const auto& [k, f] : files_) {
    if (k == n) {
      return &f;
    }
  }
  const auto base_of = [](std::string_view p) {
    const std::size_t s = p.rfind('/');
    return s == std::string_view::npos ? p : p.substr(s + 1);
  };
  const std::string_view want = base_of(n);
  for (const auto& [k, f] : files_) {
    if (base_of(k) == want) {
      return &f;
    }
  }
  return nullptr;
}
bool Interp::delete_file(std::string_view name) {
  const std::string n = norm(name);
  const auto it = std::ranges::find_if(files_, [&](const auto& e) { return e.first == n; });
  if (it == files_.end()) {
    return false;
  }
  files_.erase(it);
  return true;
}
std::vector<Interp::File> Interp::files() const {
  std::vector<File> out;
  out.reserve(files_.size());
  for (const auto& e : files_) {
    out.push_back(e.second);
  }
  return out;
}

// ---------------------------------------------------------------- run_now
namespace {

Task<void> drive(Task<Result> t, std::optional<Result>& into) {
  into = co_await std::move(t);
}

}  // namespace

Result run_now(Task<Result> t) {
  std::optional<Result> r;
  Task<void> d = drive(std::move(t), r);
  d.start();
  if (!d.done() || !r) {
    return fail_result(std::string(kMsgCancel), true);
  }
  if (d.error()) {
    return fail_result(to_lisp_error(d.error()).message());
  }
  return std::move(*r);
}

// ---------------------------------------------------------------- the special forms
namespace detail {

namespace {

Task<Value> run_body(std::vector<Value> body, Interp& L) {
  Value r;
  for (Value& f : body) {
    r = co_await L.eval(std::move(f));
  }
  co_return r;
}

Value nth_or_nil(const std::vector<Value>& v, std::size_t i) {
  return i < v.size() ? v[i] : Value();
}

Task<Value> sp_quote(Value l, Interp& /*L*/) {
  co_return l.is_cons() ? l.cons()->car : Value();
}

Task<Value> sp_function(Value l, Interp& L) {
  const Value x = l.is_cons() ? l.cons()->car : Value();
  if (x.is_cons() && x.cons()->car.is_sym() && x.cons()->car.sym() == k_lambda()) {
    co_return L.make_lambda({}, x.cons()->cdr);
  }
  if (x.is_sym()) {
    const Value v = L.get(x.sym());
    co_return v.nil() ? x : v;
  }
  co_return x;
}

Task<Value> sp_setq(Value l, Interp& L) {
  Value r;
  while (l.is_cons()) {
    const Value s = l.cons()->car;
    if (!s.is_sym()) {
      throw bad("symbolp", s);
    }
    if (!l.cons()->cdr.is_cons()) {
      throw LispError("too few arguments");
    }
    r = co_await L.eval(l.cons()->cdr.cons()->car);
    L.set(s.sym(), r);
    l = Value(l.cons()->cdr.cons()->cdr);
  }
  co_return r;
}

Task<Value> sp_defun(Value l, Interp& L) {
  const Value name = l.is_cons() ? l.cons()->car : Value();
  if (!name.is_sym()) {
    throw bad("symbolp", name);
  }
  L.set(name.sym(), L.make_lambda(name.sym()->name, l.cons()->cdr));
  const std::string& n = name.sym()->name;
  if (n.starts_with("C:") && n.size() > 2 && L.on_defun) {
    L.on_defun(n.substr(2));
  }
  co_return name;
}

Task<Value> sp_lambda(Value l, Interp& L) {
  co_return L.make_lambda({}, l);
}

Task<Value> sp_if(Value l, Interp& L) {
  const std::vector<Value> f = to_vector(l);
  if (f.size() < 2) {
    throw LispError("too few arguments");
  }
  if (f.size() > 3) {
    throw LispError("too many arguments");
  }
  if ((co_await L.eval(f[0])).truthy()) {
    co_return co_await L.eval(f[1]);
  }
  if (f.size() == 3) {  // no co_await inside ?: — GCC 14 evaluates it either way
    co_return co_await L.eval(f[2]);
  }
  co_return Value();
}

Task<Value> sp_cond(Value l, Interp& L) {
  for (const Value& clause : to_vector(l)) {
    if (!clause.is_cons()) {
      throw bad("consp", clause);
    }
    Value v = co_await L.eval(clause.cons()->car);
    if (v.truthy()) {
      if (clause.cons()->cdr.nil()) {
        co_return v;
      }
      co_return co_await run_body(to_vector(clause.cons()->cdr), L);
    }
  }
  co_return Value();
}

Task<Value> sp_while(Value l, Interp& L) {
  const std::vector<Value> f = to_vector(l);
  const Value test = nth_or_nil(f, 0);
  const std::vector<Value> body(f.size() > 1 ? f.begin() + 1 : f.end(), f.end());
  Value r;
  while ((co_await L.eval(test)).truthy()) {
    r = co_await run_body(body, L);
    L.step();
  }
  co_return r;
}

Task<Value> sp_repeat(Value l, Interp& L) {
  const std::vector<Value> f = to_vector(l);
  const Value c = co_await L.eval(nth_or_nil(f, 0));
  if (!c.is_int()) {
    throw bad("fixnump", c);
  }
  const std::vector<Value> body(f.size() > 1 ? f.begin() + 1 : f.end(), f.end());
  Value r;
  for (std::int32_t i = 0; i < c.int_v(); ++i) {
    r = co_await run_body(body, L);
    L.step();
  }
  co_return r;
}

Task<Value> sp_foreach(Value l, Interp& L) {
  const std::vector<Value> f = to_vector(l);
  const Value v = nth_or_nil(f, 0);
  if (!v.is_sym()) {
    throw bad("symbolp", v);
  }
  if (f.size() < 2) {
    throw bad("listp", Value());  // the JS evaluates `undefined`, which is not null
  }
  const Value items = co_await L.eval(f[1]);
  if (!items.is_list()) {
    throw bad("listp", items);
  }
  const std::vector<Value> body(f.size() > 2 ? f.begin() + 2 : f.end(), f.end());
  struct Restore {
    Interp& L;
    Sym s;
    std::optional<Value> old;
    Restore(Interp& in, Sym sy) : L(in), s(sy), old(in.saved(sy)) {}
    Restore(const Restore&) = delete;
    Restore& operator=(const Restore&) = delete;
    Restore(Restore&&) = delete;
    Restore& operator=(Restore&&) = delete;
    ~Restore() {
      try {
        L.restore(s, std::move(old));
      } catch (...) {  // NOLINT(bugprone-empty-catch) as in ~Bindings
      }
    }
  } const restore(L, v.sym());
  Value r;
  for (const Value* x = &items; x->is_cons(); x = &x->cons()->cdr) {
    L.set(v.sym(), x->cons()->car);
    r = co_await run_body(body, L);
  }
  co_return r;
}

Task<Value> sp_progn(Value l, Interp& L) {
  co_return co_await run_body(to_vector(l), L);
}

Task<Value> sp_and(Value l, Interp& L) {
  for (const Value& f : to_vector(l)) {
    if (!(co_await L.eval(f)).truthy()) {
      co_return Value();
    }
  }
  co_return Value(kT());
}

Task<Value> sp_or(Value l, Interp& L) {
  for (const Value& f : to_vector(l)) {
    if ((co_await L.eval(f)).truthy()) {
      co_return Value(kT());
    }
  }
  co_return Value();
}

}  // namespace

void install_specials(Interp& L) {
  L.def("QUOTE", &sp_quote);
  L.def("FUNCTION", &sp_function);
  L.def("SETQ", &sp_setq);
  L.def("DEFUN", &sp_defun);
  L.def("DEFUN-Q", &sp_defun);
  L.def("LAMBDA", &sp_lambda);
  L.def("IF", &sp_if);
  L.def("COND", &sp_cond);
  L.def("WHILE", &sp_while);
  L.def("REPEAT", &sp_repeat);
  L.def("FOREACH", &sp_foreach);
  L.def("PROGN", &sp_progn);
  L.def("AND", &sp_and);
  L.def("OR", &sp_or);
}

}  // namespace detail

}  // namespace cc::lisp
