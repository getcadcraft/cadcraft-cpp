// SPDX-License-Identifier: Apache-2.0
// cc::autosave — L2. Autosave and crash recovery for every app (CadCraft 0.5 app-autosave.js, the shell's): a document
// says it changed; a copy is written about 2 s after the edits stop and at least every 30 s while they go on; saving to
// a file (or undoing back to it) drops the copy; at start the app is offered the copies of sessions that are no longer
// running ("Recover your work?"). Copies older than 30 days, or beyond the newest 20 per app, are removed then.
//
//   Autosave a({.app = "markupcraft"}, std::make_unique<DirStore>(recovery_dir));
//   auto& doc = a.doc({.name = "Plans.pdf", .snapshot = [&] { return Snapshot{bytes, meta}; }});
//   doc.changed();            // after every edit
//   a.tick(now_ms);           // from the app's loop: writes what is due
//   doc.clean(); doc.close(); doc.rename("Plans (2).pdf");
//   for (const Record& r : a.orphans()) …  a.load(r) … a.discard(r);
//
// The JS runs on timers and promises; here the app's loop calls tick(now) (deterministic, and the same in tests), and
// snapshots are synchronous. A session is "running" while it holds its lock: on the desktop a lock file in the
// recovery folder (the JS's Web Lock), so a crashed or killed app's copies become recoverable; two windows of one app
// never offer each other's. Storage problems never break the app: on_error({code, message}) and the app goes on.
// Records and their words are 0.5's (format 1; keys `<app>:<session>:<doc>`; shell.autosave.* in the shell catalog).
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/value.hpp"

namespace cc::autosave {

inline constexpr int kFormat = 1;

struct Options {
  double delay = 2000;      // ms after the last change
  double max_wait = 30000;  // ms at most between the first unsaved change and its copy
  double keep_days = 30;
  std::size_t keep_per_app = 20;
};

/// One recovery copy's record (without its data).
struct Record {
  std::string key;
  std::string app;
  std::string session;
  std::string doc;
  std::string name;
  double saved_at = 0;
  double size = 0;
  base::Value meta;  // null when none
  int format = kFormat;
  [[nodiscard]] base::Value to_value() const;
  [[nodiscard]] static std::optional<Record> from_value(const base::Value& v);
};

struct StoreError {
  std::string code;  // quota write unavailable
  std::string message;
};
template <class T>
using StoreResult = std::expected<T, StoreError>;

/// Where copies live (the JS's storage adapters).
class Store {
 public:
  Store() = default;
  virtual ~Store() = default;
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&&) = delete;
  Store& operator=(Store&&) = delete;

  [[nodiscard]] virtual std::string_view kind() const = 0;
  virtual StoreResult<void> put_index(const Record& r) = 0;
  virtual StoreResult<void> put_data(const std::string& key, std::span<const std::byte> data) = 0;
  [[nodiscard]] virtual StoreResult<std::optional<std::vector<std::byte>>> get_data(const std::string& key) = 0;
  virtual StoreResult<void> del(const std::string& key) = 0;
  [[nodiscard]] virtual StoreResult<std::vector<Record>> index(const std::string& app) = 0;
  /// Takes this session's lock (held until release or the process ends).
  virtual StoreResult<void> lock(const std::string& app, const std::string& session) = 0;
  virtual void release(const std::string& app, const std::string& session) = 0;
  /// The sessions of `app` that hold their lock now.
  [[nodiscard]] virtual StoreResult<std::set<std::string>> live(const std::string& app) = 0;
};

/// In memory (tests, or where no storage is available — nothing survives a crash then).
class MemoryStore : public Store {
 public:
  [[nodiscard]] std::string_view kind() const override { return "memory"; }
  StoreResult<void> put_index(const Record& r) override;
  StoreResult<void> put_data(const std::string& key, std::span<const std::byte> data) override;
  [[nodiscard]] StoreResult<std::optional<std::vector<std::byte>>> get_data(const std::string& key) override;
  StoreResult<void> del(const std::string& key) override;
  [[nodiscard]] StoreResult<std::vector<Record>> index(const std::string& app) override;
  StoreResult<void> lock(const std::string& app, const std::string& session) override;
  void release(const std::string& app, const std::string& session) override;
  [[nodiscard]] StoreResult<std::set<std::string>> live(const std::string& app) override;
  /// Tests: make a session look stopped.
  void forget_lock(const std::string& app, const std::string& session) { live_[app].erase(session); }

 private:
  std::map<std::string, Record> idx_;
  std::map<std::string, std::vector<std::byte>> data_;
  std::map<std::string, std::set<std::string>> live_;
};

#if !defined(__EMSCRIPTEN__)
/// A recovery folder on disk (0700): <dir>/<app>/<session>.<doc>.json + .bin, <dir>/<app>/<session>.lock held with an
/// OS file lock while the session runs. Files are written atomically.
class DirStore final : public Store {
 public:
  explicit DirStore(std::filesystem::path dir);
  ~DirStore() override;
  DirStore(const DirStore&) = delete;
  DirStore& operator=(const DirStore&) = delete;
  DirStore(DirStore&&) = delete;
  DirStore& operator=(DirStore&&) = delete;

  [[nodiscard]] std::string_view kind() const override { return "folder"; }
  StoreResult<void> put_index(const Record& r) override;
  StoreResult<void> put_data(const std::string& key, std::span<const std::byte> data) override;
  [[nodiscard]] StoreResult<std::optional<std::vector<std::byte>>> get_data(const std::string& key) override;
  StoreResult<void> del(const std::string& key) override;
  [[nodiscard]] StoreResult<std::vector<Record>> index(const std::string& app) override;
  StoreResult<void> lock(const std::string& app, const std::string& session) override;
  void release(const std::string& app, const std::string& session) override;
  [[nodiscard]] StoreResult<std::set<std::string>> live(const std::string& app) override;

 private:
  [[nodiscard]] StoreResult<std::filesystem::path> file(const std::string& key, std::string_view ext) const;
  std::filesystem::path dir_;
  std::map<std::string, std::intptr_t> held_;  // app:session → the open lock file
};
#endif

struct Snapshot {
  std::vector<std::byte> data;
  std::optional<base::Value> meta;
  std::optional<double> size;
};

class Autosave;

/// One document to keep a copy of (AutosaveDoc).
class Doc {
 public:
  void changed();
  /// Writes now when changed; false when nothing was written.
  bool save();
  /// The document matches a file again: drop the copy (the doc stays tracked).
  void clean();
  /// Closed or discarded on purpose: drop the copy and forget the doc (this object is gone afterwards).
  void close();
  void rename(std::string name);

  [[nodiscard]] const std::string& id() const noexcept { return id_; }
  [[nodiscard]] const std::string& key() const noexcept { return key_; }
  [[nodiscard]] bool dirty() const noexcept { return dirty_; }
  [[nodiscard]] std::optional<double> saved_at() const noexcept { return saved_at_; }

  Doc(Autosave& owner, std::string id, std::string name, std::function<std::optional<Snapshot>()> snapshot,
      std::optional<base::Value> meta);

 private:
  friend class Autosave;
  void schedule(double ms);
  Autosave* owner_;
  std::string id_;
  std::string name_;
  std::function<std::optional<Snapshot>()> snapshot_;
  std::optional<base::Value> meta_;
  std::string key_;
  bool dirty_ = false;
  std::uint64_t rev_ = 0;
  std::optional<double> first_dirty_;
  std::optional<double> due_;  // when the next write is due
  std::optional<double> saved_at_;
  bool closed_ = false;
};

struct DocSpec {
  std::string id;  // empty = new
  std::string name;
  std::function<std::optional<Snapshot>()> snapshot;
  std::optional<base::Value> meta;
};

struct Config {
  std::string app;      // ^[a-z0-9-]{1,40}$
  std::string session;  // empty = new
  Options options;
  std::function<double()> now;  // ms (default: the system clock)
  std::function<void(const StoreError&, const Doc*)> on_error;
  std::function<void(const Doc&, double size, double saved_at)> on_saved;
};

class Autosave {
 public:
  /// Fails on a bad app slug. Takes the session's lock at once (start()).
  [[nodiscard]] static std::expected<std::unique_ptr<Autosave>, std::string> make(Config cfg,
                                                                                  std::unique_ptr<Store> store);
  ~Autosave();
  Autosave(const Autosave&) = delete;
  Autosave& operator=(const Autosave&) = delete;
  Autosave(Autosave&&) = delete;
  Autosave& operator=(Autosave&&) = delete;

  /// Releases the lock: the copies stay, and become recoverable.
  void stop();
  Doc& doc(DocSpec spec);
  /// Writes what is due by `now` (call from the app's loop).
  void tick(double now);
  /// Writes every changed document now.
  void flush();
  /// Copies this app can offer back: of sessions no longer running, newest first. Prunes old ones.
  [[nodiscard]] std::vector<Record> orphans();
  /// One copy with its data (nullopt when gone).
  [[nodiscard]] std::optional<std::pair<Record, std::vector<std::byte>>> load(const std::string& key);
  void discard(const std::string& key);

  [[nodiscard]] const std::string& app() const noexcept { return cfg_.app; }
  [[nodiscard]] const std::string& session() const noexcept { return cfg_.session; }
  [[nodiscard]] const Options& options() const noexcept { return cfg_.options; }
  [[nodiscard]] double now() const;
  [[nodiscard]] Store& store() noexcept { return *store_; }
  /// The problems reported so far (each code once: the shell says it once).
  [[nodiscard]] const std::set<std::string>& warned() const noexcept { return warned_; }

 private:
  friend class Doc;
  Autosave(Config cfg, std::unique_ptr<Store> store);
  void warn(const StoreError& e, const Doc* d);

  Config cfg_;
  std::unique_ptr<Store> store_;
  std::map<std::string, std::unique_ptr<Doc>> docs_;
  std::set<std::string> warned_;
  bool started_ = false;
};

/// newId(): 16 random bytes as 32 hex digits (randomUUID's shape is not needed: only uniqueness).
[[nodiscard]] std::string new_id();

}  // namespace cc::autosave
