// SPDX-License-Identifier: Apache-2.0
// cc::cmd — L5. CadCraft 0.5's command line (commands.js Engine), ported: commands are coroutines that ask for input
// through a context (`c.point(…)`, `c.distance(…)`, `c.select(…)`, …) and edit the document with doc.change(); every
// edit a command makes is one undo step when it ends. Input reaches the engine as
//   engine.text(line)              typed at the command line (Enter sends it; an empty line repeats the last command)
//   engine.click(point, raw)       a click: `point` after snaps and ortho, `raw` the cursor itself
//   engine.window(rect, crossing)  a selection window while selecting
//   engine.escape()                Esc: the waiting command is cancelled (its frames destroyed)
// The host (the viewer, the CLI's script runner, a test) prints, shows prompts, keeps the selection and answers
// picks. The same commands therefore run with a window, headless from a script, or from an agent.
//
// Not ported yet (docs/gaps.md): constraints kept through edits (ckeep.js), deferred tangent snaps, the clipboard,
// the commands outside M4.6's list.
#pragma once

#include <coroutine>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/cmd/task.hpp"
#include "cc/doc/doc.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::cmd {

using doc::Id;

struct Pt {
  double x = 0, y = 0;
};
struct Rect {
  double min_x = 0, min_y = 0, max_x = 0, max_y = 0;
};

/// A JS Set of ids: insertion order, no repeats.
class IdSet {
 public:
  [[nodiscard]] bool has(Id id) const;
  /// Adds; false when it was there.
  bool add(Id id);
  /// Removes; false when it was not there.
  bool erase(Id id);
  void clear() noexcept { ids_.clear(); }
  [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }
  [[nodiscard]] bool empty() const noexcept { return ids_.empty(); }
  [[nodiscard]] const std::vector<Id>& ids() const noexcept { return ids_; }
  [[nodiscard]] auto begin() const noexcept { return ids_.begin(); }
  [[nodiscard]] auto end() const noexcept { return ids_.end(); }
  std::vector<Rect> windows;  // the selection windows that made it (STRETCH)

 private:
  std::vector<Id> ids_;
};

/// The view's middle and size in drawing units (ZOOM C, NAMEDVIEW S).
struct ViewRect {
  double cx = 0, cy = 0, w = 0, h = 0;
};

/// What the engine needs from whoever shows it.
class Host {
 public:
  Host() = default;
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;
  Host(Host&&) = delete;
  Host& operator=(Host&&) = delete;
  virtual ~Host() = default;
  virtual void print(const std::string& text) = 0;
  virtual void prompt(const std::string& text) = 0;
  /// The cursor in world coordinates, when there is one.
  [[nodiscard]] virtual std::optional<Pt> cursor() const { return std::nullopt; }
  [[nodiscard]] virtual const IdSet& selection() const = 0;
  virtual void set_selection(IdSet s) = 0;
  /// The object under a point, if any.
  virtual std::optional<Id> pick_at(Pt raw) = 0;
  /// The objects a selection window takes (crossing: touched; else inside).
  virtual std::vector<Id> pick_window(const Rect& r, bool crossing) = 0;
  /// Rubber-band entities to draw over the view (each move); empty: none.
  virtual void preview(const base::Array& entities) { (void)entities; }
  /// A command started (its name) or ended (nullptr).
  virtual void on_command(const std::string* name) { (void)name; }
  /// Shift held (it flips TRIM ↔ EXTEND, removes from a selection).
  [[nodiscard]] virtual bool shift() const { return false; }

  // ---- what a host with a view and files does; a script's host has none of it (the defaults)
  virtual void zoom_extents() {}
  /// ZOOM P: false when there is no previous view.
  virtual bool zoom_previous() { return false; }
  virtual void zoom_by(double factor) { (void)factor; }
  virtual void zoom_window(Pt a, Pt b, bool exact) { (void)a, (void)b, (void)exact; }
  [[nodiscard]] virtual std::optional<ViewRect> view_rect() const { return std::nullopt; }
  /// Turn the view so the angle's direction is level; false when the host cannot (PLAN says so).
  virtual bool turn_view(double ang) {
    (void)ang;
    return false;
  }
  virtual void ucs_changed() {}
  virtual void show_layers() {}
  virtual void new_drawing() {}
  virtual void open_file() {}
  /// SAVE (format empty: the drawing's own) / DXFOUT ("dxf", remember false).
  virtual void save_file(const std::string& format, bool remember) { (void)format, (void)remember; }
  virtual void save_as() {}
  /// PLOT: false when the host cannot plot.
  virtual bool plot() { return false; }
  // the Properties panel (propcmds.js: host.props): open, fold away, Quick Properties on / off, a point picked for it
  virtual void props_open() {}
  virtual void props_close() {}
  virtual void props_toggle_quick() {}
  [[nodiscard]] virtual std::string props_pick_label() const { return {}; }
  virtual void props_picked(Pt p) { (void)p; }
  // sheets: the clock for a title block's date, panes in the drawing window, working through a viewport
  [[nodiscard]] virtual double now_ms() const { return 0; }
  [[nodiscard]] virtual bool can_split() const { return false; }
  virtual void set_panes(const std::string& layout) { (void)layout; }
  [[nodiscard]] virtual bool can_enter_viewport() const { return false; }
  [[nodiscard]] virtual std::optional<base::Value> viewport_entered() const { return std::nullopt; }
  virtual void enter_viewport(const base::Value& vp) { (void)vp; }
  virtual void leave_viewport() {}
  [[nodiscard]] virtual std::optional<base::Value> viewport_at(Pt p) const {
    (void)p;
    return std::nullopt;
  }
};

/// A prompt option: `key` to type, `label` shown (translated), `en` the English label (scripts type English words).
struct Option {
  std::string key, label, en;
  bool hidden = false;
};

/// A prompt's settings (the JS opts object).
struct Ask {
  std::optional<Pt> base;       // rubber band from here; typed distances run from it
  std::vector<Option> options;  // keywords the prompt takes
  std::optional<bool> none;     // Enter answers nothing (true) / undefined (false); unset: Enter ends a point prompt
  base::Value def;              // the default Enter gives (undefined: none)
  bool ucs = false;             // an angle from the UCS X axis
  bool length = false;          // number: a signed length
  bool raw = false;             // string: kept as typed
  bool remove = false, single = false, fresh = false;   // select
  bool empty = false, path = false;                     // entity: a click on nothing / a dragged path answers too
  std::function<base::Value(std::string_view)> accept;  // a command's own token
};

/// An answer to a prompt (the JS's resolved value).
struct Answer {
  enum class Kind : std::uint8_t { nothing, point, option, number, text, set, entity, empty, path, typed };
  Kind kind = Kind::nothing;
  Pt p;                // point, entity (the pick), empty (where)
  std::string option;  // option: its key
  double number = 0;   // number, distance, angle
  std::string text;    // string
  IdSet set;           // select
  Id id = 0;           // entity
  bool shift = false;  // entity, empty, path
  std::vector<Pt> path;
  base::Value typed;  // accept()'s value
  /// JS truthiness of the value (0, NaN and '' are falsy; null / undefined too).
  [[nodiscard]] bool truthy() const noexcept;
  [[nodiscard]] bool is(std::string_view key) const { return kind == Kind::option && option == key; }
  /// `k?.option || k` of a keyword answer (an option's key or a default given as a string).
  [[nodiscard]] std::string key() const {
    if (kind == Kind::option) {
      return option;
    }
    return kind == Kind::text ? text : std::string();
  }
};

class Engine;
class Ctx;

using CommandFn = std::function<Task<void>(Ctx&)>;
struct CommandDef {
  std::string name;
  std::vector<std::string> aliases;
  CommandFn fn;
  bool no_repeat = false;
  bool keep_selection = false;
  bool hidden = false;  // a _COMMAND the panels run: never listed
  std::string help;     // a language key
};

/// The awaitable a prompt returns.
class AskAwait {
 public:
  AskAwait(Engine& e, std::string kind, std::string prompt, Ask opts)
      : e_(e), kind_(std::move(kind)), prompt_(std::move(prompt)), opts_(std::move(opts)) {}
  [[nodiscard]] bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h);
  Answer await_resume() { return std::move(out_); }

 private:
  Engine& e_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) — lives in one co_await
  std::string kind_, prompt_;
  Ask opts_;
  Answer out_;
};

/// The `c` a command gets.
class Ctx {
 public:
  Ctx(Engine& e, doc::Doc& d, Host& h, const i18n::Catalog& w) : engine(e), doc(d), host(h), words(w) {}
  // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members,misc-non-private-member-variables-in-classes)
  Engine& engine;
  doc::Doc& doc;
  Host& host;
  const i18n::Catalog& words;
  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members,misc-non-private-member-variables-in-classes)

  void print(const std::string& s) const { host.print(s); }
  [[nodiscard]] std::string t(std::string_view key,
                              std::initializer_list<std::pair<std::string_view, std::string>> fills = {}) const {
    return words.t(key, fills);
  }
  /// opt(key, label): the label translated when the catalog has opt.<word>.
  [[nodiscard]] Option opt(std::string key, const std::string& label) const;
  /// The rubber band for the cursor (nullptr: none).
  void preview(std::function<base::Array(Pt)> fn);
  AskAwait point(std::string prompt, Ask o = {});
  AskAwait distance(std::string prompt, Ask o = {});
  AskAwait angle(std::string prompt, Ask o = {});
  AskAwait number(std::string prompt, Ask o = {});
  AskAwait string(std::string prompt, Ask o = {});
  AskAwait keyword(std::string prompt, Ask o = {});
  AskAwait entity(std::string prompt, Ask o = {});
  /// Objects to work on: the current selection (noun-verb), else ask until Enter (ALL, L, P, W, C, R, A, ?).
  Task<Answer> select(std::string prompt = {}, Ask o = {});
  /// doc.change('add', () => doc.add(e)) → the entity as stored.
  base::Value add(const base::Value& e);
  /// ent(c, e): { layer, color } of the drawing's current settings, its current linetype / lineweight, then e.
  [[nodiscard]] base::Value ent(const base::Value& e) const;
};

class Engine {
 public:
  Engine(doc::Doc& d, Host& h, const i18n::Catalog& words);
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) = delete;
  Engine& operator=(Engine&&) = delete;
  ~Engine();

  /// Starts a command by name or alias (a running one is cancelled first).
  void run(std::string_view name);
  void text(std::string_view raw);
  /// A click; false when nothing waits for one (or a select prompt found nothing there: the host starts a window).
  bool click(Pt point, std::optional<Pt> raw = std::nullopt);
  void window(const Rect& r, bool crossing);
  /// Esc: true when a prompt was cancelled.
  bool escape();
  [[nodiscard]] bool busy() const noexcept { return root_.has_value(); }
  /// The kind of input waited for ("point", "select", …) or empty.
  [[nodiscard]] std::string expects() const { return waiting_ ? waiting_->kind : std::string(); }
  [[nodiscard]] std::string prompt_text() const;
  /// The rubber band for a cursor position (the host calls it on every move).
  [[nodiscard]] base::Array preview(Pt cursor) const;
  /// commands.js lookup(name): a command by name or alias.
  [[nodiscard]] static const CommandDef* lookup(std::string_view name);
  [[nodiscard]] static std::vector<const CommandDef*> commands();

  // ---- what commands keep between runs (the JS engine's fields)
  std::optional<Pt> last_point;               // for @ input
  std::optional<Pt> rubber_base;              // ortho and direct distance use it
  base::Value state = base::Value::object();  // lastRadius, lastRotation, lastSides, lastOffset, …
  std::size_t cmd_mark = 0;
  IdSet prev_selection;

  // ---- for Ctx
  void wait(std::string kind, std::string prompt, Ask opts, std::coroutine_handle<> h, Answer* out);
  void set_preview(std::function<base::Array(Pt)> fn);
  Task<bool> select_option(const std::string& key, IdSet& set, bool remove);
  [[nodiscard]] Ctx& ctx() noexcept { return c_; }

 private:
  struct Waiting {
    std::string kind, prompt;
    Ask opts;
    IdSet* set = nullptr;  // select: the set being built (lives in the select task)
    std::coroutine_handle<> h;
    Answer* out = nullptr;
  };
  doc::Doc& doc_;
  Host& host_;
  const i18n::Catalog& words_;
  Ctx c_;
  std::optional<Waiting> waiting_;
  std::optional<Task<void>> root_;
  const CommandDef* running_ = nullptr;
  std::string last_;
  std::function<base::Array(Pt)> preview_fn_;
  IdSet* select_set_ = nullptr;
  friend class Ctx;

  void resolve(Answer v);
  void resume(std::coroutine_handle<> h);
  void after_step();
  void finish(bool cancelled);
  [[nodiscard]] std::optional<Pt> parse_point(std::string_view typed) const;
};

/// units.js parseLength(raw, lunits): a typed length → drawing units, or nothing (feet and inches in an engineering
/// or architectural drawing).
[[nodiscard]] std::optional<double> parse_length(std::string_view raw, const base::Value& lunits);

}  // namespace cc::cmd
