// SPDX-License-Identifier: Apache-2.0
// libs/lisp: the sessions in tests/lisp/ give what CadCraft 0.5's createLisp gave (tests/golden/js-0.5-lisp.tar.gz,
// made by tools/parity/lisp.mjs), and the limits the JS has no counterpart for (docs/gaps.md).
#include "cc/lisp/lisp.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"
#include "cc/lisp/script.hpp"
#include "golden.hpp"

namespace fs = std::filesystem;
using cc::base::Value;
using cc::lisp::Interp;
using cc::lisp::Result;
using cc::lisp::run_now;

namespace {

Value read_json(const fs::path& p) {
  const auto bytes = cc::base::read_file(p, std::uint64_t{1} << 28U);
  REQUIRE(bytes);
  auto v = cc::base::parse_json(cc::base::as_text(*bytes));
  REQUIRE(v);
  return std::move(*v);
}

const cc::i18n::Catalog& words() {
  static const cc::i18n::Catalog c = [] {
    const auto bytes = cc::base::read_file(fs::path(CC_ASSETS_DIR) / "lang/en.json", std::uint64_t{1} << 26U);
    REQUIRE(bytes);
    auto w = cc::i18n::Catalog::parse("en", cc::base::as_text(*bytes));
    REQUIRE(w);
    return std::move(*w);
  }();
  return c;
}

cc::lisp::Options opts(std::string* out) {
  cc::lisp::Options o;
  o.out = [out](const std::string& s, bool /*piece*/) { *out += s; };
  return o;
}

Result eval(Interp& L, std::string text) {
  return run_now(L.eval_text(std::move(text)));
}

}  // namespace

TEST_CASE("golden: the LISP sessions give what CadCraft 0.5's LISP gave") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "lisp";
  if (!fs::exists(golden)) {
    SKIP("references missing (tests/golden/js-0.5-lisp.tar.gz)");
  }
  int checked = 0;
  for (const auto& entry : fs::directory_iterator(CC_SCRIPTS_DIR)) {
    if (entry.path().extension() != ".json") {
      continue;
    }
    const Value sessions = read_json(entry.path());
    const Value want = read_json(golden / entry.path().filename());
    REQUIRE(want.as_array().size() == sessions.as_array().size());
    for (std::size_t i = 0; i < sessions.as_array().size(); ++i) {
      const Value got = cc::lisp::run_session(sessions.as_array()[i], &words());
      INFO(entry.path().filename().string() << ": " << cc::base::js_string(sessions.as_array()[i]["name"]));
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

TEST_CASE("files from older routines: UTF-8, else Windows-1252") {
  const std::array<std::byte, 11> b = {std::byte{0x28}, std::byte{0x70}, std::byte{0x72}, std::byte{0x69},
                                       std::byte{0x6e}, std::byte{0x63}, std::byte{0x20}, std::byte{0x22},
                                       std::byte{0xa9}, std::byte{0x22}, std::byte{0x29}};
  CHECK(cc::lisp::decode_source(b) == "(princ \"\xC2\xA9\")");
  const std::array<std::byte, 5> bom = {std::byte{0xEF}, std::byte{0xBB}, std::byte{0xBF}, std::byte{0x28},
                                        std::byte{0x29}};
  CHECK(cc::lisp::decode_source(bom) == "()");
  const std::array<std::byte, 2> high = {std::byte{0x80}, std::byte{0x9F}};
  CHECK(cc::lisp::decode_source(high) == "\xE2\x82\xAC\xC5\xB8");
}

TEST_CASE("the command line reads on until the text is complete") {
  CHECK(cc::lisp::balanced("(+ 1 2)"));
  CHECK(!cc::lisp::balanced("(+ 1"));
  CHECK(!cc::lisp::balanced("\"abc"));
  CHECK(cc::lisp::balanced("(+ 1 2))"));  // complete, and wrong: the evaluation says so
  CHECK(cc::lisp::balanced(""));
}

TEST_CASE("wildcards through the library's API: a pattern the JS refuses is an error, not an exception") {
  CHECK(cc::lisp::wcmatch("BLOCK1", "BLOCK#").value());
  const auto refused = cc::lisp::wcmatch("a", "[z-a]");
  REQUIRE(!refused);
  CHECK(refused.error().message == "Invalid regular expression: /^(?:[z-a])$/: Range out of order in character class");
}

TEST_CASE("limits: nesting, depth, indirection") {
  std::string out;
  Interp L(opts(&out));
  // read text nested 4096 lists deep reads (the reader keeps its open lists on the heap: under ASan a recursive
  // reader overflowed the native stack before its cap; fuzz/corpus/lisp/regress-reader-nesting.lsp)
  CHECK(eval(L, "(length (quote " + std::string(4095, '(') + std::string(4095, ')') + "))").ok);
  CHECK(cc::lisp::show(eval(L, "(length (quote " + std::string(4000, '\'') + "x))").value) == "2");
  // read text nested past 4096 lists: V8's words (the JS reader overflows near 5 300)
  const Result deep = eval(L, std::string(5000, '(') + std::string(5000, ')'));
  CHECK(!deep.ok);
  CHECK(deep.error == "Maximum call stack size exceeded");
  // a runaway recursion stops at kMaxDepth, and the bindings come back
  const Result r = eval(L, "(setq n 7) (defun r (n) (r (1+ n))) (r 0)");
  CHECK(!r.ok);
  CHECK(r.error == "Maximum call stack size exceeded");
  CHECK(cc::lisp::show(eval(L, "n").value) == "7");
  CHECK(L.depth == 0);
  // a value that names a value that names … the first
  CHECK(eval(L, "(setq a 'b b 'a) (a)").error == "Maximum call stack size exceeded");
}

TEST_CASE("limits: the step budget and the longest string") {
  std::string out;
  cc::lisp::Options o = opts(&out);
  o.max_steps = 100000;
  o.max_string = 1000;
  Interp L(std::move(o));
  const Result loop = eval(L, "(while t nil)");
  CHECK(!loop.ok);
  CHECK(loop.cancel);
  CHECK(loop.error == "Function cancelled");
  Interp C([&] {
    cc::lisp::Options c = opts(&out);
    c.max_string = 1000;
    return c;
  }());
  const Result big = eval(C, "(setq s \"ab\") (repeat 20 (setq s (strcat s s)))");
  CHECK(!big.ok);
  CHECK(big.error == "Invalid string length");
  // a doubled structure printed: bounded by the longest string, not by its 2^60 leaves
  const Result dag = eval(C, "(setq x '(1)) (repeat 60 (setq x (list x x))) (vl-prin1-to-string x)");
  CHECK(dag.error == "Invalid string length");
  // and compared: every node a step, so the budget stops it
  Interp E([&] {
    cc::lisp::Options c = opts(&out);
    c.max_steps = 1000000;
    return c;
  }());
  const Result cmp = eval(E, "(setq x '(1) y '(1)) (repeat 60 (setq x (list x x) y (list y y))) (equal x y)");
  CHECK(cmp.cancel);
}

TEST_CASE("structures a million deep, and functions holding functions, are freed without recursion") {
  std::string out;
  Interp L(opts(&out));
  CHECK(eval(L, "(setq x nil) (repeat 1000000 (setq x (list x))) (setq x nil) 1").ok);
  CHECK(eval(L, "(setq f nil) (repeat 300000 (setq f (eval (list 'lambda '() (list 'quote (list f)))))) (setq f nil)")
            .ok);
  // equal on two deep ones stops with V8's words where V8's stack would
  const Result eq = eval(L, "(setq a 1 b 1) (repeat 9000 (setq a (list a) b (list b))) (equal a b)");
  CHECK(eq.error == "Maximum call stack size exceeded");
}

TEST_CASE("selection-set and temp-file counters start over per session") {
  std::string out;
  Interp L(opts(&out));
  cc::lisp::reset_counters();
  CHECK(cc::lisp::show(eval(L, "(vl-filename-mktemp)").value) == "\"temp/$VL~~001\"");
  cc::lisp::reset_counters();
  CHECK(cc::lisp::show(eval(L, "(vl-filename-mktemp \"a.b\")").value) == "\"temp/a001.b\"");
}
