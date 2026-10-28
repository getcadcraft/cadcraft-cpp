// SPDX-License-Identifier: Apache-2.0
// cc::addons — L2. The add-on host every app uses: CadCraft 0.5's assets/js/addons.js (the loader; docs/ADDONS-SPEC.md
// § 2–5), with the owner's decision of 2026-10-22 (option c): the add-ons CadCraft bundles (electrical, hvac, piping,
// apparel) are C++ modules built into the app, which plug in here as Builtin entries and get the same manifest checks,
// on/off switches, strings, storage and clean-up as 0.5's JavaScript ones. Nothing is ever loaded as native code: an
// add-on is either built in or refused. An installed package is JavaScript (its entry is .js/.mjs), and this version
// has no JavaScript engine, so every installed package is listed and refused with reason "code" (docs/gaps.md).
//
//   Loader loader({.app = "cadcraft", .api_version = "1.0", .builtins = cadcraft_builtins(), .storage = &kv});
//   loader.start();            // check, then activate every add-on that is on; never throws
//   loader.list();             // {id, name, version, source, state, reason, detail, manifest, reloadNeeded}
//   loader.set_enabled("electrical", false);
//
// ON / OFF: per app, storage key "cc-addons:<app>" = {off: {id: reason}, on: {id: true}}, as 0.5's localStorage.
// FAILURE: a module that cannot be made ("import"), makes nothing ("entry", "no activate()") or whose activate()
// throws ("activate") is switched off with the reason, its clean-ups run, the shell is told once, the app carries on.
// 0.5 also gives an async activate() 10 s; a built-in module's activate() is synchronous, so there is no timer here.
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cc/addons/trust.hpp"
#include "cc/base/value.hpp"

namespace cc::addons {

inline constexpr int kLoaderVersion = 1;

/// ID_RE /^[a-z][a-z0-9.-]{1,63}$/ with no "..", "--" or trailing "." / "-" (an add-on id).
[[nodiscard]] bool good_id(std::string_view id);

struct ApiVersion {
  int major = 0;
  int minor = 0;
};
/// apiParts(): "1" / "1.2" / "1.2.3" (String(v ?? '')) → {major, minor}, or nullopt.
[[nodiscard]] std::optional<ApiVersion> api_parts(const base::Value& v);

struct ManifestCheck {
  bool ok = false;
  base::Value manifest;  // normalised when the fields are good; else the raw value when it is an object (or null)
  std::string reason;    // manifest app api-newer api-older
  std::string detail;
};
/// checkManifest(raw, {app, apiVersion}).
[[nodiscard]] ManifestCheck check_manifest(const base::Value& raw, std::string_view app, std::string_view api_version);

/// cleanAddonData(): the add-on data an app keeps on its own records ({id: {kind, v, data}}), read as untrusted. A
/// clean copy, or undefined when nothing is left.
[[nodiscard]] base::Value clean_addon_data(const base::Value& v);

/// localStorage, as the loader uses it. A call may throw (storage blocked); the loader carries on.
class KeyValue {
 public:
  KeyValue() = default;
  KeyValue(const KeyValue&) = delete;
  KeyValue& operator=(const KeyValue&) = delete;
  KeyValue(KeyValue&&) = delete;
  KeyValue& operator=(KeyValue&&) = delete;
  virtual ~KeyValue() = default;
  [[nodiscard]] virtual std::optional<std::string> get_item(std::string_view key) = 0;
  virtual void set_item(std::string_view key, std::string value) = 0;
  virtual void remove_item(std::string_view key) = 0;
};
class MemoryKeyValue : public KeyValue {
 public:
  [[nodiscard]] std::optional<std::string> get_item(std::string_view key) override;
  void set_item(std::string_view key, std::string value) override;
  void remove_item(std::string_view key) override;
  std::map<std::string, std::string, std::less<>> items;
};

/// What an app adds to the base API (CadCraft's commands, toolbar, objects …); a module downcasts Api::app_api().
class AppApi {
 public:
  AppApi() = default;
  AppApi(const AppApi&) = delete;
  AppApi& operator=(const AppApi&) = delete;
  AppApi(AppApi&&) = delete;
  AppApi& operator=(AppApi&&) = delete;
  virtual ~AppApi() = default;
};

class Loader;
struct Entry;

/// The base API every add-on gets (ADDONS-SPEC § 5.1).
class Api {
 public:
  Api(Loader* loader, Entry* entry, base::Value strings);
  [[nodiscard]] const std::string& version() const;
  [[nodiscard]] const std::string& app_slug() const;
  [[nodiscard]] const std::string& app_name() const;
  [[nodiscard]] const std::string& id() const;
  [[nodiscard]] std::string name() const;
  [[nodiscard]] std::string addon_version() const;
  /// Its own strings (lang/<locale>.json merged: en, then the language, then the exact locale); the key when missing.
  [[nodiscard]] std::string t(std::string_view key, const base::Value& vars = base::Value()) const;
  /// JSON in storage "cc-addon:<app>:<id>:<key>"; null when absent or unreadable.
  [[nodiscard]] base::Value storage_get(std::string_view key) const;
  [[nodiscard]] bool storage_set(std::string_view key, const base::Value& v) const;
  void storage_remove(std::string_view key) const;
  /// Clean-up to run when it is switched off (newest first).
  void on_deactivate(std::function<void()> fn);
  void log(std::string_view text) const;
  void help(const base::Value& topics) const;
  [[nodiscard]] AppApi* app_api() const noexcept { return app_.get(); }
  template <class T>
  [[nodiscard]] T* app_api_as() const noexcept {
    return dynamic_cast<T*>(app_.get());
  }
  void set_app_api(std::shared_ptr<AppApi> a) { app_ = std::move(a); }

 private:
  Loader* loader_;
  Entry* entry_;
  base::Value strings_;
  std::shared_ptr<AppApi> app_;
};

/// A built-in add-on's code: activate(api) may return a clean-up (0.5's returned deactivate function).
class Module {
 public:
  Module() = default;
  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;
  Module(Module&&) = delete;
  Module& operator=(Module&&) = delete;
  virtual ~Module() = default;
  [[nodiscard]] virtual std::function<void()> activate(Api& api) = 0;
  /// 0.5's optional exported deactivate(api).
  [[nodiscard]] virtual bool has_deactivate() const { return false; }
  virtual void deactivate(Api& /*api*/) {}
};

/// One add-on shipped inside the app (0.5's addons/<key>/ folder, now compiled in).
struct Builtin {
  std::string key;                                       // its folder name
  base::Value manifest;                                  // addon.json as shipped; checked like any other
  std::map<std::string, base::Value, std::less<>> lang;  // locale → lang/<locale>.json
  base::Value help;                                      // compiled help topics, or undefined
  std::function<std::unique_ptr<Module>()> make;         // a fresh module per activation; nullptr = "no activate()"
};

/// Installed packages (0.5: IndexedDB "cadcraft-addons"; the desktop app's data folder addons/).
class PackageStore {
 public:
  PackageStore() = default;
  PackageStore(const PackageStore&) = delete;
  PackageStore& operator=(const PackageStore&) = delete;
  PackageStore(PackageStore&&) = delete;
  PackageStore& operator=(PackageStore&&) = delete;
  virtual ~PackageStore() = default;
  [[nodiscard]] virtual std::vector<Package> list() = 0;
  virtual void del(std::string_view id) = 0;
};
class MemoryPackageStore : public PackageStore {
 public:
  [[nodiscard]] std::vector<Package> list() override { return packages; }
  void del(std::string_view id) override;
  std::vector<Package> packages;
};
/// A folder of <id>-<version>.ccaddon.json files as the directory serves them; unreadable ones are skipped.
class DirPackageStore : public PackageStore {
 public:
  explicit DirPackageStore(std::filesystem::path dir) : dir_(std::move(dir)) {}
  [[nodiscard]] std::vector<Package> list() override;
  void del(std::string_view id) override;

 private:
  std::filesystem::path dir_;
};

struct Row {
  std::string id, name, version, source, state, reason, detail;
  base::Value manifest;
  bool reload_needed = false;
  /// The JS list() row: {id, name, version, source, state, reason, detail, manifest, reloadNeeded}.
  [[nodiscard]] base::Value to_value() const;
};

struct LoaderOptions {
  std::string app;
  std::string app_name;  // default: app
  std::string api_version;
  std::string locale = "en";
  std::vector<Builtin> builtins;
  KeyValue* storage = nullptr;       // none: nothing is remembered
  PackageStore* packages = nullptr;  // none: no installed packages
  /// The app's own API on top of the base (0.5 makeApi).
  std::function<std::shared_ptr<AppApi>(Api& base, const Entry& addon)> make_api;
  /// A shell message (key "shell.addons.failed.say", vars {name}), once per add-on.
  std::function<void(std::string_view key, const base::Value& vars)> say;
  std::function<void(std::string_view id, const base::Value& topics)> help;
  std::function<void(std::string_view line)> log;
};

struct Entry {
  std::string id;
  std::string source;  // bundled installed
  std::string key;
  const Builtin* builtin = nullptr;
  base::Value manifest;
  std::string state;  // ready active off failed refused
  std::string reason;
  std::string detail;
  bool default_off = false;
  bool reload_needed = false;
  std::vector<std::function<void()>> cleanups;
  std::unique_ptr<Module> mod;
  std::unique_ptr<Api> api;
};

struct InstallResult {
  bool ok = false;
  std::string reason;
  std::string detail;
};

class Loader {
 public:
  explicit Loader(LoaderOptions o);
  Loader(const Loader&) = delete;
  Loader& operator=(const Loader&) = delete;
  Loader(Loader&&) = delete;
  Loader& operator=(Loader&&) = delete;
  ~Loader();

  /// Find, check and activate everything (once). Never throws.
  std::vector<Row> start();
  [[nodiscard]] std::vector<Row> list() const;
  /// Settings › Add-ons. false for an unknown or refused add-on, or one that failed to come on.
  bool set_enabled(std::string_view id, bool on);
  /// A package from the directory: the manifest is checked, then it is refused ("code": it is JavaScript).
  [[nodiscard]] InstallResult install_package(const Package& pkg) const;
  void uninstall(std::string_view id);
  /// A listener on every change; the return value removes it.
  std::function<void()> on(std::function<void(const std::vector<Row>&)> fn);
  [[nodiscard]] bool dev_mode() const;

  [[nodiscard]] const LoaderOptions& options() const noexcept { return o_; }
  /// The shell's prefs object {…, off, on} (prefs()).
  [[nodiscard]] base::Value prefs() const;

 private:
  friend class Api;
  LoaderOptions o_;
  std::vector<std::unique_ptr<Entry>> entries_;  // in load order
  std::map<int, std::function<void(const std::vector<Row>&)>> listeners_;
  int next_listener_ = 0;
  std::vector<std::string> said_;
  bool started_ = false;

  [[nodiscard]] Entry* find(std::string_view id) const;
  void discover();
  [[nodiscard]] std::unique_ptr<Entry> entry_for(const base::Value& raw, std::string source, std::string key) const;
  void set_off(std::string_view id, const char* reason) const;
  [[nodiscard]] base::Value load_strings(const Entry& e) const;
  bool activate(Entry& e);
  bool fail(Entry& e, std::string reason, std::string detail);
  static bool run_cleanups(Entry& e);
  void changed();
};

}  // namespace cc::addons
