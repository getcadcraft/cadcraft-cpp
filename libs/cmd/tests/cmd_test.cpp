// SPDX-License-Identifier: Apache-2.0
// The command line (the commands.js port): every script in tests/commands replayed on a fresh drawing gives what
// CadCraft 0.5's Engine gave (printed lines, prompts, the model, undo labels; tests/golden/README.md); and the engine's
// own behaviour — Esc destroys the waiting command's frames, a command is one undo step, typed points.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/cmd/engine.hpp"
#include "cc/cmd/script.hpp"
#include "cc/doc/doc.hpp"
#include "cc/i18n/i18n.hpp"
#include "golden.hpp"

namespace fs = std::filesystem;
using cc::base::Value;

namespace {

const cc::i18n::Catalog& words() {
  static const cc::i18n::Catalog kWords = [] {
    const auto text = cc::base::read_file(fs::path(CC_ASSETS_DIR) / "lang/en.json", std::uint64_t{1} << 26U);
    REQUIRE(text.has_value());
    auto c = cc::i18n::Catalog::parse("en", cc::base::as_text(*text));
    REQUIRE(c.has_value());
    return std::move(*c);
  }();
  return kWords;
}

Value read_json(const fs::path& p) {
  const auto text = cc::base::read_file(p, std::uint64_t{1} << 28U);
  REQUIRE(text.has_value());
  auto v = cc::base::parse_json(cc::base::as_text(*text));
  REQUIRE(v.has_value());
  return std::move(*v);
}

class TestHost final : public cc::cmd::Host {
 public:
  std::vector<std::string> printed;
  cc::cmd::IdSet sel;
  void print(const std::string& t) override { printed.push_back(t); }
  void prompt(const std::string& /*t*/) override {}
  [[nodiscard]] const cc::cmd::IdSet& selection() const override { return sel; }
  void set_selection(cc::cmd::IdSet s) override { sel = std::move(s); }
  std::optional<cc::doc::Id> pick_at(cc::cmd::Pt /*raw*/) override { return std::nullopt; }
  std::vector<cc::doc::Id> pick_window(const cc::cmd::Rect& /*r*/, bool /*crossing*/) override { return {}; }
};

}  // namespace

TEST_CASE("golden: the command scripts give what CadCraft 0.5's command line gave") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "commands";
  if (!fs::exists(golden)) {
    SKIP("references missing (tests/golden/js-0.5-commands.tar.gz)");
  }
  int checked = 0;
  for (const auto& entry : fs::directory_iterator(CC_SCRIPTS_DIR)) {
    if (entry.path().extension() != ".json") {
      continue;
    }
    const Value scripts = read_json(entry.path());
    const Value want = read_json(golden / entry.path().filename());
    REQUIRE(want.as_array().size() == scripts.as_array().size());
    for (std::size_t i = 0; i < scripts.as_array().size(); ++i) {
      const Value got = cc::cmd::run_script(scripts.as_array()[i], words());
      INFO(entry.path().filename().string() << ": " << cc::base::js_string(scripts.as_array()[i]["name"]));
      std::vector<std::string> diffs;
      cc::test::compare(want.as_array()[i], got, "", diffs);
      for (const auto& d : diffs) {
        UNSCOPED_INFO(d);
      }
      CHECK(diffs.empty());
      ++checked;
    }
  }
  CHECK(checked > 0);
}

TEST_CASE("Esc while a command waits ends it; its edits so far are one undo step") {
  cc::doc::Doc d;
  TestHost host;
  cc::cmd::Engine eng(d, host, words());
  eng.text("LINE");
  eng.text("0,0");
  eng.text("10,0");
  eng.text("10,10");
  CHECK(eng.busy());
  CHECK(eng.expects() == "point");
  CHECK(eng.escape());
  CHECK(!eng.busy());
  CHECK(d.entities().size() == 2);
  CHECK(d.undo_labels() == std::vector<std::string>{"LINE"});
  CHECK(host.printed.back() == "*Cancel*");
  CHECK(d.undo().value_or("") == "LINE");
  CHECK(d.entities().size() == 0);
}

TEST_CASE("a running command is cancelled when another starts; Enter repeats the last") {
  cc::doc::Doc d;
  TestHost host;
  cc::cmd::Engine eng(d, host, words());
  eng.text("CIRCLE");
  eng.text("0,0");
  eng.run("LINE");  // CIRCLE waits for its radius: cancelled
  CHECK(eng.busy());
  eng.escape();
  eng.text("");  // LINE again
  CHECK(eng.busy());
  eng.text("1,1");
  eng.text("2,2");
  eng.text("");
  CHECK(!eng.busy());
  CHECK(d.entities().size() == 1);
}

TEST_CASE("typed lengths: plain, fractions, feet and inches only in an imperial format") {
  CHECK(cc::cmd::parse_length("2.5", Value(2)) == 2.5);
  CHECK(cc::cmd::parse_length("1-1/2", Value(2)) == 1.5);
  CHECK(cc::cmd::parse_length("3/4", Value(2)) == 0.75);
  CHECK(cc::cmd::parse_length("-2", Value(2)) == -2);
  CHECK(!cc::cmd::parse_length("12'6\"", Value(2)));
  CHECK(cc::cmd::parse_length("12'6\"", Value(4)) == 150);
  CHECK(cc::cmd::parse_length("1'-6-1/2\"", Value(4)) == 18.5);
  CHECK(!cc::cmd::parse_length("abc", Value(2)));
  CHECK(!cc::cmd::parse_length("", Value(2)));
}

TEST_CASE("a hostile script stops at its step limit") {
  Value steps = Value::array();
  for (int i = 0; i < 50; ++i) {
    steps.push(Value("LINE"));
  }
  const Value script = cc::base::make_object({{"name", "many"}, {"steps", steps}});
  const Value r = cc::cmd::run_script(script, words(), {.max_steps = 10});
  bool limited = false;
  for (const Value& l : r["log"].as_array()) {
    limited = limited || cc::base::js_string(l.as_array()[1]) == "*script step limit*";
  }
  CHECK(limited);
}
