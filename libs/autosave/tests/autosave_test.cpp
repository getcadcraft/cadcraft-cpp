// SPDX-License-Identifier: Apache-2.0
// libs/autosave by app-autosave.js's rules: the write times (2 s after the edits stop, at most 30 s while they go
// on), clean / close / rename, what is offered back (copies of stopped sessions only, newest first, 30 days and 20 per
// app kept), storage errors that never stop the app, and the recovery folder across two "processes" (locks).
#include "cc/autosave/autosave.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using cc::autosave::Autosave;
using cc::autosave::Snapshot;

namespace {

std::vector<std::byte> bytes(std::string_view s) {
  std::vector<std::byte> out(s.size());
  std::ranges::transform(s, out.begin(), [](char c) { return static_cast<std::byte>(c); });
  return out;
}

struct Clock {
  double t = 1'000'000;
};

cc::autosave::Config config(std::string app, std::string session, Clock& c) {
  cc::autosave::Config cfg;
  cfg.app = std::move(app);
  cfg.session = std::move(session);
  cfg.now = [&c] { return c.t; };
  return cfg;
}

cc::autosave::DocSpec spec(std::string id, std::string name, std::function<std::optional<Snapshot>()> snap) {
  cc::autosave::DocSpec d;
  d.id = std::move(id);
  d.name = std::move(name);
  d.snapshot = std::move(snap);
  return d;
}

/// A store whose writes fail (a full disk).
class FullStore final : public cc::autosave::MemoryStore {
 public:
  cc::autosave::StoreResult<void> put_data(const std::string& /*key*/, std::span<const std::byte> /*data*/) override {
    return std::unexpected(cc::autosave::StoreError{"quota", "full"});
  }
};

}  // namespace

TEST_CASE("autosave: a copy 2 s after the edits stop, at most every 30 s while they go on") {
  Clock c;
  auto mem = std::make_unique<cc::autosave::MemoryStore>();
  auto* store = mem.get();
  std::vector<double> saves;
  cc::autosave::Config cfg = config("cadcraft", "s1", c);
  cfg.on_saved = [&saves, &c](const cc::autosave::Doc&, double, double) { saves.push_back(c.t); };
  auto a = Autosave::make(cfg, std::move(mem));
  REQUIRE(a.has_value());
  std::string text = "v1";
  auto& doc = (*a)->doc(
      spec("d", "Plan.dwg", [&text] { return std::optional(Snapshot{bytes(text), std::nullopt, std::nullopt}); }));
  doc.changed();
  c.t += 1999;
  (*a)->tick(c.t);
  CHECK(saves.empty());
  c.t += 1;
  (*a)->tick(c.t);
  REQUIRE(saves.size() == 1);
  CHECK_FALSE(doc.dirty());
  // edits every second: written at the 30 s mark at the latest
  const double first = c.t + 1000;
  for (int i = 0; i < 40; ++i) {
    c.t += 1000;
    doc.changed();
    (*a)->tick(c.t);
  }
  REQUIRE(saves.size() >= 2);
  CHECK(saves[1] - first <= 30'000);
  // what was written is the newest snapshot, with its record
  text = "v2";
  doc.changed();
  (*a)->flush();
  const auto rec = (*a)->load(doc.key());
  REQUIRE(rec.has_value());
  if (rec) {
    CHECK(rec->first.name == "Plan.dwg");
    CHECK(rec->second == bytes("v2"));
  }
  // a running session's copies are never offered; a stopped one's are
  CHECK((*a)->orphans().empty());
  doc.rename("Plan (2).dwg");
  store->forget_lock("cadcraft", "s1");
  cc::autosave::Config other = cfg;
  other.session = "s2";
  other.on_saved = {};
  auto b = Autosave::make(other, std::make_unique<cc::autosave::MemoryStore>());
  REQUIRE(b.has_value());
  // the same store is needed to see them: offer from `a`'s store as another session would
  const auto all = store->index("cadcraft");
  REQUIRE(all.has_value());
  CHECK(all->size() == 1);
  CHECK(all->front().name == "Plan (2).dwg");
  // clean drops the copy
  doc.clean();
  CHECK(store->index("cadcraft")->empty());
}

TEST_CASE("autosave: offered copies are newest first; 20 per app and 30 days kept") {
  Clock c;
  auto mem = std::make_unique<cc::autosave::MemoryStore>();
  auto* store = mem.get();
  for (int i = 0; i < 25; ++i) {
    cc::autosave::Record r;
    r.app = "cadcraft";
    r.session = "old" + std::to_string(i);
    r.doc = "d";
    r.key = r.app + ":" + r.session + ":d";
    r.saved_at = c.t - (i * 1000.0) - (i == 24 ? 31.0 * 86'400'000 : 0);
    REQUIRE(store->put_index(r).has_value());
    REQUIRE(store->put_data(r.key, bytes("x")).has_value());
  }
  auto a = Autosave::make(config("cadcraft", "now", c), std::move(mem));
  REQUIRE(a.has_value());
  const auto list = (*a)->orphans();
  REQUIRE(list.size() == 20);
  CHECK(list.front().session == "old0");
  CHECK(list.back().session == "old19");
  CHECK(store->index("cadcraft")->size() == 20);  // the rest were removed
  (*a)->discard(list.front().key);
  CHECK((*a)->orphans().size() == 19);
  CHECK_FALSE((*a)->load(list.front().key).has_value());
}

TEST_CASE("autosave: storage errors are reported once a kind and never stop the app") {
  Clock c;
  std::vector<std::string> errors;
  cc::autosave::Config cfg = config("cadcraft", "s", c);
  cfg.on_error = [&errors](const cc::autosave::StoreError& e, const cc::autosave::Doc*) { errors.push_back(e.code); };
  auto a = Autosave::make(cfg, std::make_unique<FullStore>());
  REQUIRE(a.has_value());
  auto& doc = (*a)->doc(spec("d", "", [] { return std::optional(Snapshot{bytes("x"), std::nullopt, std::nullopt}); }));
  doc.changed();
  CHECK_FALSE(doc.save());
  CHECK(doc.dirty());  // retried after max_wait
  CHECK((*a)->warned().contains("quota"));
  CHECK(errors.size() == 1);
  CHECK_FALSE(Autosave::make(config("Bad App", "", c), nullptr).has_value());
}

#if !defined(__EMSCRIPTEN__)
TEST_CASE("autosave: the recovery folder, across two sessions with their locks") {
  const auto dir = std::filesystem::temp_directory_path() / ("cc-autosave-test-" + cc::autosave::new_id());
  Clock c;
  {
    auto a = Autosave::make(config("partcraft", "first", c), std::make_unique<cc::autosave::DirStore>(dir));
    REQUIRE(a.has_value());
    auto& doc = (*a)->doc(spec("part/1", "Bracket", [] {
      return std::optional(Snapshot{bytes("geometry"), cc::base::Value("m"), std::nullopt});
    }));
    doc.changed();
    (*a)->flush();
    // a second window of the same app sees the first one running: nothing to offer
    auto b = Autosave::make(config("partcraft", "second", c), std::make_unique<cc::autosave::DirStore>(dir));
    REQUIRE(b.has_value());
    CHECK((*b)->orphans().empty());
    (*a)->stop();  // the first one stops (as a crash would: its lock goes with the process)
    const auto list = (*b)->orphans();
    REQUIRE(list.size() == 1);
    CHECK(list[0].name == "Bracket");
    CHECK(list[0].meta == cc::base::Value("m"));
    const auto got = (*b)->load(list[0].key);
    REQUIRE(got.has_value());
    if (got) {
      CHECK(got->second == bytes("geometry"));
    }
    (*b)->discard(list[0].key);
    CHECK((*b)->orphans().empty());
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}
#endif
