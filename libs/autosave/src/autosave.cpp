// SPDX-License-Identifier: Apache-2.0
// app-autosave.js, ported. See autosave.hpp.
#include "cc/autosave/autosave.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <system_error>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/random.hpp"
#include "cc/base/reader.hpp"

#if !defined(__EMSCRIPTEN__)
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#endif

namespace cc::autosave {

using base::Value;

namespace {

/// What 0.5 lets go (a `.catch(() => {})`): a store call whose failure changes nothing for the person.
template <class T>
void let_go(const StoreResult<T>& /*r*/) {}

[[maybe_unused]] std::string hex(std::string_view s) {  // the native store only
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string out;
  for (const char c : s) {
    const auto b = static_cast<unsigned char>(c);
    out += kHex[b >> 4U];
    out += kHex[b & 15U];
  }
  return out;
}

bool good_app(std::string_view a) {
  return !a.empty() && a.size() <= 40 &&
         std::ranges::all_of(a, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

std::string rec_key(std::string_view app, std::string_view session, std::string_view doc) {
  return std::string(app) + ":" + std::string(session) + ":" + std::string(doc);
}

}  // namespace

std::string new_id() {
  std::array<std::uint8_t, 16> b{};
  base::os_random(b);
  std::string s;
  static constexpr std::string_view kHex = "0123456789abcdef";
  for (const std::uint8_t x : b) {
    s += kHex[x >> 4U];
    s += kHex[x & 15U];
  }
  return s;
}

Value Record::to_value() const {
  Value v = Value::object();
  v.append("key", key);
  v.append("format", format);
  v.append("app", app);
  v.append("session", session);
  v.append("doc", doc);
  v.append("name", name);
  v.append("savedAt", saved_at);
  v.append("size", size);
  v.append("meta", meta.is_undefined() ? Value(nullptr) : meta);
  return v;
}

std::optional<Record> Record::from_value(const Value& v) {
  if (!v.is_object()) {
    return std::nullopt;
  }
  const auto str = [&v](const char* k) -> std::optional<std::string> {
    const Value x = v.get(k);
    if (!x.is_string()) {
      return std::nullopt;
    }
    return x.as_string();
  };
  Record r;
  auto key = str("key");
  auto app = str("app");
  auto session = str("session");
  auto doc = str("doc");
  if (!key || !app || !session || !doc || !v.get("savedAt").is_number()) {
    return std::nullopt;
  }
  r.key = *key;
  r.app = *app;
  r.session = *session;
  r.doc = *doc;
  r.name = str("name").value_or("");
  r.saved_at = v.get("savedAt").as_number();
  r.size = v.get("size").num_or(0);
  r.meta = v.has("meta") ? v.get("meta") : Value(nullptr);
  r.format = static_cast<int>(v.get("format").num_or(kFormat));
  return r;
}

// ---------------------------------------------------------------------------------------------------- MemoryStore
StoreResult<void> MemoryStore::put_index(const Record& r) {
  idx_[r.key] = r;
  return {};
}
StoreResult<void> MemoryStore::put_data(const std::string& key, std::span<const std::byte> data) {
  data_[key] = std::vector<std::byte>(data.begin(), data.end());
  return {};
}
StoreResult<std::optional<std::vector<std::byte>>> MemoryStore::get_data(const std::string& key) {
  const auto it = data_.find(key);
  if (it == data_.end()) {
    return std::optional<std::vector<std::byte>>{};
  }
  return std::optional<std::vector<std::byte>>{it->second};
}
StoreResult<void> MemoryStore::del(const std::string& key) {
  idx_.erase(key);
  data_.erase(key);
  return {};
}
StoreResult<std::vector<Record>> MemoryStore::index(const std::string& app) {
  std::vector<Record> out;
  for (const auto& [k, r] : idx_) {
    if (r.app == app) {
      out.push_back(r);
    }
  }
  return out;
}
StoreResult<void> MemoryStore::lock(const std::string& app, const std::string& session) {
  live_[app].insert(session);
  return {};
}
void MemoryStore::release(const std::string& app, const std::string& session) {
  live_[app].erase(session);
}
StoreResult<std::set<std::string>> MemoryStore::live(const std::string& app) {
  return live_[app];
}

// ---------------------------------------------------------------------------------------------------- DirStore
#if !defined(__EMSCRIPTEN__)
namespace {

StoreError io_error(const std::string& what) {
  return StoreError{"write", what};
}

#if defined(_WIN32)
std::intptr_t open_locked(const std::filesystem::path& p, bool wait) {
  const HANDLE h =
      CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return -1;
  OVERLAPPED o{};
  const DWORD flags = LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
  if (LockFileEx(h, flags, 0, 1, 0, &o) == 0) {
    CloseHandle(h);
    return -1;
  }
  return reinterpret_cast<std::intptr_t>(h);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) — a handle
}
void close_locked(std::intptr_t fd) {
  CloseHandle(
      reinterpret_cast<HANDLE>(fd));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr)
}
#else
std::intptr_t open_locked(const std::filesystem::path& p, bool wait) {
  const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  if (fd < 0) {
    return -1;
  }
  if (::flock(fd, LOCK_EX | (wait ? 0 : LOCK_NB)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}
void close_locked(std::intptr_t fd) {
  ::close(static_cast<int>(fd));
}
#endif

}  // namespace

DirStore::DirStore(std::filesystem::path dir) : dir_(std::move(dir)) {}

DirStore::~DirStore() {
  for (const auto& [k, fd] : held_) {
    close_locked(fd);
  }
}

StoreResult<std::filesystem::path> DirStore::file(const std::string& key, std::string_view ext) const {
  // key = app:session:doc
  const std::size_t a = key.find(':');
  const std::size_t b = a == std::string::npos ? std::string::npos : key.find(':', a + 1);
  if (b == std::string::npos) {
    return std::unexpected(io_error("not a recovery key"));
  }
  const std::string app = key.substr(0, a);
  if (!good_app(app)) {
    return std::unexpected(io_error("not a recovery key"));
  }
  return dir_ / app / (hex(key.substr(a + 1, b - a - 1)) + "." + hex(key.substr(b + 1)) + std::string(ext));
}

namespace {
StoreResult<void> ensure_dir(const std::filesystem::path& d) {
  std::error_code ec;
  std::filesystem::create_directories(d, ec);
  if (ec) {
    return std::unexpected(StoreError{"unavailable", "the recovery folder cannot be made: " + ec.message()});
  }
  std::filesystem::permissions(d, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
  return {};
}
StoreResult<void> write_atomic(const std::filesystem::path& p, std::span<const std::byte> bytes) {
  if (auto ok = ensure_dir(p.parent_path()); !ok) {
    return ok;
  }
  if (auto w = base::write_file_atomic(p, bytes); !w) {
    const bool full = w.error().message.contains("No space");
    return std::unexpected(StoreError{full ? "quota" : "write", w.error().message});
  }
  return {};
}
}  // namespace

StoreResult<void> DirStore::put_index(const Record& r) {
  auto p = file(r.key, ".json");
  if (!p) {
    return std::unexpected(p.error());
  }
  const std::string text = base::to_json(r.to_value());
  return write_atomic(*p, std::as_bytes(std::span(text)));
}

StoreResult<void> DirStore::put_data(const std::string& key, std::span<const std::byte> data) {
  auto p = file(key, ".bin");
  if (!p) {
    return std::unexpected(p.error());
  }
  return write_atomic(*p, data);
}

StoreResult<std::optional<std::vector<std::byte>>> DirStore::get_data(const std::string& key) {
  auto p = file(key, ".bin");
  if (!p) {
    return std::unexpected(p.error());
  }
  std::error_code ec;
  if (!std::filesystem::exists(*p, ec)) {
    return std::optional<std::vector<std::byte>>{};
  }
  auto b = base::read_file(*p, std::uint64_t{1} << 32U);
  if (!b) {
    return std::unexpected(io_error(b.error().message));
  }
  return std::optional<std::vector<std::byte>>{std::move(*b)};
}

StoreResult<void> DirStore::del(const std::string& key) {
  auto j = file(key, ".json");
  auto d = file(key, ".bin");
  if (!j || !d) {
    return std::unexpected(io_error("not a recovery key"));
  }
  std::error_code ec;
  std::filesystem::remove(*j, ec);
  std::filesystem::remove(*d, ec);
  return {};
}

StoreResult<std::vector<Record>> DirStore::index(const std::string& app) {
  std::vector<Record> out;
  if (!good_app(app)) {
    return out;
  }
  std::error_code ec;
  const auto folder = dir_ / app;
  if (!std::filesystem::is_directory(folder, ec)) {
    return out;
  }
  for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
    if (e.path().extension() != ".json") {
      continue;
    }
    auto b = base::read_file(e.path(), std::uint64_t{1} << 20U);
    if (!b) {
      continue;
    }
    auto v = base::parse_json(base::as_text(*b));
    if (!v) {
      continue;
    }
    if (auto r = Record::from_value(*v); r && r->app == app) {
      out.push_back(std::move(*r));
    }
  }
  return out;
}

StoreResult<void> DirStore::lock(const std::string& app, const std::string& session) {
  if (!good_app(app)) {
    return std::unexpected(StoreError{"unavailable", "not an app"});
  }
  if (auto ok = ensure_dir(dir_ / app); !ok) {
    return ok;
  }
  const std::string k = app + ":" + session;
  if (held_.contains(k)) {
    return {};
  }
  const std::intptr_t fd = open_locked(dir_ / app / (hex(session) + ".lock"), false);
  if (fd < 0) {
    return std::unexpected(StoreError{"unavailable", "the session's lock cannot be held"});
  }
  held_[k] = fd;
  return {};
}

void DirStore::release(const std::string& app, const std::string& session) {
  const auto it = held_.find(app + ":" + session);
  if (it == held_.end()) {
    return;
  }
  close_locked(it->second);
  held_.erase(it);
  std::error_code ec;
  std::filesystem::remove(dir_ / app / (hex(session) + ".lock"), ec);
}

StoreResult<std::set<std::string>> DirStore::live(const std::string& app) {
  std::set<std::string> out;
  for (const auto& [k, fd] : held_) {
    if (k.starts_with(app + ":")) {
      out.insert(k.substr(app.size() + 1));
    }
  }
  std::error_code ec;
  const auto folder = dir_ / app;
  if (!std::filesystem::is_directory(folder, ec)) {
    return out;
  }
  for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
    if (e.path().extension() != ".lock") {
      continue;
    }
    const std::string stem = e.path().stem().string();
    std::string session;
    bool ok = stem.size() % 2 == 0;
    for (std::size_t i = 0; ok && i < stem.size(); i += 2) {
      const auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
      if (!std::isxdigit(static_cast<unsigned char>(stem[i])) ||
          !std::isxdigit(static_cast<unsigned char>(stem[i + 1]))) {
        ok = false;
      } else {
        session += static_cast<char>((nib(stem[i]) << 4) | nib(stem[i + 1]));
      }
    }
    if (!ok || out.contains(session)) {
      continue;
    }
    const std::intptr_t fd = open_locked(e.path(), false);
    if (fd < 0) {
      out.insert(session);  // someone holds it: running
    } else {
      close_locked(fd);  // nobody did: a stopped session's leftover
      std::filesystem::remove(e.path(), ec);
    }
  }
  return out;
}
#endif

// ---------------------------------------------------------------------------------------------------- Doc
Doc::Doc(Autosave& owner, std::string id, std::string name, std::function<std::optional<Snapshot>()> snapshot,
         std::optional<Value> meta)
    : owner_(&owner),
      id_(std::move(id)),
      name_(std::move(name)),
      snapshot_(std::move(snapshot)),
      meta_(std::move(meta)),
      key_(rec_key(owner.app(), owner.session(), id_)) {}

void Doc::schedule(double ms) {
  due_ = owner_->now() + ms;
}

void Doc::changed() {
  if (closed_) {
    return;
  }
  dirty_ = true;
  ++rev_;
  const double t = owner_->now();
  if (!first_dirty_) {
    first_dirty_ = t;
  }
  const double wait = std::max(0.0, std::min(owner_->options().delay, *first_dirty_ + owner_->options().max_wait - t));
  schedule(wait);
}

bool Doc::save() {
  if (closed_ || !dirty_) {
    return false;
  }
  due_.reset();
  const std::uint64_t rev = rev_;
  Autosave& o = *owner_;
  bool ok = false;
  std::optional<Snapshot> snap;
  try {
    snap = snapshot_();
  } catch (const std::exception& e) {
    o.warn(StoreError{"write", e.what()}, this);
  }
  if (!snap) {
    if (rev == rev_) {
      dirty_ = false;
      first_dirty_.reset();
    }
    return false;
  }
  const double saved_at = o.now();
  const double size = snap->size ? *snap->size : static_cast<double>(snap->data.size());
  Record r;
  r.key = key_;
  r.app = o.app();
  r.session = o.session();
  r.doc = id_;
  r.name = name_;
  r.saved_at = saved_at;
  r.size = size;
  if (snap->meta) {
    r.meta = *snap->meta;
  } else if (meta_) {
    r.meta = *meta_;
  } else {
    r.meta = Value(nullptr);
  }
  auto wd = o.store().put_data(key_, snap->data);
  auto wi = wd ? o.store().put_index(r) : StoreResult<void>{};
  if (!wd || !wi) {
    o.warn(!wd ? wd.error() : wi.error(), this);
  } else {
    saved_at_ = saved_at;
    if (rev == rev_) {
      dirty_ = false;
      first_dirty_.reset();
    }
    ok = true;
    if (o.cfg_.on_saved) {
      o.cfg_.on_saved(*this, size, saved_at);
    }
  }
  if (dirty_ && !closed_) {  // changed meanwhile, or a failed write: again, a failed one after max_wait
    const double now = o.now();
    const double wait =
        std::max(0.0, std::min(o.options().delay, first_dirty_.value_or(now) + o.options().max_wait - now));
    schedule(ok ? wait : o.options().max_wait);
  }
  return ok;
}

void Doc::clean() {
  due_.reset();
  dirty_ = false;
  first_dirty_.reset();
  saved_at_.reset();
  let_go(owner_->store().del(key_));
}

void Doc::close() {
  clean();
  closed_ = true;
  owner_->docs_.erase(id_);  // destroys this object: nothing may touch it afterwards
}

void Doc::rename(std::string name) {
  name_ = std::move(name);
  if (!saved_at_) {
    return;
  }
  auto all = owner_->store().index(owner_->app());
  if (!all) {
    return;
  }
  for (Record& r : *all) {
    if (r.key == key_ && !closed_) {
      r.name = name_;
      let_go(owner_->store().put_index(r));
    }
  }
}

// ---------------------------------------------------------------------------------------------------- Autosave
Autosave::Autosave(Config cfg, std::unique_ptr<Store> store) : cfg_(std::move(cfg)), store_(std::move(store)) {}

std::expected<std::unique_ptr<Autosave>, std::string> Autosave::make(Config cfg, std::unique_ptr<Store> store) {
  if (!good_app(cfg.app)) {
    return std::unexpected("Autosave needs the app slug.");
  }
  if (cfg.session.empty()) {
    cfg.session = new_id();
  }
  if (!store) {
    store = std::make_unique<MemoryStore>();
  }
  std::unique_ptr<Autosave> a(
      new Autosave(std::move(cfg), std::move(store)));  // NOLINT(cppcoreguidelines-owning-memory)
  if (auto ok = a->store_->lock(a->cfg_.app, a->cfg_.session); !ok) {
    a->warn(ok.error(), nullptr);
    a->store_ = std::make_unique<MemoryStore>();  // the app keeps working, without surviving a crash
    let_go(a->store_->lock(a->cfg_.app, a->cfg_.session));
  }
  a->started_ = true;
  return a;
}

Autosave::~Autosave() {
  stop();
}

void Autosave::stop() {
  if (!started_) {
    return;
  }
  for (auto& [id, d] : docs_) {
    d->due_.reset();
  }
  store_->release(cfg_.app, cfg_.session);
  started_ = false;
}

double Autosave::now() const {
  if (cfg_.now) {
    return cfg_.now();
  }
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
}

Doc& Autosave::doc(DocSpec spec) {
  std::string id = spec.id.empty() ? new_id() : std::move(spec.id);
  auto d = std::make_unique<Doc>(*this, id, std::move(spec.name), std::move(spec.snapshot), std::move(spec.meta));
  Doc& ref = *d;
  docs_.insert_or_assign(id, std::move(d));
  return ref;
}

void Autosave::tick(double now) {
  std::vector<Doc*> due;
  for (auto& [id, d] : docs_) {
    if (d->due_ && *d->due_ <= now) {
      due.push_back(d.get());
    }
  }
  for (Doc* d : due) {
    d->save();
  }
}

void Autosave::flush() {
  std::vector<Doc*> all;
  all.reserve(docs_.size());
  for (auto& [id, d] : docs_) {
    all.push_back(d.get());
  }
  for (Doc* d : all) {
    if (d->dirty_) {
      d->save();
    }
  }
}

std::vector<Record> Autosave::orphans() {
  auto all = store_->index(cfg_.app);
  if (!all) {
    return {};
  }
  auto live = store_->live(cfg_.app);
  std::set<std::string> running = live ? *live : std::set<std::string>{};
  running.insert(cfg_.session);
  const double t = now();
  const double max_age = cfg_.options.keep_days * 86'400'000;
  std::vector<Record> dead;
  for (Record& r : *all) {
    if (!running.contains(r.session)) {
      dead.push_back(std::move(r));
    }
  }
  std::ranges::stable_sort(dead, [](const Record& a, const Record& b) { return a.saved_at > b.saved_at; });
  std::vector<Record> keep;
  for (Record& r : dead) {
    if (t - r.saved_at > max_age || keep.size() >= cfg_.options.keep_per_app) {
      let_go(store_->del(r.key));
    } else {
      keep.push_back(std::move(r));
    }
  }
  return keep;
}

std::optional<std::pair<Record, std::vector<std::byte>>> Autosave::load(const std::string& key) {
  auto all = store_->index(cfg_.app);
  if (!all) {
    return std::nullopt;
  }
  const auto it = std::ranges::find_if(*all, [&key](const Record& r) { return r.key == key; });
  if (it == all->end()) {
    return std::nullopt;
  }
  auto data = store_->get_data(key);
  if (!data || !*data) {
    return std::nullopt;
  }
  return std::pair<Record, std::vector<std::byte>>{*it, std::move(**data)};
}

void Autosave::discard(const std::string& key) {
  let_go(store_->del(key));
}

void Autosave::warn(const StoreError& e, const Doc* d) {
  if (cfg_.on_error) {
    try {
      cfg_.on_error(e, d);
    } catch (...) {  // NOLINT(bugprone-empty-catch) — the app's handler never stops autosave
    }
  }
  warned_.insert(e.code);
}

}  // namespace cc::autosave
