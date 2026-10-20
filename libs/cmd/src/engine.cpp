// SPDX-License-Identifier: Apache-2.0
// commands.js Engine (CadCraft 0.5) in C++: input → the waiting prompt, typed points and lengths, the prompt line,
// running a command as one undo step. The JS resolves a prompt's promise and the command goes on in a microtask; here
// the waiting coroutine is resumed at once — the same order of prints, prompts and edits.
#include "cc/cmd/engine.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>
#include <utility>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "commands.hpp"

namespace cc::cmd {

using base::Value;

namespace {

constexpr double kTau = std::numbers::pi * 2;

double rad(double d) {
  return d * std::numbers::pi / 180;
}
double deg(double r) {
  return r * 180 / std::numbers::pi;
}
double norm_angle(double a) {
  a = std::fmod(a, kTau);
  return a < 0 ? a + kTau : a;
}
double dist(Pt a, Pt b) {
  return base::js_hypot(b.x - a.x, b.y - a.y);
}
double angle_of(Pt a, Pt b) {
  return base::js_atan2(b.y - a.y, b.x - a.x);
}
Pt polar(Pt p, double ang, double d) {
  return {p.x + base::js_cos(ang) * d, p.y + base::js_sin(ang) * d};
}

/// fmt(n): '0' under 1e-10, else Number(n.toFixed(4)).toString()
std::string fmt(double n) {
  if (std::abs(n) < 1e-10) {
    return "0";
  }
  std::string s;
  base::append_number(s, base::js_number(base::js_to_fixed(n, 4)));
  return s;
}
std::string count_found(std::size_t n) {
  return std::to_string(n) + " found";
}
bool is_digit(char c) {
  return c >= '0' && c <= '9';
}
bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
/// /^[-+]?(\d+\.?\d*|\.\d+)(e[-+]?\d+)?$/i (exponent optional by `exp`)
bool plain_number(std::string_view s, bool exp) {
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
    ++i;
  }
  const std::size_t d0 = i;
  while (i < s.size() && is_digit(s[i])) {
    ++i;
  }
  if (i > d0) {
    if (i < s.size() && s[i] == '.') {
      ++i;
      while (i < s.size() && is_digit(s[i])) {
        ++i;
      }
    }
  } else {
    if (i >= s.size() || s[i] != '.') {
      return false;
    }
    ++i;
    const std::size_t f0 = i;
    while (i < s.size() && is_digit(s[i])) {
      ++i;
    }
    if (i == f0) {
      return false;
    }
  }
  if (exp && i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
      ++i;
    }
    const std::size_t e0 = i;
    while (i < s.size() && is_digit(s[i])) {
      ++i;
    }
    if (i == e0) {
      return false;
    }
  }
  return i == s.size();
}
/// units.js mixed(t): "6", "6.5", "6-1/2", "6 1/2", "1/2" → a number
std::optional<double> mixed(std::string_view t) {
  // NUM = /^(?:\d+(?:\.\d*)?|\.\d+)(?:e[-+]?\d+)?$/i (no sign)
  if (!t.empty() && t[0] != '-' && t[0] != '+' && plain_number(t, true)) {
    return base::js_number(t);
  }
  const auto digits = [](std::string_view s) { return !s.empty() && std::ranges::all_of(s, is_digit); };
  if (const std::size_t slash = t.find('/'); slash != std::string_view::npos) {
    const std::string_view den = t.substr(slash + 1);
    const std::string_view head = t.substr(0, slash);
    if (const std::size_t sep = head.find_first_of("- "); sep != std::string_view::npos) {
      const std::string_view w = head.substr(0, sep);
      const std::string_view n = head.substr(sep + 1);
      if (digits(w) && digits(n) && digits(den) && base::js_number(den) > 0) {
        return base::js_number(w) + base::js_number(n) / base::js_number(den);
      }
      return std::nullopt;
    }
    if (digits(head) && digits(den) && base::js_number(den) > 0) {
      return base::js_number(head) / base::js_number(den);
    }
  }
  return std::nullopt;
}
/// units.js lunitCode(v)
int lunit_code(const Value& v) {
  const double n = base::js_to_number(v);
  const int i = base::js_to_int32(n);
  return i >= 1 && i <= 5 && n == static_cast<double>(i) ? i : 2;
}

struct Ucs {
  double x = 0, y = 0, ang = 0;
};
/// ucs.js activeUcs(doc): the UCS in model space, else none
std::optional<Ucs> active_ucs(const doc::Doc& d) {
  if (!d.in_model() || !d.ucs.truthy()) {
    return std::nullopt;
  }
  return Ucs{.x = base::js_to_number(d.ucs.get("x")),
             .y = base::js_to_number(d.ucs.get("y")),
             .ang = base::js_to_number(d.ucs.get("ang"))};
}
Pt vec_to_world(const std::optional<Ucs>& u, Pt v) {
  if (!u) {
    return v;
  }
  const double c = base::js_cos(u->ang);
  const double s = base::js_sin(u->ang);
  return {v.x * c - v.y * s, v.x * s + v.y * c};
}
Pt to_world(const std::optional<Ucs>& u, Pt p) {
  if (!u) {
    return p;
  }
  const Pt w = vec_to_world(u, p);
  return {u->x + w.x, u->y + w.y};
}

const Option* match_option(std::string_view typed, const std::vector<Option>& options) {
  const std::string u = base::js_to_upper(typed);
  for (const Option& o : options) {
    if (base::js_to_upper(o.key) == u) {
      return &o;
    }
  }
  const auto starts = [&](const std::string& s) { return base::js_to_upper(s).starts_with(u); };
  for (const Option& o : options) {
    if (!u.empty() && base::utf16_length(u) >= base::utf16_length(o.key) && (starts(o.label) || starts(o.en))) {
      return &o;
    }
  }
  return nullptr;
}

Answer from_value(const Value& v) {
  Answer a;
  if (v.is_number()) {
    a.kind = Answer::Kind::number;
    a.number = v.as_number();
  } else if (v.is_string()) {
    a.kind = Answer::Kind::text;
    a.text = v.as_string();
  } else if (!v.is_undefined() && !v.is_null()) {
    a.kind = Answer::Kind::typed;
    a.typed = v;
  }
  return a;
}

std::string_view invalid_key(std::string_view kind) {
  if (kind == "point") {
    return "cmd.invalid.point";
  }
  if (kind == "distance") {
    return "cmd.invalid.distance";
  }
  if (kind == "number") {
    return "cmd.invalid.number";
  }
  if (kind == "angle") {
    return "cmd.invalid.angle";
  }
  if (kind == "keyword") {
    return "cmd.invalid.keyword";
  }
  if (kind == "select") {
    return "cmd.invalid.select";
  }
  if (kind == "entity") {
    return "cmd.invalid.entity";
  }
  return "cmd.invalid_input";
}

}  // namespace

// ---------------------------------------------------------------- IdSet, Answer

bool IdSet::has(Id id) const {
  return std::ranges::find(ids_, id) != ids_.end();
}
bool IdSet::add(Id id) {
  if (has(id)) {
    return false;
  }
  ids_.push_back(id);
  return true;
}
bool IdSet::erase(Id id) {
  const auto it = std::ranges::find(ids_, id);
  if (it == ids_.end()) {
    return false;
  }
  ids_.erase(it);
  return true;
}

bool Answer::truthy() const noexcept {
  switch (kind) {
    case Kind::nothing: return false;
    case Kind::number: return number != 0 && !std::isnan(number);
    case Kind::text: return !text.empty();
    case Kind::typed: return typed.truthy();
    default: return true;
  }
}

std::optional<double> parse_length(std::string_view raw, const Value& lunits) {
  std::string_view s = base::js_trim(raw);
  if (s.empty()) {
    return std::nullopt;
  }
  double sign = 1;
  if (s[0] == '-' || s[0] == '+') {
    if (s[0] == '-') {
      sign = -1;
    }
    s = base::js_trim(s.substr(1));
  }
  if (s.empty()) {
    return std::nullopt;
  }
  if (s.find_first_of("'\"") != std::string_view::npos) {
    const int code = lunit_code(lunits);
    if (code != 3 && code != 4) {
      return std::nullopt;
    }
    double ft = 0;
    std::string_view rest = s;
    if (const std::size_t q = s.find('\''); q != std::string_view::npos) {
      const auto f = mixed(base::js_trim(s.substr(0, q)));
      if (!f) {
        return std::nullopt;
      }
      ft = *f;
      rest = base::js_trim(s.substr(q + 1));
      if (rest.starts_with('-')) {
        rest.remove_prefix(1);
      }
    }
    if (rest.ends_with('"')) {
      rest.remove_suffix(1);
    }
    rest = base::js_trim(rest);
    if (rest.find_first_of("'\"") != std::string_view::npos) {
      return std::nullopt;
    }
    double inch = 0;
    if (!rest.empty()) {
      const auto i = mixed(rest);
      if (!i) {
        return std::nullopt;
      }
      inch = *i;
    }
    return sign * (ft * 12 + inch);
  }
  const auto v = mixed(s);
  if (!v) {
    return std::nullopt;
  }
  return sign * *v;
}

// ---------------------------------------------------------------- Ctx

Option Ctx::opt(std::string key, const std::string& label) const {
  std::string k = "opt.";
  bool sep = false;
  for (const char ch : base::js_to_lower(label)) {
    if ((ch >= 'a' && ch <= 'z') || is_digit(ch)) {
      k += ch;
      sep = false;
    } else if (!sep) {
      sep = true;  // a run of others is dropped (replace(/[^a-z0-9]+/g, ''))
    }
  }
  const std::string shown = k != "opt." && words.has(k) ? words.t(k) : label;
  return {.key = std::move(key),
          .label = base::js_to_lower(shown) == base::js_to_lower(label) ? label : shown,
          .en = label,
          .hidden = false};
}

void Ctx::preview(std::function<base::Array(Pt)> fn) {
  engine.set_preview(std::move(fn));
}

AskAwait Ctx::point(std::string prompt, Ask o) {
  return {engine, "point", std::move(prompt), std::move(o)};
}
AskAwait Ctx::distance(std::string prompt, Ask o) {
  return {engine, "distance", std::move(prompt), std::move(o)};
}
AskAwait Ctx::angle(std::string prompt, Ask o) {
  return {engine, "angle", std::move(prompt), std::move(o)};
}
AskAwait Ctx::files(bool multiple) {
  return {engine, multiple ? "files+" : "files", std::string(), Ask{}};
}
AskAwait Ctx::number(std::string prompt, Ask o) {
  return {engine, "number", std::move(prompt), std::move(o)};
}
AskAwait Ctx::string(std::string prompt, Ask o) {
  return {engine, "string", std::move(prompt), std::move(o)};
}
AskAwait Ctx::keyword(std::string prompt, Ask o) {
  return {engine, "keyword", std::move(prompt), std::move(o)};
}
AskAwait Ctx::entity(std::string prompt, Ask o) {
  return {engine, "entity", std::move(prompt), std::move(o)};
}

Value Ctx::add(const Value& e) {
  Value made;
  doc.change("add", [&] { made = doc.add(e); });
  return made;
}

Value Ctx::ent(const Value& e) const {
  Value o = base::make_object({{"layer", doc.current.get("layer")}, {"color", doc.current.get("color")}});
  // doc.currentStyle(): { lt?, lw? }
  if (const Value lt = doc.current.get("linetype"); lt.truthy()) {
    o.set("lt", lt);
  }
  if (const Value lw = doc.current.get("lineweight"); lw.is_number() && std::isfinite(lw.as_number())) {
    o.set("lw", lw);
  }
  if (e.is_object() && !e.is_point()) {
    for (const base::Member* m : base::js_members(e.as_object())) {
      o.set(m->key, m->value);
    }
  }
  return o;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the JS's c.select, in order
Task<Answer> Ctx::select(std::string prompt, Ask o) {
  if (prompt.empty()) {
    prompt = t("cmd.sim.select");
  }
  IdSet pre;
  for (const Id id : host.selection()) {
    const Value* e = doc.entities().get(id);
    if (e != nullptr && doc.editable(*e)) {
      pre.add(id);
    }
  }
  if (!pre.empty() && !o.fresh) {
    print(count_found(pre.size()));
    engine.prev_selection = pre;
    Answer a;
    a.kind = Answer::Kind::set;
    a.set = std::move(pre);
    co_return a;
  }
  host.set_selection({});
  IdSet set;
  bool remove = false;
  for (;;) {
    Ask ask = o;
    ask.none = true;
    ask.remove = remove;
    for (const auto& [key, k] :
         std::initializer_list<std::pair<std::string_view, std::string_view>>{{"P", "sel.opt.previous"},
                                                                              {"F", "sel.opt.fence"},
                                                                              {"W", "sel.opt.window"},
                                                                              {"C", "sel.opt.crossing"},
                                                                              {"R", "sel.opt.remove"},
                                                                              {"A", "sel.opt.add"}}) {
      ask.options.push_back({.key = std::string(key), .label = t(k), .en = {}, .hidden = true});
    }
    ask.options.push_back({.key = "?", .label = "?", .en = {}, .hidden = true});
    engine.select_set_ = &set;
    Answer r = co_await AskAwait(engine, "select", remove ? t("sel.remove.prompt") : prompt, std::move(ask));
    engine.select_set_ = nullptr;
    if (r.kind == Answer::Kind::option) {
      if (r.option == "R" || r.option == "A") {
        remove = r.option == "R";
        continue;
      }
      if (co_await engine.select_option(r.option, set, remove)) {
        continue;
      }
      co_return r;  // the command's own option
    }
    break;
  }
  if (!set.empty()) {
    engine.prev_selection = set;
  }
  Answer a;
  a.kind = Answer::Kind::set;
  a.set = std::move(set);
  co_return a;
}

void AskAwait::await_suspend(std::coroutine_handle<> h) {
  e_.wait(std::move(kind_), std::move(prompt_), std::move(opts_), h, &out_);
}

// ---------------------------------------------------------------- Engine

Engine::Engine(doc::Doc& d, Host& h, const i18n::Catalog& words)
    : doc_(d), host_(h), words_(words), c_(*this, d, h, words) {
  if (std::ranges::none_of(d.edit_hooks, [](const doc::Doc::EditHook& x) { return x.tag == "viewlabel"; })) {
    const i18n::Catalog* w = &words;
    d.edit_hooks.push_back({.tag = "viewlabel", .fn = [w](doc::Doc& doc, const std::vector<doc::Op>& ops) {
                              detail::view_label_hook(doc, ops, *w);
                            }});
  }
}

Engine::~Engine() {
  std::erase_if(doc_.edit_hooks, [](const doc::Doc::EditHook& x) { return x.tag == "viewlabel"; });
  waiting_.reset();
  root_.reset();
}

void Engine::wait(std::string kind, std::string prompt, Ask opts, std::coroutine_handle<> h, Answer* out) {
  Waiting w{.kind = std::move(kind),
            .prompt = std::move(prompt),
            .opts = std::move(opts),
            .set = nullptr,
            .h = h,
            .out = out};
  if (w.kind == "select") {
    w.set = select_set_;
  }
  rubber_base = w.opts.base;
  const bool files = w.kind == "files" || w.kind == "files+";
  const bool multiple = w.kind == "files+";
  waiting_ = std::move(w);
  if (files) {  // the picker, not the command line, has the user's attention
    host_.pick_pictures(multiple);
    return;
  }
  host_.prompt(prompt_text());
}

bool Engine::waiting_for_files() const noexcept {
  return waiting_ && (waiting_->kind == "files" || waiting_->kind == "files+");
}

void Engine::give_files(std::vector<PickedFile> files) {
  if (!waiting_for_files()) {
    return;
  }
  Answer a;
  a.kind = Answer::Kind::files;
  a.files = std::move(files);
  resolve(std::move(a));
}

void Engine::set_preview(std::function<base::Array(Pt)> fn) {
  preview_fn_ = std::move(fn);
  const auto cur = host_.cursor();
  host_.preview(preview_fn_ && cur ? preview(*cur) : base::Array{});
}

base::Array Engine::preview(Pt cursor) const {
  if (!preview_fn_) {
    return {};
  }
  try {
    return preview_fn_(cursor);
  } catch (const std::exception&) {  // the JS's try { … } catch { return [] }
    return {};
  }
}

const CommandDef* Engine::lookup(std::string_view name) {
  std::string raw = base::js_to_upper(base::js_trim(name));
  const auto& table = detail::command_table();
  const auto find = [&](std::string_view n) -> const CommandDef* {
    for (const CommandDef& c : table) {
      if (c.name == n) {
        return &c;
      }
    }
    return nullptr;
  };
  if (!raw.empty() && (raw[0] == '-' || raw[0] == '_')) {
    if (const CommandDef* c = find(raw)) {  // the command-line form (-PARAMETERS), a hidden _COMMAND
      return c;
    }
  }
  std::size_t i = 0;
  while (i < raw.size() && (raw[i] == '_' || raw[i] == '.' || raw[i] == '\'' || raw[i] == '-')) {
    ++i;
  }
  const std::string u = raw.substr(i);
  if (const CommandDef* c = find(u)) {
    return c;
  }
  // ALIASES: a later definition's alias replaces an earlier one's
  for (const CommandDef& c : std::views::reverse(table)) {
    if (std::ranges::find(c.aliases, u) != c.aliases.end()) {
      return &c;
    }
  }
  return nullptr;
}

std::vector<const CommandDef*> Engine::commands() {
  std::vector<const CommandDef*> out;
  for (const CommandDef& c : detail::command_table()) {
    if (!c.hidden) {
      out.push_back(&c);
    }
  }
  return out;
}

void Engine::run(std::string_view name) {
  const CommandDef* cmd = lookup(name);
  if (cmd == nullptr) {
    host_.print(words_.t("cmd.unknown", {{"name", base::js_to_upper(name)}}));
    return;
  }
  if (root_) {
    escape();
  }
  if (!cmd->no_repeat) {
    last_ = cmd->name;
  }
  running_ = cmd;
  host_.on_command(&cmd->name);
  host_.print(words_.t("cmd.echo", {{"name", cmd->name}}));
  cmd_mark = doc_.mark();
  root_.emplace(cmd->fn(c_));
  root_->start();
  after_step();
}

void Engine::after_step() {
  if (root_ && root_->done() && !waiting_) {
    finish(false);
  }
}

void Engine::finish(bool cancelled) {
  const CommandDef* cmd = running_;
  const std::exception_ptr err = root_ ? root_->error() : nullptr;
  waiting_.reset();
  select_set_ = nullptr;
  root_.reset();  // a cancelled command's frames go here (their destructors are its `finally` blocks)
  if (cancelled) {
    host_.print("*Cancel*");
  } else if (err) {
    try {
      std::rethrow_exception(err);
    } catch (const std::exception& e) {
      host_.print(std::string("Error: ") + e.what());
    } catch (...) {
      host_.print("Error: ");
    }
  }
  if (cmd != nullptr) {
    doc_.squash(cmd_mark, cmd->name);
  }
  running_ = nullptr;
  rubber_base.reset();
  preview_fn_ = nullptr;
  if (cmd == nullptr || !cmd->keep_selection) {
    host_.set_selection({});
  }
  host_.preview({});
  host_.prompt("");
  host_.on_command(nullptr);
}

void Engine::resolve(Answer v) {
  if (!waiting_) {
    return;
  }
  Waiting w = std::move(*waiting_);
  waiting_.reset();
  if (v.kind == Answer::Kind::point && w.kind == "point") {
    last_point = v.p;
  }
  *w.out = std::move(v);
  resume(w.h);
}

void Engine::resume(std::coroutine_handle<> h) {
  h.resume();
  after_step();
}

bool Engine::escape() {
  if (waiting_) {
    finish(true);
    return true;
  }
  return false;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the JS's text(), in order
void Engine::text(std::string_view raw) {
  const std::string s(raw);
  const std::string typed(base::js_trim(s));
  if (!waiting_) {
    if (typed.empty()) {
      if (!last_.empty()) {
        run(last_);
      }
      return;
    }
    run(typed);
    return;
  }
  if (waiting_for_files()) {  // the picker is open: typing waits for it
    return;
  }
  Waiting& w = *waiting_;
  const std::string kind = w.kind;
  if (kind == "string") {
    Answer a;
    a.kind = Answer::Kind::text;
    a.text = s;
    resolve(std::move(a));
    return;
  }
  if (kind == "select" && typed.empty()) {
    Answer a;
    a.kind = Answer::Kind::set;
    a.set = w.set != nullptr ? *w.set : IdSet{};
    resolve(std::move(a));
    return;
  }
  // Enter where a point or an object is required (and there is no default) ends the command
  if (typed.empty() && (kind == "point" || kind == "entity") && !w.opts.none.value_or(false) &&
      w.opts.def.is_undefined()) {
    escape();
    return;
  }
  if (typed.empty()) {
    resolve(w.opts.none == std::optional<bool>(false) ? Answer{} : from_value(w.opts.def));
    return;
  }
  if (const Option* o = match_option(typed, w.opts.options)) {
    Answer a;
    a.kind = Answer::Kind::option;
    a.option = o->key;
    resolve(std::move(a));
    return;
  }
  if (w.opts.accept) {
    if (Value v = w.opts.accept(typed); !v.is_undefined() && !v.is_null()) {  // a command's own token (ZOOM 2x)
      Answer a;
      a.kind = Answer::Kind::option;
      a.option = "_typed";
      a.typed = std::move(v);
      resolve(std::move(a));
      return;
    }
  }
  if (kind == "point") {
    if (const auto p = parse_point(typed)) {
      Answer a;
      a.kind = Answer::Kind::point;
      a.p = *p;
      resolve(std::move(a));
      return;
    }
  } else if (kind == "distance") {
    if (const auto n = parse_length(typed, doc_.meta.get("lunits")); n && *n >= 0) {
      Answer a;
      a.kind = Answer::Kind::number;
      a.number = *n;
      resolve(std::move(a));
      return;
    }
  } else if (kind == "number") {
    if (w.opts.length) {
      if (const auto n = parse_length(typed, doc_.meta.get("lunits"))) {
        Answer a;
        a.kind = Answer::Kind::number;
        a.number = *n;
        resolve(std::move(a));
        return;
      }
    } else if (plain_number(typed, true)) {
      Answer a;
      a.kind = Answer::Kind::number;
      a.number = base::js_parse_float(typed);
      resolve(std::move(a));
      return;
    }
  } else if (kind == "angle") {
    if (plain_number(typed, false)) {
      // an absolute angle: from the UCS X axis (DR.8)
      const auto u = w.opts.ucs ? active_ucs(doc_) : std::nullopt;
      Answer a;
      a.kind = Answer::Kind::number;
      a.number = rad(base::js_parse_float(typed)) + (u ? u->ang : 0);
      resolve(std::move(a));
      return;
    }
  } else if (kind == "select") {
    const std::string u = base::js_to_upper(typed);
    if (u == "ALL") {
      doc_.entities().each([&](const Id&, const Value& e) {
        if (doc_.editable(e) && w.set != nullptr) {
          w.set->add(static_cast<Id>(base::js_to_number(e.get("id"))));
        }
      });
      if (w.set != nullptr) {
        host_.set_selection(*w.set);
        host_.print(count_found(w.set->size()));
      }
      return;
    }
    if (u == "L" || u == "LAST") {
      std::optional<Id> last;
      doc_.entities().each([&](const Id& id, const Value&) { last = id; });
      if (last && *last != 0 && w.set != nullptr) {
        w.set->add(*last);
        host_.set_selection(*w.set);
        host_.print("1 found");
      }
      return;
    }
  }
  host_.print(words_.t(invalid_key(kind)));
  host_.prompt(prompt_text());
}

bool Engine::click(Pt point, std::optional<Pt> raw_in) {
  if (!waiting_ || waiting_for_files()) {
    return false;
  }
  const Pt raw = raw_in.value_or(point);
  Waiting& w = *waiting_;
  if (w.kind == "point") {
    Answer a;
    a.kind = Answer::Kind::point;
    a.p = point;
    resolve(std::move(a));
    return true;
  }
  if (w.kind == "distance" || w.kind == "angle") {
    if (w.opts.base) {
      Answer a;
      a.kind = Answer::Kind::number;
      a.number = w.kind == "distance" ? dist(*w.opts.base, point) : angle_of(*w.opts.base, point);
      resolve(std::move(a));
    } else {
      w.opts.base = point;
      rubber_base = point;
      host_.prompt(words_.t("cmd.specify_second_point"));
    }
    return true;
  }
  if (w.kind == "select") {
    const auto id = host_.pick_at(raw);
    if (id && *id != 0 && w.set != nullptr) {
      if (w.opts.remove || (w.set->has(*id) && host_.shift())) {
        w.set->erase(*id);
      } else {
        w.set->add(*id);
      }
      host_.set_selection(*w.set);
      if (w.opts.single) {
        Answer a;
        a.kind = Answer::Kind::set;
        a.set = *w.set;
        resolve(std::move(a));
      }
      return true;
    }
    return false;  // nothing there: the host starts a selection window
  }
  if (w.kind == "entity") {
    const auto id = host_.pick_at(raw);
    if (id && *id != 0) {
      Answer a;
      a.kind = Answer::Kind::entity;
      a.id = *id;
      a.p = raw;
      a.shift = host_.shift();
      resolve(std::move(a));
    } else if (w.opts.empty) {  // TRIM: a fence from here
      Answer a;
      a.kind = Answer::Kind::empty;
      a.p = raw;
      a.shift = host_.shift();
      resolve(std::move(a));
    } else {
      host_.print(words_.t("cmd.nothing_found_there"));
    }
    return true;
  }
  return false;
}

void Engine::window(const Rect& r, bool crossing) {
  if (!waiting_ || waiting_->kind != "select" || waiting_->set == nullptr) {
    return;
  }
  Waiting& w = *waiting_;
  const std::vector<Id> ids = host_.pick_window(r, crossing);
  for (const Id id : ids) {
    if (host_.shift() || w.opts.remove) {
      w.set->erase(id);
    } else {
      w.set->add(id);
    }
  }
  if (!host_.shift() && !w.opts.remove) {
    w.set->windows.push_back(r);  // STRETCH stretches what these windows hold
  }
  host_.set_selection(*w.set);
  host_.print(w.opts.remove ? words_.tn("sel.removed", static_cast<double>(ids.size())) : count_found(ids.size()));
}

std::string Engine::prompt_text() const {
  if (!waiting_) {
    return {};
  }
  const Waiting& w = *waiting_;
  std::string opts;
  for (const Option& o : w.opts.options) {
    if (o.hidden) {
      continue;
    }
    if (!opts.empty()) {
      opts += '/';
    }
    // a translated label may not start with the letters to type: then they follow it, 'Deshacer (U)'
    opts += base::js_to_upper(o.label).starts_with(base::js_to_upper(o.key)) ? o.label : o.label + " (" + o.key + ")";
  }
  const Value& d = w.opts.def;
  const Option* d_opt = nullptr;
  if (d.is_string()) {  // a default given as an English label shows translated
    for (const Option& o : w.opts.options) {
      if (o.en == d.as_string() && o.en != o.key) {
        d_opt = &o;
        break;
      }
    }
  }
  std::string def;
  if (!d.is_undefined() && !d.is_null() && (w.kind != "point" || d_opt != nullptr)) {
    std::string v;
    if (w.kind == "angle") {
      const auto u = w.opts.ucs ? active_ucs(doc_) : std::nullopt;
      v = fmt(deg(norm_angle(base::js_to_number(d) - (u ? u->ang : 0))));
    } else if (d.is_number()) {
      v = fmt(d.as_number());
    } else if (d_opt != nullptr) {
      v = d_opt->label;
    } else {
      v = base::js_string(d);
    }
    def = " <" + v + ">";
  }
  return w.prompt + (opts.empty() ? std::string() : " [" + opts + "]") + def + ":";
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the JS's parsePoint
std::optional<Pt> Engine::parse_point(std::string_view typed) const {
  // s = t.trim().replace(/\s*([,<@#*])\s*/g, '$1')
  const std::string_view tt = base::js_trim(typed);
  std::string s;
  for (std::size_t i = 0; i < tt.size(); ++i) {
    const char ch = tt[i];
    if (ch == ',' || ch == '<' || ch == '@' || ch == '#' || ch == '*') {
      while (!s.empty() && is_space(s.back())) {
        s.pop_back();
      }
      s += ch;
      while (i + 1 < tt.size() && is_space(tt[i + 1])) {
        ++i;
      }
    } else {
      s += ch;
    }
  }
  // in the UCS (DR.8); a * after @ / # or first = world coordinates
  bool world = false;
  if (s.starts_with('*')) {
    world = true;
    s.erase(0, 1);
  } else if (s.size() >= 2 && (s[0] == '@' || s[0] == '#') && s[1] == '*') {
    world = true;
    s.erase(1, 1);
  }
  const auto u = world ? std::nullopt : active_ucs(doc_);
  const Value lunits = doc_.meta.get("lunits");
  const auto L = [&](std::string_view x) { return parse_length(x, lunits); };
  const auto part_ok = [](std::string_view p) {
    return !p.empty() && p.find_first_of(",<@#*") == std::string_view::npos;
  };
  std::string_view body = s;
  char prefix = 0;
  if (!body.empty() && (body[0] == '@' || body[0] == '#')) {
    prefix = body[0];
    body.remove_prefix(1);
  }
  if (const std::size_t comma = body.find(','); comma != std::string_view::npos) {
    const std::string_view a = body.substr(0, comma);
    const std::string_view b = body.substr(comma + 1);
    if (part_ok(a) && part_ok(b)) {
      const auto x = L(a);
      const auto y = L(b);
      if (!x || !y) {
        return std::nullopt;
      }
      if (prefix == '@') {
        const Pt base = last_point.value_or(Pt{});
        const Pt d = vec_to_world(u, {*x, *y});
        return Pt{base.x + d.x, base.y + d.y};
      }
      return to_world(u, {*x, *y});
    }
  }
  if (prefix != '#') {
    if (const std::size_t lt = body.find('<'); lt != std::string_view::npos) {
      const std::string_view a = body.substr(0, lt);
      const std::string_view b = body.substr(lt + 1);
      if (part_ok(a) && part_ok(b)) {
        const auto d = L(a);
        const bool ang_ok = plain_number(b, false);
        if (!d || !ang_ok) {
          return std::nullopt;
        }
        const double ang = base::js_number(b);
        const Pt from = prefix == '@' ? last_point.value_or(Pt{}) : to_world(u, {0, 0});
        return polar(from, rad(ang) + (u ? u->ang : 0), *d);
      }
    }
  }
  if (const auto d = L(s)) {
    const std::optional<Pt> base = waiting_ && waiting_->opts.base ? waiting_->opts.base : rubber_base;
    const auto cur = host_.cursor();
    if (!base || !cur || dist(*base, *cur) < 1e-12) {
      return std::nullopt;
    }
    return polar(*base, angle_of(*base, *cur), *d);
  }
  return std::nullopt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the JS's selectOption
Task<bool> Engine::select_option(const std::string& key, IdSet& set, bool remove) {
  Ctx& c = c_;
  const auto take = [&](const std::vector<Id>& ids) {
    std::size_t n = 0;
    for (const Id id : ids) {
      if (remove ? set.erase(id) : set.add(id)) {
        ++n;
      }
    }
    host_.set_selection(set);
    host_.print(remove ? words_.tn("sel.removed", static_cast<double>(n)) : count_found(n));
  };
  auto keep_preview = preview_fn_;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members) — lives in this frame
  struct Restore {  // the JS's finally: the preview it had
    Engine& e;
    std::function<base::Array(Pt)> fn;
    Restore(Engine& en, std::function<base::Array(Pt)> f) : e(en), fn(std::move(f)) {}
    Restore(const Restore&) = delete;
    Restore& operator=(const Restore&) = delete;
    Restore(Restore&&) = delete;
    Restore& operator=(Restore&&) = delete;
    ~Restore() {
      e.preview_fn_ = std::move(fn);
      e.host_.preview({});
    }
  } restore(*this, std::move(keep_preview));
  if (key == "P") {
    std::vector<Id> ids;
    for (const Id id : prev_selection) {
      const Value* e = doc_.entities().get(id);
      if (e != nullptr && doc_.editable(*e)) {
        ids.push_back(id);
      }
    }
    if (ids.empty()) {
      host_.print(words_.t("sel.previous.none"));
      co_return true;
    }
    take(ids);
    co_return true;
  }
  if (key == "W" || key == "C") {
    const Answer a = co_await c.point(words_.t("sel.window.first"));
    const Pt pa = a.p;
    c.preview([pa](Pt cur) {
      return base::Array{
          base::make_object({{"type", "poly"},
                             {"closed", true},
                             {"pts", base::Array{base::make_object({{"x", pa.x}, {"y", pa.y}, {"b", 0}}),
                                                 base::make_object({{"x", cur.x}, {"y", pa.y}, {"b", 0}}),
                                                 base::make_object({{"x", cur.x}, {"y", cur.y}, {"b", 0}}),
                                                 base::make_object({{"x", pa.x}, {"y", cur.y}, {"b", 0}})}}})};
    });
    Ask o;
    o.base = pa;
    const Answer b = co_await c.point(words_.t("sel.window.second"), std::move(o));
    const Rect r{.min_x = std::min(pa.x, b.p.x),
                 .min_y = std::min(pa.y, b.p.y),
                 .max_x = std::max(pa.x, b.p.x),
                 .max_y = std::max(pa.y, b.p.y)};
    take(host_.pick_window(r, key == "C"));
    if (!remove) {
      set.windows.push_back(r);
    }
    co_return true;
  }
  if (key == "?") {
    host_.print(words_.t("sel.help"));
    co_return true;
  }
  co_return false;  // F(ence): not ported yet (docs/gaps.md)
}

}  // namespace cc::cmd
