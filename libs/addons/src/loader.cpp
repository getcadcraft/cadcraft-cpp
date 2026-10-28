// SPDX-License-Identifier: Apache-2.0
// The add-on host (CadCraft 0.5 assets/js/addons.js): checkManifest, apiParts, cleanAddonData and AddonLoader's
// lifecycle, line by line, for add-ons built into the app.
#include "cc/addons/loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <fstream>
#include <ranges>
#include <sstream>
#include <system_error>
#include <utility>

#include "cc/base/js.hpp"
#include "cc/base/json.hpp"

namespace cc::addons {

namespace {

using base::Value;

bool lower_or_digit(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}
bool alnum(char c) {
  return lower_or_digit(c) || (c >= 'A' && c <= 'Z');
}

/// /^[a-z0-9-]{1,40}$/ (an app slug).
bool good_slug(std::string_view s) {
  return !s.empty() && s.size() <= 40 && std::ranges::all_of(s, [](char c) { return lower_or_digit(c) || c == '-'; });
}

/// SEMVER_RE /^(\d{1,4})\.(\d{1,4})\.(\d{1,6})(?:-[0-9A-Za-z.-]{1,40})?$/.
bool good_semver(std::string_view s) {
  const auto digits = [&](std::size_t& i, std::size_t most) {
    const std::size_t from = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && i - from < most) {
      ++i;
    }
    return i > from && (i >= s.size() || s[i] < '0' || s[i] > '9');
  };
  std::size_t i = 0;
  const auto dot = [&] {
    if (i >= s.size() || s[i] != '.') {
      return false;
    }
    ++i;
    return true;
  };
  if (!digits(i, 4) || !dot() || !digits(i, 4) || !dot() || !digits(i, 6)) {
    return false;
  }
  if (i == s.size()) {
    return true;
  }
  if (s[i] != '-') {
    return false;
  }
  const std::string_view pre = s.substr(i + 1);
  return !pre.empty() && pre.size() <= 40 &&
         std::ranges::all_of(pre, [](char c) { return alnum(c) || c == '.' || c == '-'; });
}

/// PERM_RE /^[a-z][a-z0-9-]{0,31}$/.
bool good_perm(std::string_view s) {
  return !s.empty() && s.size() <= 32 && s[0] >= 'a' && s[0] <= 'z' &&
         std::ranges::all_of(s, [](char c) { return lower_or_digit(c) || c == '-'; });
}

/// SPDX_RE /^[A-Za-z0-9.+-]{2,40}(?: (?:AND|OR|WITH) [A-Za-z0-9.+-]{2,40})*$/ (a token holds no space, so the
/// expression splits on single spaces: tokens at even places, operators at odd ones).
bool good_spdx(std::string_view s) {
  std::size_t at = 0;
  for (std::size_t n = 0;; ++n) {
    const std::size_t sp = s.find(' ', at);
    const std::string_view part = s.substr(at, sp == std::string_view::npos ? std::string_view::npos : sp - at);
    if (n % 2 == 0) {
      if (part.size() < 2 || part.size() > 40 ||
          !std::ranges::all_of(part, [](char c) { return alnum(c) || c == '.' || c == '+' || c == '-'; })) {
        return false;
      }
    } else if (part != "AND" && part != "OR" && part != "WITH") {
      return false;
    }
    if (sp == std::string_view::npos) {
      return n % 2 == 0;
    }
    at = sp + 1;
  }
}

/// /^(\.\/)?[A-Za-z0-9_][A-Za-z0-9_./-]*\.m?js$/, no ".." and no "//".
bool good_entry(std::string_view s) {
  if (s.contains("..") || s.contains("//")) {
    return false;
  }
  std::string_view b = s.starts_with("./") ? s.substr(2) : s;
  std::string_view stem;
  if (b.ends_with(".mjs")) {
    stem = b.substr(0, b.size() - 4);
  } else if (b.ends_with(".js")) {
    stem = b.substr(0, b.size() - 3);
  } else {
    return false;
  }
  const auto ok = [](char c) { return alnum(c) || c == '_' || c == '.' || c == '/' || c == '-'; };
  // the regex's first class may also take the "." of ".js" when the stem is empty: "x.js" needs a first char
  if (stem.empty()) {
    return false;
  }
  return (alnum(stem[0]) || stem[0] == '_') && std::ranges::all_of(stem, ok);
}

/// /[\u0000-\u001f<>]/ — a control character or an angle bracket.
bool has_control_or_angle(std::string_view s) {
  return std::ranges::any_of(s, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == '<' || c == '>'; });
}
/// /[\u0000-\u0008\u000b\u000c\u000e-\u001f]/.
bool has_control_but_space(std::string_view s) {
  return std::ranges::any_of(s, [](char c) {
    const auto u = static_cast<unsigned char>(c);
    return u < 0x20 && u != 0x09 && u != 0x0a && u != 0x0d;
  });
}
/// /^https:\/\/[^\s"'<>]+$/.
bool good_homepage(std::string_view s) {
  if (!s.starts_with("https://") || s.size() == 8) {
    return false;
  }
  for (std::size_t i = 8; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"' || c == '\'' || c == '<' || c == '>' || base::js_space_at(s, i) > 0) {
      return false;
    }
  }
  return true;
}
/// /^[a-z]+:|^\//i.
bool scheme_or_absolute(std::string_view s) {
  if (s.starts_with('/')) {
    return true;
  }
  std::size_t i = 0;
  while (i < s.size() && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))) {
    ++i;
  }
  return i > 0 && i < s.size() && s[i] == ':';
}

std::string strip_dot_slash(std::string_view s) {
  return std::string(s.starts_with("./") ? s.substr(2) : s);
}

ManifestCheck bad(const Value& raw, std::string reason, std::string detail = {}) {
  ManifestCheck r;
  r.reason = std::move(reason);
  r.detail = std::move(detail);
  r.manifest = raw.is_object() || raw.is_array() ? raw : Value(nullptr);
  return r;
}

/// Intl.PluralRules(locale).select(n) for the languages CadCraft ships and their neighbours (CLDR 45): "one" / "many" /
/// "other". The add-on's catalogue falls back to "other" for a form it lacks, as makeT does.
std::string_view plural(std::string_view locale, double n) {
  if (std::isnan(n)) {
    return "other";
  }
  const std::string_view lang = locale.substr(0, locale.find('-'));
  const double a = std::fabs(n);
  const bool integer = a == std::floor(a);
  const bool romance = lang == "es" || lang == "fr" || lang == "it" || lang == "pt";
  if (lang == "fr" || (lang == "pt" && locale != "pt-PT")) {
    if (a < 2) {
      return "one";  // i = 0, 1
    }
  } else if (a == 1 && integer) {
    return "one";
  }
  if (romance && integer && a != 0 && std::fmod(a, 1e6) == 0) {
    return "many";
  }
  return "other";
}

/// fillText(): "{{" → "{", "}}" → "}", "{name}" → String(vars[name]) when set and not null.
std::string fill_text(std::string_view text, const Value& vars) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '{' && i + 1 < text.size() && text[i + 1] == '{') {
      out += '{';
      i += 2;
      continue;
    }
    if (c == '}' && i + 1 < text.size() && text[i + 1] == '}') {
      out += '}';
      i += 2;
      continue;
    }
    if (c == '{' && i + 1 < text.size() &&
        ((text[i + 1] >= 'A' && text[i + 1] <= 'Z') || (text[i + 1] >= 'a' && text[i + 1] <= 'z') ||
         text[i + 1] == '_')) {
      std::size_t j = i + 2;
      while (j < text.size() && (alnum(text[j]) || text[j] == '_')) {
        ++j;
      }
      if (j < text.size() && text[j] == '}') {
        const std::string_view name = text.substr(i + 1, j - i - 1);
        const Value* v = vars.is_object() ? vars.find(name) : nullptr;
        if (v != nullptr && !v->is_undefined() && !v->is_null()) {
          out += base::js_string(*v);
        } else {
          out.append(text.substr(i, j - i + 1));
        }
        i = j + 1;
        continue;
      }
    }
    out += c;
    ++i;
  }
  return out;
}

const Value* member(const Value& obj, std::string_view key) {
  return obj.is_object() && !obj.is_point() ? obj.find(key) : nullptr;
}

/// {...v} for a parsed JSON value that is an object or an array (JS property order).
Value spread(const Value& v) {
  Value out = Value::object();
  if (v.is_object()) {
    for (const base::Member* m : base::js_members(v.as_object())) {
      out.set(m->key, m->value);
    }
  } else if (v.is_array()) {
    const auto& a = v.as_array();
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (!a[i].is_undefined()) {
        out.set(std::to_string(i), a[i]);
      }
    }
  }
  return out;
}

Value plain_object_or_empty(const Value* v) {
  return v != nullptr && v->is_object() ? *v : Value::object();
}

std::string json_text(const Value& v) {
  return v.is_undefined() ? std::string("undefined") : base::js_stringify(v);
}

}  // namespace

bool good_id(std::string_view id) {
  if (id.size() < 2 || id.size() > 64 || id[0] < 'a' || id[0] > 'z') {
    return false;
  }
  if (!std::ranges::all_of(id, [](char c) { return lower_or_digit(c) || c == '.' || c == '-'; })) {
    return false;
  }
  return !id.contains("..") && !id.contains("--") && id.back() != '.' && id.back() != '-';
}

std::optional<ApiVersion> api_parts(const Value& v) {
  // /^(\d{1,3})(?:\.(\d{1,3}))?(?:\.\d{1,3})?$/ on String(v ?? '')
  const std::string s = v.is_undefined() || v.is_null() ? std::string() : base::js_string(v);
  std::array<int, 3> part{};
  std::size_t n = 0;
  std::size_t i = 0;
  while (true) {
    const std::size_t from = i;
    int x = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && i - from < 3) {
      x = x * 10 + (s[i] - '0');
      ++i;
    }
    if (i == from || (i < s.size() && s[i] >= '0' && s[i] <= '9')) {
      return std::nullopt;
    }
    part.at(n) = x;
    ++n;
    if (i == s.size()) {
      break;
    }
    if (s[i] != '.' || n == 3) {
      return std::nullopt;
    }
    ++i;
  }
  return ApiVersion{part[0], n > 1 ? part[1] : 0};
}

ManifestCheck check_manifest(const Value& raw, std::string_view app, std::string_view api_version) {
  if (!raw.is_object() || raw.is_point()) {
    return bad(raw, "manifest", "addon.json is not an object");
  }
  Value m = Value::object();
  const Value& id = raw["id"];
  if (!id.is_string() || !good_id(id.as_string())) {
    return bad(raw, "manifest", "id");
  }
  m.set("id", id);
  const Value& name = raw["name"];
  if (!name.is_string() || base::js_blank(name.as_string()) || base::utf16_length(name.as_string()) > 60 ||
      has_control_or_angle(name.as_string())) {
    return bad(raw, "manifest", "name");
  }
  m.set("name", std::string(base::js_trim(name.as_string())));
  const Value& version = raw["version"];
  if (!version.is_string() || !good_semver(version.as_string())) {
    return bad(raw, "manifest", "version");
  }
  m.set("version", version);
  const Value& apps = raw["apps"];
  if (!apps.is_array() || apps.as_array().empty() ||
      !std::ranges::all_of(apps.as_array(), [](const Value& a) { return a.is_string() && good_slug(a.as_string()); })) {
    return bad(raw, "manifest", "apps");
  }
  m.set("apps", apps);
  const auto need = api_parts(raw["api"]);
  const auto have = api_parts(Value(api_version));
  if (!need) {
    return bad(raw, "manifest", "api");
  }
  m.set("api", base::js_string(raw["api"]));
  const Value& license = raw["license"];
  if (!license.is_string() || !good_spdx(license.as_string())) {
    return bad(raw, "manifest", "license");
  }
  m.set("license", license);
  const Value& entry = raw["entry"];
  if (!entry.is_string() || !good_entry(entry.as_string())) {
    return bad(raw, "manifest", "entry");
  }
  m.set("entry", strip_dot_slash(entry.as_string()));
  const Value* perms = raw.find("permissions");
  base::Array uniq;
  if (perms != nullptr && !perms->is_undefined()) {
    if (!perms->is_array() || perms->as_array().size() > 20 ||
        !std::ranges::all_of(perms->as_array(),
                             [](const Value& p) { return p.is_string() && good_perm(p.as_string()); })) {
      return bad(raw, "manifest", "permissions");
    }
    for (const Value& p : perms->as_array()) {
      if (std::ranges::find(uniq, p) == uniq.end()) {
        uniq.push_back(p);
      }
    }
  }
  m.set("permissions", Value(std::move(uniq)));
  for (const std::string_view k :
       {std::string_view("description"), std::string_view("author"), std::string_view("homepage")}) {
    const Value* v = raw.find(k);
    if (v == nullptr || v->is_undefined()) {
      continue;
    }
    if (!v->is_string() || base::utf16_length(v->as_string()) > (k == "description" ? 400U : 200U) ||
        has_control_but_space(v->as_string())) {
      return bad(raw, "manifest", std::string(k));
    }
    if (k == "homepage" && !good_homepage(v->as_string())) {
      return bad(raw, "manifest", std::string(k));
    }
    m.set(k, *v);
  }
  if (const Value* d = raw.find("default"); d != nullptr && !d->is_undefined()) {
    if (!d->is_string() || (d->as_string() != "on" && d->as_string() != "off")) {
      return bad(raw, "manifest", "default");
    }
    m.set("default", *d);
  }
  for (const std::string_view k : {std::string_view("lang"), std::string_view("help")}) {
    const Value* v = raw.find(k);
    if (v == nullptr || v->is_undefined()) {
      continue;
    }
    if (!v->is_string() || v->as_string().contains("..") || scheme_or_absolute(v->as_string())) {
      return bad(raw, "manifest", std::string(k));
    }
    m.set(k, strip_dot_slash(v->as_string()));
  }
  // Fit for this app, last: a well-formed add-on for another app or API is "not for this app", not "broken".
  ManifestCheck r;
  const auto& list = m["apps"].as_array();
  if (std::ranges::find(list, Value(app)) == list.end()) {
    r.reason = "app";
    std::string joined;
    for (std::size_t i = 0; i < list.size(); ++i) {
      joined += (i == 0 ? "" : ", ") + list[i].as_string();
    }
    r.detail = joined;
  } else if (!have || need->major != have->major) {
    r.reason = need->major > (have ? have->major : 0) ? "api-newer" : "api-older";
    r.detail = m["api"].as_string();
  } else if (need->minor > have->minor) {
    r.reason = "api-newer";
    r.detail = m["api"].as_string();
  } else {
    r.ok = true;
  }
  r.manifest = std::move(m);
  return r;
}

Value clean_addon_data(const Value& v) {
  if (!v.is_object() || v.is_point()) {
    return {};
  }
  Value out = Value::object();
  const auto members = base::js_members(v.as_object());
  for (std::size_t i = 0; i < members.size() && i < 20; ++i) {
    const std::string& id = members[i]->key;
    const Value& x = members[i]->value;
    if (!good_id(id) || !x.is_object()) {
      continue;
    }
    const Value kind = x.get("kind");
    bool kind_ok = false;
    if (kind.is_string()) {
      const std::string& k = kind.as_string();
      kind_ok = k.empty() || (k.size() <= 41 && k[0] >= 'a' && k[0] <= 'z' &&
                              std::ranges::all_of(k, [](char c) { return lower_or_digit(c) || c == '-'; }));
    }
    const Value ver = x.get("v");
    const bool ver_ok = ver.is_number() && std::isfinite(ver.as_number()) &&
                        ver.as_number() == std::trunc(ver.as_number()) && ver.as_number() >= 1 &&
                        ver.as_number() <= 9999;
    const Value data = x.get("data");
    const std::string json = base::js_stringify(data.is_undefined() || data.is_null() ? Value(nullptr) : data);
    if (!kind_ok || !ver_ok || base::utf16_length(json) > 12000) {
      continue;
    }
    auto parsed = base::parse_json(json);
    if (!parsed) {
      continue;
    }
    Value one = Value::object();
    one.set("kind", kind);
    one.set("v", ver);
    one.set("data", std::move(*parsed));
    out.set(id, std::move(one));
  }
  return out.as_object().empty() ? Value() : out;
}

// ---------------------------------------------------------------- storage and packages

std::optional<std::string> MemoryKeyValue::get_item(std::string_view key) {
  const auto it = items.find(key);
  return it == items.end() ? std::nullopt : std::optional<std::string>(it->second);
}
void MemoryKeyValue::set_item(std::string_view key, std::string value) {
  items.insert_or_assign(std::string(key), std::move(value));
}
void MemoryKeyValue::remove_item(std::string_view key) {
  if (const auto it = items.find(key); it != items.end()) {
    items.erase(it);
  }
}

namespace {
std::string package_id(const Package& p) {
  const Value id = p.manifest.get("id");
  return id.is_string() ? id.as_string() : std::string();
}
}  // namespace

void MemoryPackageStore::del(std::string_view id) {
  std::erase_if(packages, [&](const Package& p) { return package_id(p) == id; });
}

namespace {
std::optional<Package> read_package_file(const std::filesystem::path& p) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(p, ec);
  if (ec || size > kMaxBytes * 2) {  // base64 and JSON around at most 5 MB of files
    return std::nullopt;
  }
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  auto v = base::parse_json(ss.str());
  if (!v) {
    return std::nullopt;
  }
  auto r = read_package_json(*v);
  return r.ok ? std::move(r.pkg) : std::nullopt;
}
}  // namespace

std::vector<Package> DirPackageStore::list() {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (const auto& d : std::filesystem::directory_iterator(dir_, ec)) {
    if (d.is_regular_file(ec) && d.path().filename().string().ends_with(".ccaddon.json")) {
      files.push_back(d.path());
    }
  }
  std::ranges::sort(files);
  std::vector<Package> out;
  for (const auto& f : files) {
    if (auto p = read_package_file(f)) {
      out.push_back(std::move(*p));
    }
  }
  return out;
}

void DirPackageStore::del(std::string_view id) {
  std::error_code ec;
  std::vector<std::filesystem::path> gone;
  for (const auto& d : std::filesystem::directory_iterator(dir_, ec)) {
    if (d.is_regular_file(ec) && d.path().filename().string().ends_with(".ccaddon.json")) {
      if (auto p = read_package_file(d.path()); p && package_id(*p) == id) {
        gone.push_back(d.path());
      }
    }
  }
  for (const auto& g : gone) {
    std::filesystem::remove(g, ec);
  }
}

Value Row::to_value() const {
  Value v = Value::object();
  v.set("id", id);
  v.set("name", name);
  v.set("version", version);
  v.set("source", source);
  v.set("state", state);
  v.set("reason", reason);
  v.set("detail", detail);
  v.set("manifest", manifest);
  v.set("reloadNeeded", reload_needed);
  return v;
}

// ---------------------------------------------------------------- the base API

Api::Api(Loader* loader, Entry* entry, Value strings) : loader_(loader), entry_(entry), strings_(std::move(strings)) {}
const std::string& Api::version() const {
  return loader_->o_.api_version;
}
const std::string& Api::app_slug() const {
  return loader_->o_.app;
}
const std::string& Api::app_name() const {
  return loader_->o_.app_name;
}
const std::string& Api::id() const {
  return entry_->id;
}
std::string Api::name() const {
  return entry_->manifest["name"].as_string();
}
std::string Api::addon_version() const {
  return entry_->manifest["version"].as_string();
}

std::string Api::t(std::string_view key, const Value& vars) const {
  const Value* v = strings_.find(key);
  if (v == nullptr) {
    return std::string(key);
  }
  if (v->is_string()) {
    return fill_text(v->as_string(), vars);
  }
  const Value n = vars.is_object() ? vars.get("n") : Value();
  if (!n.is_undefined() && !n.is_null()) {
    const Value exact = v->get("=" + base::js_string(n));
    if (exact.is_string()) {
      return fill_text(exact.as_string(), vars);
    }
  }
  const std::string_view c =
      n.is_undefined() || n.is_null() ? std::string_view("other") : plural(loader_->o_.locale, base::js_to_number(n));
  const Value form = v->get(c);
  return fill_text(form.is_string() ? form.as_string() : v->get("other").as_string(), vars);
}

Value Api::storage_get(std::string_view key) const {
  KeyValue* st = loader_->o_.storage;
  if (st == nullptr) {
    return nullptr;
  }
  try {
    const auto v = st->get_item("cc-addon:" + loader_->o_.app + ":" + entry_->id + ":" + std::string(key));
    if (!v) {
      return nullptr;
    }
    auto parsed = base::parse_json(*v);
    return parsed ? std::move(*parsed) : Value(nullptr);
  } catch (...) {
    return nullptr;
  }
}

bool Api::storage_set(std::string_view key, const Value& v) const {
  KeyValue* st = loader_->o_.storage;
  try {
    if (st != nullptr) {
      st->set_item("cc-addon:" + loader_->o_.app + ":" + entry_->id + ":" + std::string(key), json_text(v));
    }
    return true;
  } catch (...) {
    return false;
  }
}

void Api::storage_remove(std::string_view key) const {
  KeyValue* st = loader_->o_.storage;
  try {
    if (st != nullptr) {
      st->remove_item("cc-addon:" + loader_->o_.app + ":" + entry_->id + ":" + std::string(key));
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch) storage blocked: 0.5 ignores it too
  }
}

void Api::on_deactivate(std::function<void()> fn) {
  if (fn) {
    entry_->cleanups.push_back(std::move(fn));
  }
}

void Api::log(std::string_view text) const {
  if (loader_->o_.log) {
    loader_->o_.log("[" + entry_->id + "] " + std::string(text));
  }
}

void Api::help(const Value& topics) const {
  if (loader_->o_.help) {
    try {
      loader_->o_.help(entry_->id, topics);
    } catch (...) {  // NOLINT(bugprone-empty-catch) the help panel's trouble is not the add-on's
    }
  }
}

// ---------------------------------------------------------------- the loader

Loader::Loader(LoaderOptions o) : o_(std::move(o)) {
  if (o_.app_name.empty()) {
    o_.app_name = o_.app;
  }
}

Loader::~Loader() = default;

Entry* Loader::find(std::string_view id) const {
  for (const auto& e : entries_) {
    if (e->id == id) {
      return e.get();
    }
  }
  return nullptr;
}

bool Loader::dev_mode() const {
  try {
    return o_.storage != nullptr && o_.storage->get_item("cc-addons-dev") == std::optional<std::string>("1");
  } catch (...) {
    return false;
  }
}

Value Loader::prefs() const {
  Value fallback = Value::object();
  fallback.set("off", Value::object());
  fallback.set("on", Value::object());
  try {
    std::optional<std::string> text =
        o_.storage != nullptr ? o_.storage->get_item("cc-addons:" + o_.app) : std::nullopt;
    auto v = base::parse_json(text && !text->empty() ? *text : std::string("{}"));
    if (!v) {
      return fallback;
    }
    if (!v->is_object() && !v->is_array()) {
      return fallback;
    }
    Value p = spread(*v);
    const Value off = plain_object_or_empty(member(*v, "off"));
    const Value on = plain_object_or_empty(member(*v, "on"));
    p.set("off", off);
    p.set("on", on);
    return p;
  } catch (...) {
    return fallback;
  }
}

void Loader::set_off(std::string_view id, const char* reason) const {
  Value p = prefs();
  Value& off = *p.find("off");
  Value& on = *p.find("on");
  if (reason != nullptr) {
    off.set(id, reason);
    on.erase(id);
  } else {
    off.erase(id);
    on.set(id, true);
  }
  try {
    if (o_.storage != nullptr) {
      o_.storage->set_item("cc-addons:" + o_.app, base::js_stringify(p));
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch) storage blocked: nothing is remembered, as in 0.5
  }
}

std::function<void()> Loader::on(std::function<void(const std::vector<Row>&)> fn) {
  const int k = next_listener_++;
  listeners_.emplace(k, std::move(fn));
  return [this, k] { listeners_.erase(k); };
}

void Loader::changed() {
  const auto rows = list();
  for (auto& [k, fn] : listeners_) {
    try {
      fn(rows);
    } catch (...) {  // NOLINT(bugprone-empty-catch) a listener's trouble is its own
    }
  }
}

std::vector<Row> Loader::list() const {
  std::vector<Row> out;
  out.reserve(entries_.size());
  for (const auto& e : entries_) {
    Row r;
    r.id = e->id;
    const bool has = e->manifest.is_object();
    const Value name = has ? e->manifest.get("name") : Value();
    const Value version = has ? e->manifest.get("version") : Value();
    if (name.is_string()) {
      r.name = name.as_string();
    } else if (!has) {
      r.name = e->id;
    }
    r.version = version.is_string() ? version.as_string() : std::string();
    r.source = e->source;
    r.state = e->state;
    r.reason = e->reason;
    r.detail = e->detail;
    r.manifest = e->manifest;
    r.reload_needed = e->reload_needed;
    out.push_back(std::move(r));
  }
  return out;
}

std::vector<Row> Loader::start() {
  if (started_) {
    return list();
  }
  started_ = true;
  discover();
  for (const auto& e : entries_) {
    if (e->state == "ready") {
      (void)activate(*e);
    }
  }
  changed();
  return list();
}

std::unique_ptr<Entry> Loader::entry_for(const Value& raw, std::string source, std::string key) const {
  ManifestCheck r = check_manifest(raw, o_.app, o_.api_version);
  auto e = std::make_unique<Entry>();
  const Value id = r.manifest.is_object() ? r.manifest.get("id") : Value();
  e->id = id.is_string() && good_id(id.as_string()) ? id.as_string() : source + ":" + key;
  e->source = std::move(source);
  e->key = std::move(key);
  e->manifest = std::move(r.manifest);
  e->state = r.ok ? "ready" : "refused";
  e->reason = r.reason;
  e->detail = r.detail;
  if (r.ok) {
    const Value p = prefs();
    const Value off = p["off"].get(e->id);
    if (off.truthy()) {
      e->state = "off";
      // 0.5 keeps whatever the stored value is as the reason; it is shown as text, so it is kept as String(value)
      e->reason = off.is_string() && off.as_string() == "user" ? std::string() : base::js_string(off);
    } else if (e->source == "bundled" && e->manifest["default"] == Value("off") && !p["on"].get(e->id).truthy()) {
      e->state = "off";
      e->reason.clear();
      e->default_off = true;
    }
  }
  return e;
}

void Loader::discover() {
  const auto add = [this](std::unique_ptr<Entry> e) {
    if (find(e->id) == nullptr) {
      entries_.push_back(std::move(e));
    }
  };
  for (const Builtin& b : o_.builtins) {
    auto e = entry_for(b.manifest, "bundled", b.key);
    e->builtin = &b;
    add(std::move(e));
  }
  if (o_.packages == nullptr) {
    return;
  }
  std::vector<Package> pkgs;
  try {
    pkgs = o_.packages->list();
  } catch (...) {
    pkgs.clear();
  }
  for (const Package& pkg : pkgs) {
    auto e = entry_for(pkg.manifest, "installed", package_id(pkg));
    if (e->state == "ready" || e->state == "off") {
      // The owner's decision of 2026-10-22: no JavaScript engine in this version, so a package (always JavaScript)
      // is listed and refused rather than run.
      e->state = "refused";
      e->reason = "code";
      e->detail = e->manifest["entry"].as_string();
    }
    add(std::move(e));
  }
}

Value Loader::load_strings(const Entry& e) const {
  Value out = Value::object();
  if (e.builtin == nullptr || !e.manifest.has("lang")) {
    return out;
  }
  const std::string& loc = o_.locale;
  std::vector<std::string> want;
  for (std::string l : {loc, loc.substr(0, loc.find('-')), std::string("en")}) {
    if (std::ranges::find(want, l) == want.end()) {
      want.push_back(std::move(l));
    }
  }
  for (const std::string& l : std::views::reverse(want)) {
    const auto it = e.builtin->lang.find(l);
    if (it == e.builtin->lang.end() || !it->second.is_object()) {
      continue;
    }
    for (const base::Member* m : base::js_members(it->second.as_object())) {
      const Value& v = m->value;
      if ((m->key.empty() || m->key[0] != '@') && (v.is_string() || (v.is_object() && v.get("other").is_string()))) {
        out.set(m->key, v);
      }
    }
  }
  return out;
}

bool Loader::fail(Entry& e, std::string reason, std::string detail) {
  e.state = "failed";
  e.reason = std::move(reason);
  e.detail = std::move(detail);
  set_off(e.id, "failed");
  (void)run_cleanups(e);
  if (std::ranges::find(said_, e.id) == said_.end()) {
    said_.push_back(e.id);
    if (o_.say) {
      Value vars = Value::object();
      vars.set("name", e.manifest["name"]);
      try {
        o_.say("shell.addons.failed.say", vars);
      } catch (...) {  // NOLINT(bugprone-empty-catch) the shell's trouble is not the add-on's
      }
    }
  }
  if (o_.log) {
    o_.log("add-on " + e.id + " was switched off: " + e.detail);
  }
  changed();
  return false;
}

bool Loader::activate(Entry& e) {
  if (e.state == "active") {
    return true;
  }
  if (e.builtin == nullptr) {
    return fail(e, "entry", e.manifest["entry"].is_string() ? e.manifest["entry"].as_string() : std::string());
  }
  try {
    e.mod = e.builtin->make ? e.builtin->make() : nullptr;
  } catch (const std::exception& x) {
    return fail(e, "import", x.what());
  } catch (...) {
    return fail(e, "import", "");
  }
  if (!e.mod) {
    return fail(e, "entry", "no activate()");
  }
  try {
    e.api = std::make_unique<Api>(this, &e, load_strings(e));
    if (o_.make_api) {
      e.api->set_app_api(o_.make_api(*e.api, e));
    }
    if (auto r = e.mod->activate(*e.api)) {
      e.cleanups.push_back(std::move(r));
    }
    if (e.manifest.has("help") && !e.builtin->help.is_undefined()) {
      e.api->help(e.builtin->help);
    }
  } catch (const std::exception& x) {
    return fail(e, "activate", x.what());
  } catch (...) {
    return fail(e, "activate", "");
  }
  e.state = "active";
  e.reason.clear();
  e.detail.clear();
  changed();
  return true;
}

bool Loader::run_cleanups(Entry& e) {
  std::vector<std::function<void()>> list;
  list.swap(e.cleanups);
  bool ok = true;
  for (auto& fn : std::views::reverse(list)) {
    try {
      fn();
    } catch (...) {
      ok = false;
    }
  }
  return ok;
}

bool Loader::set_enabled(std::string_view id, bool on) {
  Entry* e = find(id);
  if (e == nullptr || e->state == "refused") {
    return false;
  }
  if (on) {
    set_off(id, nullptr);
    e->state = "ready";
    e->reason.clear();
    e->mod.reset();
    return activate(*e);
  }
  set_off(id, "user");
  if (e->state == "active") {
    bool full = true;
    const bool has_deactivate = e->mod && e->mod->has_deactivate();
    try {
      if (has_deactivate) {
        e->mod->deactivate(*e->api);
      }
    } catch (...) {
      full = false;
    }
    const bool had = !e->cleanups.empty() || has_deactivate;
    full = run_cleanups(*e) && full && had;
    e->reload_needed = !full;
  }
  e->state = "off";
  e->reason.clear();
  changed();
  return true;
}

InstallResult Loader::install_package(const Package& pkg) const {
  const ManifestCheck r = check_manifest(pkg.manifest, o_.app, o_.api_version);
  if (!r.ok) {
    return {.ok = false, .reason = r.reason, .detail = r.detail};
  }
  return {.ok = false, .reason = "code", .detail = r.manifest["entry"].as_string()};
}

void Loader::uninstall(std::string_view id) {
  Entry* e = find(id);
  if (e != nullptr && e->state == "active") {
    (void)set_enabled(id, false);
  }
  if (o_.packages != nullptr) {
    o_.packages->del(id);
  }
  if (e != nullptr && e->source == "installed") {
    std::erase_if(entries_, [&](const std::unique_ptr<Entry>& x) { return x.get() == e; });
  }
  changed();
}

}  // namespace cc::addons
