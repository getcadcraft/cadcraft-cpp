// SPDX-License-Identifier: Apache-2.0
// cc::lisp — L4. CadCraft 0.5's LISP (lisp/types.js, reader.js, interp.js, builtins.js, vl.js, wild.js): the language
// AutoLISP routines are written in, evaluated as 0.5 evaluates them, so a routine prints, returns and fails with the
// same words. The drawing side (cadfns.js, host-cad.js: get* prompts, (command …), DXF lists) reaches the drawing
// through a Host; libs/cmd implements it over the document and the command engine.
//
//   Interp L({.out = …});            the language with every built-in installed (runtime.js createLisp)
//   co_await L.load_text(text, name) read and evaluate a file; reader errors are reported and nothing of it runs
//   co_await L.eval_text(text)       the command line's "(…" and "!var"
//   co_await L.call_command("NAME")  a (defun c:NAME …) routine, as typing its name runs it
//   run_now(task)                    runs a task that never waits on a host (no drawing): the tests, the CLI
//
// Values, as types.js: nil (the empty list and false), the symbol T, integers (32-bit, distinct from reals: (/ 7 2) is
// 3), reals, strings (UTF-8, lone UTF-16 surrogates as WTF-8; lengths and indexes count UTF-16 units as the JS does),
// symbols (interned, upper case), conses, and objects (entity names, selection sets, functions, files, VLA objects).
// Evaluation has dynamic scope: a function's arguments and its locals after "/" are bound while it runs and restored
// when it returns or fails. Errors reach the innermost *error* function bound where they happen, unless
// vl-catch-all-apply is catching; with none, "; error: <message>" is printed. The messages are AutoLISP's own words.
//
// Limits (0.5 has none but memory and V8's stack; docs/gaps.md): nesting in read text 4096, function calls nested
// kMaxDepth deep, structures printed or compared 200 / kMaxDepth deep — each refused with V8's own words, "Maximum call
// stack size exceeded"; and an optional step budget (Options::max_steps, off in the app as in 0.5, where Esc stops a
// routine) for scripts and fuzzing.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "cc/base/error.hpp"
#include "cc/base/task.hpp"
#include "cc/base/value.hpp"

namespace cc::i18n {
class Catalog;
}

namespace cc::lisp {

template <class T>
using Task = base::Task<T>;

class Interp;
class Object;
struct Cons;

/// An interned symbol (its name upper case, as String#toUpperCase gives it). Symbols live for the whole program, so
/// a Sym is a stable pointer: equal names, equal pointers.
struct Symbol {
  std::string name;
};
using Sym = const Symbol*;
/// The symbol with this name (case-insensitive).
[[nodiscard]] Sym sym(std::string_view name);

/// A LISP integer (32-bit, wrapping as AutoLISP's).
struct Int {
  std::int32_t v = 0;
};

using ConsRef = std::shared_ptr<Cons>;
using ObjRef = std::shared_ptr<Object>;
using StrRef = std::shared_ptr<const std::string>;

class Value {
 public:
  Value() noexcept = default;          // nil
  Value(std::nullptr_t) noexcept {}    // NOLINT(google-explicit-constructor)
  Value(Int i) noexcept : v_(i) {}     // NOLINT(google-explicit-constructor)
  Value(double d) noexcept : v_(d) {}  // NOLINT(google-explicit-constructor)
  Value(std::string s);                // NOLINT(google-explicit-constructor)
  Value(Sym s) noexcept : v_(s) {}     // NOLINT(google-explicit-constructor)
  Value(ConsRef c) noexcept;           // NOLINT(google-explicit-constructor)
  Value(ObjRef o) noexcept;            // NOLINT(google-explicit-constructor)
  static Value str_ref(StrRef s) noexcept;

  [[nodiscard]] bool nil() const noexcept { return std::holds_alternative<std::monostate>(v_); }
  [[nodiscard]] bool truthy() const noexcept { return !nil(); }
  [[nodiscard]] bool is_int() const noexcept { return std::holds_alternative<Int>(v_); }
  [[nodiscard]] bool is_real() const noexcept { return std::holds_alternative<double>(v_); }
  [[nodiscard]] bool is_num() const noexcept { return is_int() || is_real(); }
  [[nodiscard]] bool is_str() const noexcept { return std::holds_alternative<StrRef>(v_); }
  [[nodiscard]] bool is_sym() const noexcept { return std::holds_alternative<Sym>(v_); }
  [[nodiscard]] bool is_cons() const noexcept { return std::holds_alternative<ConsRef>(v_); }
  [[nodiscard]] bool is_list() const noexcept { return nil() || is_cons(); }
  [[nodiscard]] bool is_obj() const noexcept { return std::holds_alternative<ObjRef>(v_); }

  [[nodiscard]] std::int32_t int_v() const { return std::get<Int>(v_).v; }
  [[nodiscard]] double real() const { return std::get<double>(v_); }
  /// An integer's or a real's value (numVal).
  [[nodiscard]] double num() const { return is_int() ? static_cast<double>(int_v()) : real(); }
  [[nodiscard]] const std::string& str() const { return *std::get<StrRef>(v_); }
  [[nodiscard]] const StrRef& str_ref() const { return std::get<StrRef>(v_); }
  [[nodiscard]] Sym sym() const { return std::get<Sym>(v_); }
  [[nodiscard]] const ConsRef& cons() const { return std::get<ConsRef>(v_); }
  [[nodiscard]] const ObjRef& obj() const { return std::get<ObjRef>(v_); }
  /// The object as T, or null when it is not one.
  template <class T>
  [[nodiscard]] T* as() const noexcept;

  /// The same JS value (===): the same symbol, cons or object; equal strings; the same number (an integer and a real
  /// never are). Used by eq, the keys of association lists and the comparison of two runs.
  [[nodiscard]] bool same(const Value& o) const;

 private:
  std::variant<std::monostate, Int, double, StrRef, Sym, ConsRef, ObjRef> v_;
};

/// A cons cell. Lists are never changed after they are made (no rplaca in the language), so they are shared freely.
/// A long or deeply nested list is freed without recursion.
struct Cons {
  Value car, cdr;
  Cons(Value a, Value d) noexcept : car(std::move(a)), cdr(std::move(d)) {}
  Cons(const Cons&) = delete;
  Cons& operator=(const Cons&) = delete;
  Cons(Cons&&) = delete;
  Cons& operator=(Cons&&) = delete;
  ~Cons();
};
[[nodiscard]] Value cons(Value a, Value d);
[[nodiscard]] Value list(std::vector<Value> xs, Value tail = Value());
/// A list's elements (a dotted tail is dropped).
[[nodiscard]] std::vector<Value> to_vector(const Value& l);
[[nodiscard]] bool is_proper(const Value& l);

/// Everything else a value can be: what show() prints, what (type x) says, how eq / equal compare it.
class Object {
 public:
  Object() = default;
  Object(const Object&) = delete;
  Object& operator=(const Object&) = delete;
  Object(Object&&) = delete;
  Object& operator=(Object&&) = delete;
  virtual ~Object() = default;
  /// prin1 / princ text
  [[nodiscard]] virtual std::string show(bool readable) const = 0;
  /// (type x); empty = nil
  [[nodiscard]] virtual std::string type_name() const { return "SYM"; }
  /// eq / equal between two objects (default: the same object)
  [[nodiscard]] virtual bool same(const Object& o) const noexcept { return this == &o; }
  /// cadfns.js fromLisp: the object as host data (plain()), or null
  [[nodiscard]] virtual base::Value plain() const { return nullptr; }
};

template <class T>
T* Value::as() const noexcept {
  if (const auto* o = std::get_if<ObjRef>(&v_)) {
    return dynamic_cast<T*>(o->get());
  }
  return nullptr;
}

/// An entity name: a document id, or a text id ('b:<block>:<i>', 'm:…') for an object inside a block definition.
class Ename final : public Object {
 public:
  explicit Ename(base::Value id) : id_(std::move(id)) {}
  [[nodiscard]] const base::Value& id() const noexcept { return id_; }
  [[nodiscard]] std::string show(bool readable) const override;
  [[nodiscard]] std::string type_name() const override { return "ENAME"; }
  [[nodiscard]] bool same(const Object& o) const noexcept override;
  [[nodiscard]] base::Value plain() const override;

 private:
  base::Value id_;
};

/// A selection set; numbered in the order made (one count for the whole program, as the JS's PickSet.count).
class PickSet final : public Object {
 public:
  explicit PickSet(std::vector<base::Value> v);
  std::vector<base::Value> ids;
  [[nodiscard]] int number() const noexcept { return n_; }
  [[nodiscard]] std::string show(bool readable) const override;
  [[nodiscard]] std::string type_name() const override { return "PICKSET"; }
  [[nodiscard]] base::Value plain() const override;

 private:
  int n_;
};

/// A function made by defun or lambda.
class Lambda final : public Object {
 public:
  Lambda(std::string n, std::vector<Sym> p, std::vector<Sym> l, std::vector<Value> b)
      : name(std::move(n)), params(std::move(p)), locals(std::move(l)), body(std::move(b)) {}
  std::string name;  // empty for lambda
  std::vector<Sym> params, locals;
  std::vector<Value> body;
  Lambda(const Lambda&) = delete;
  Lambda& operator=(const Lambda&) = delete;
  Lambda(Lambda&&) = delete;
  Lambda& operator=(Lambda&&) = delete;
  ~Lambda() override;  // a body that holds functions that hold bodies… is freed without recursion
  [[nodiscard]] std::string show(bool readable) const override;
  [[nodiscard]] std::string type_name() const override { return "USUBR"; }
};

using Args = std::vector<Value>;
/// A built-in: synchronous, awaiting (it calls back into the interpreter or the host), or special (its arguments
/// unevaluated: the form's cdr). Plain function pointers, so a frame never outlives what it runs.
using SyncFn = Value (*)(Args& a, Interp& L);
using AsyncFn = Task<Value> (*)(Args a, Interp& L);
using SpecialFn = Task<Value> (*)(Value forms, Interp& L);
class Builtin final : public Object {
 public:
  Builtin(std::string n, SyncFn f) : name(std::move(n)), sync(f) {}
  Builtin(std::string n, AsyncFn f) : name(std::move(n)), async(f) {}
  Builtin(std::string n, SpecialFn f) : name(std::move(n)), special(f) {}
  std::string name;
  SyncFn sync = nullptr;
  AsyncFn async = nullptr;
  SpecialFn special = nullptr;
  [[nodiscard]] std::string show(bool readable) const override;
  [[nodiscard]] std::string type_name() const override { return "SUBR"; }
};

/// An open file (open / read-line / write-line / close) over the interpreter's in-memory files. Text as UTF-16 units,
/// as the JS string the file is.
class LFile final : public Object {
 public:
  LFile(std::string p, char m) : path(std::move(p)), mode(m) {}
  std::string path;
  char mode;
  std::size_t pos = 0;
  bool open = true;
  std::u16string buf;
  [[nodiscard]] std::string show(bool readable) const override;
  [[nodiscard]] std::string type_name() const override { return "FILE"; }
};

/// What vl-catch-all-apply returns when the call failed.
class CatchError final : public Object {
 public:
  explicit CatchError(std::string m) : message(std::move(m)) {}
  std::string message;
  [[nodiscard]] std::string show(bool /*readable*/) const override { return "#<%catch-all-apply-error%>"; }
  [[nodiscard]] std::string type_name() const override { return "VL-CATCH-ALL-APPLY-ERROR"; }
};

/// types.js realText: a real as AutoLISP prints it (C's %g with six digits and always a decimal point).
[[nodiscard]] std::string real_text(double x);
/// types.js show: prin1 (readable: strings quoted) or princ text.
[[nodiscard]] std::string show(const Value& x, bool readable = true);
/// types.js typeName: (type x)'s name, or empty for nil.
[[nodiscard]] std::string type_name(const Value& x);

/// An error in a LISP program, with AutoLISP's message. Thrown inside the interpreter only (architecture.md, Errors):
/// it never crosses this library's API, where results carry the message. Copying one cannot throw (its texts are
/// shared and never change), so it is safe to copy while unwinding.
class LispError : public std::exception {
 public:
  explicit LispError(std::string m) : message_(std::make_shared<const std::string>(std::move(m))) {}
  [[nodiscard]] const char* what() const noexcept override { return message_->c_str(); }
  [[nodiscard]] const std::string& message() const noexcept { return *message_; }
  bool cancel = false;                      // Esc, or a cancelled prompt
  bool incomplete = false;                  // the reader ran out of text (the command line reads another line)
  bool handled = false;                     // *error* has had it
  std::shared_ptr<const std::string> note;  // one line said before the error (a function 0.5 does not have yet)
  std::shared_ptr<const Value> value;       // vl-exit-with-value

 private:
  std::shared_ptr<const std::string> message_;
};
inline constexpr std::string_view kMsgCancel = "Function cancelled";
inline constexpr std::string_view kMsgQuit = "quit / exit abort";
inline constexpr std::string_view kMsgDeep = "Maximum call stack size exceeded";

/// reader.js: whether a command-line text is complete (the command line reads another line until it is), and a
/// file's bytes as text (UTF-8, else Windows-1252).
[[nodiscard]] bool balanced(std::string_view text);
[[nodiscard]] std::string decode_source(std::span<const std::byte> bytes);

/// wild.js wcmatch: AutoLISP's wildcard patterns (# @ . * ? ~ [..] [~..] ` ,). A pattern the JS RegExp refuses (a
/// [z-a] class) is an error with V8's words.
[[nodiscard]] base::Result<bool> wcmatch(std::string_view s, std::string_view pattern);

/// The drawing side, as cadfns.js calls it: a method name and plain data (host-cad.js shapes: reals are numbers,
/// integers {i}, points [x, y, z], entity names {e}); the answer is plain data too. A host refuses with a
/// LispError-free cc::base::Error message, or cancels (Esc).
class Host {
 public:
  Host() = default;
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;
  Host(Host&&) = delete;
  Host& operator=(Host&&) = delete;
  virtual ~Host() = default;
  struct Answer {
    base::Value value;
    std::optional<std::string> error;  // refused: the message
    bool cancel = false;               // Esc
  };
  [[nodiscard]] virtual bool has(std::string_view method) const = 0;
  virtual Task<Answer> call(std::string method, base::Array args) = 0;
};

/// V8's longest string (String::kMaxLength on 64-bit), in UTF-16 units: a longer one is "Invalid string length".
inline constexpr std::size_t kMaxStringUnits = 536870888;

struct Options {
  /// (text, piece): what princ, prompt and the error lines print. `piece` = a princ / prin1 / print / prompt piece
  /// (the command line joins them into lines), else a whole line.
  std::function<void(const std::string& text, bool piece)> out;
  Host* host = nullptr;
  /// The words of the notes the language adds (lisp.missing.known); null = the keys themselves.
  const i18n::Catalog* words = nullptr;
  /// Stop with "Function cancelled" after this many evaluation steps (0 = never, as 0.5). The list elements and
  /// string units a built-in makes count as steps too.
  std::uint64_t max_steps = 0;
  /// The longest string a routine may make, in UTF-16 units ("Invalid string length" past it): V8's by default; the
  /// fuzzer sets a small one, so a doubling loop cannot take the machine's memory.
  std::size_t max_string = kMaxStringUnits;
};

/// What a top-level evaluation gives.
struct Result {
  bool ok = true;
  Value value;
  std::string error;
  bool cancel = false;  // stopped by Esc, (exit) or (quit)
};

/// The ordered global table: a JS Map from symbols to values (insertion order kept; a key set again keeps its place,
/// a deleted key set again goes last).
class Globals {
 public:
  [[nodiscard]] const Value* find(Sym s) const;
  void set(Sym s, Value v);
  void erase(Sym s);
  [[nodiscard]] bool has(Sym s) const { return index_.contains(s); }
  /// Every entry in the Map's order.
  [[nodiscard]] std::vector<std::pair<Sym, Value>> entries() const;

 private:
  struct Slot {
    Value v;
    std::uint64_t order;
  };
  std::unordered_map<Sym, Slot> index_;
  std::uint64_t next_ = 0;
};

class Interp {
 public:
  explicit Interp(Options o);
  Interp(const Interp&) = delete;
  Interp& operator=(const Interp&) = delete;
  Interp(Interp&&) = delete;
  Interp& operator=(Interp&&) = delete;
  ~Interp();

  // ---- the API (interp.js)
  Task<Result> load_text(std::string text, std::string name = "untitled.lsp");
  Task<Result> eval_text(std::string text);
  Task<Result> call_command(std::string name);
  /// The routines' commands defined so far (C:NAME → NAME), in definition order.
  [[nodiscard]] std::vector<std::string> commands() const;
  /// Told when (defun c:NAME …) defines a command.
  std::function<void(const std::string& name)> on_defun;
  /// Asks the routine to stop: the next step fails with "Function cancelled".
  void stop() noexcept { stopped_ = true; }

  // ---- in-memory files (load, findfile, open, load_dialog)
  struct File {
    std::string name, text;
  };
  void put_file(std::string name, std::string text);
  [[nodiscard]] const File* find_file(std::string_view name) const;
  bool delete_file(std::string_view name);
  [[nodiscard]] std::vector<File> files() const;
  [[nodiscard]] static std::string norm(std::string_view path);

  // ---- for built-ins
  [[nodiscard]] Value get(Sym s) const;
  Value set(Sym s, Value v);
  void restore(Sym s, std::optional<Value> v);
  [[nodiscard]] std::optional<Value> saved(Sym s) const;
  [[nodiscard]] bool bound(Sym s) const { return globals_.has(s); }
  [[nodiscard]] std::vector<std::pair<Sym, Value>> global_entries() const { return globals_.entries(); }
  void def(std::string_view name, SyncFn f);
  void def(std::string_view name, AsyncFn f);
  void def(std::string_view name, SpecialFn f);
  void out(const std::string& text, bool piece = false) const;
  Task<Value> eval(Value x);
  Task<Value> apply(Value f, Args args, Value head = Value());
  /// fnOf: a function from a function, a symbol naming one, or a (lambda …) list.
  [[nodiscard]] Value fn_of(const Value& f);
  [[nodiscard]] Value make_lambda(std::string name, const Value& rest) const;
  /// A step of a loop (while, repeat): counts toward the stop check and the budget.
  void step();
  /// A built-in that makes n list elements or string units charges them as steps, so the budget bounds memory too.
  void charge(std::size_t n);
  Task<void> handle_error(LispError& e);
  /// "Invalid string length" when a string of `units` UTF-16 units would pass Options::max_string.
  void check_string(std::size_t units) const;
  [[nodiscard]] std::size_t max_string() const noexcept { return opt_.max_string; }
  [[nodiscard]] Host* host() const noexcept { return opt_.host; }
  int catching = 0;  // vl-catch-all-apply depth: errors are not handed to *error* while > 0
  /// A function made on first use (vla.js: vla-get-…, vla-put-…, vla-…), or null.
  std::function<Value(const std::string& name)> auto_fn;
  // what builtins keep (the JS keeps them in closures over one interpreter)
  std::map<std::string, std::string> env;  // getenv / setenv
  std::map<Sym, Value> bb;                 // vl-bb-set / vl-bb-ref
  struct InitGet {
    std::int32_t flags = 0;
    std::optional<std::string> kw;
  };
  std::optional<InitGet> initget;
  int depth = 0;  // nested evaluations and applications (kMaxDepth)
  static constexpr int kMaxDepth = 250000;
  /// A function name that names another name… (fnOf follows symbols): V8 overflows its stack, this stops here.
  static constexpr int kMaxIndirection = 10000;

 private:
  Options opt_;
  Globals globals_;
  std::vector<std::pair<std::string, File>> files_;  // normalised path → file, in the Map's order
  std::uint64_t steps_ = 0;
  bool stopped_ = false;
  bool in_error_ = false;
  std::set<std::string> missing_said_;
  Value auto_fn_of(Sym s);
  [[nodiscard]] LispError no_function(Sym s);
  void tick() const;
  Task<Result> top(std::vector<Value> forms);
};

/// Runs a task that never waits on a host to its end (no drawing: the tests, the CLI's pure LISP). A task that does
/// wait is an error (the evaluation is abandoned and reported as cancelled).
[[nodiscard]] Result run_now(Task<Result> t);

/// Process-wide counters the JS keeps in module scope: selection set numbers, vl-filename-mktemp's. The parity
/// runner starts each session with fresh JS modules, so the CLI resets them per session too.
[[nodiscard]] int next_pickset_number() noexcept;
[[nodiscard]] int next_temp_number() noexcept;
void reset_counters() noexcept;

/// cadfns.js toLisp: host data → LISP (null / false → nil, true → T, numbers → reals, strings, arrays → lists,
/// {i} → an integer, {e} → an entity name, {ids} → a new selection set, {sym} → a symbol).
[[nodiscard]] Value to_lisp(const base::Value& v);

}  // namespace cc::lisp
