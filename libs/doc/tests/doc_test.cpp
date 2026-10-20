// SPDX-License-Identifier: Apache-2.0
// cc::doc: doc.js's behaviour — one undo step per change, nested changes, rollback, mark / squash, layers renamed with
// their objects, layouts as one step — and load() against the JS document on the public fixtures.
#include "cc/doc/doc.hpp"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/cad/import.hpp"
#include "cc/cad/read_dxf.hpp"
#include "cc/dwg/read.hpp"
#include "cc/dxf/read.hpp"
#include "golden.hpp"

namespace fs = std::filesystem;
using cc::base::Array;
using cc::base::make_object;
using cc::base::Value;
using cc::doc::Doc;

namespace {

cc::i18n::Catalog english() {
  const auto bytes = cc::base::read_file(fs::path(CC_ASSETS_DIR) / "lang/en.json", std::uint64_t{1} << 26U);
  REQUIRE(bytes.has_value());
  auto c = cc::i18n::Catalog::parse("en", cc::base::as_text(*bytes));
  REQUIRE(c.has_value());
  return std::move(*c);
}

Value line(double x1, double y1, double x2, double y2) {
  return make_object({{"type", "line"}, {"x1", x1}, {"y1", y1}, {"x2", x2}, {"y2", y2}});
}

}  // namespace

TEST_CASE("every change is one undo step; undo and redo walk them") {
  Doc d;
  cc::doc::Id a = 0;
  d.change("LINE", [&] {
    a = static_cast<cc::doc::Id>(d.add(line(0, 0, 10, 0))["id"].as_number());
    (void)d.add(line(10, 0, 10, 10));
  });
  REQUIRE(d.entities().size() == 2);
  CHECK(d.entities().get(a)->get("layer") == Value("0"));
  CHECK(d.entities().get(a)->get("color").is_null());
  d.change("MOVE", [&] {
    Value moved = *d.entities().get(a);
    moved.set("x1", 5);
    (void)d.update(a, moved);
  });
  CHECK(d.entities().get(a)->get("x1") == Value(5));
  CHECK(d.undo() == std::optional<std::string>("MOVE"));
  CHECK(d.entities().get(a)->get("x1") == Value(0));
  CHECK(d.undo() == std::optional<std::string>("LINE"));
  CHECK(d.entities().size() == 0);
  CHECK(!d.undo().has_value());
  CHECK(d.redo() == std::optional<std::string>("LINE"));
  CHECK(d.entities().size() == 2);
  CHECK(d.dirty());
  d.change("NOTHING", [] {});  // a change with no edits is not a step
  CHECK(d.redo() == std::optional<std::string>("MOVE"));
}

TEST_CASE("a nested change is part of the outer one; an exception rolls the change back") {
  Doc d;
  d.change("OUTER", [&] {
    (void)d.add(line(0, 0, 1, 1));
    d.change("INNER", [&] { (void)d.add(line(1, 1, 2, 2)); });
  });
  CHECK(d.entities().size() == 2);
  CHECK(d.undo() == std::optional<std::string>("OUTER"));
  CHECK(d.entities().size() == 0);
  CHECK_THROWS_AS(d.change("FAIL",
                           [&] {
                             (void)d.add(line(0, 0, 1, 1));
                             throw std::runtime_error("refused");
                           }),
                  std::runtime_error);
  CHECK(d.entities().size() == 0);  // rolled back, and no step recorded
  CHECK(!d.can_undo());
  CHECK_THROWS(d.add(line(0, 0, 1, 1)));  // edits only inside change()
}

TEST_CASE("mark and squash fold a command's steps into one") {
  Doc d;
  const std::size_t m = d.mark();
  d.change("A", [&] { (void)d.add(line(0, 0, 1, 0)); });
  d.change("B", [&] { (void)d.add(line(0, 0, 2, 0)); });
  d.squash(m, "TWO LINES");
  CHECK(d.undo() == std::optional<std::string>("TWO LINES"));
  CHECK(d.entities().size() == 0);
}

TEST_CASE("renaming a layer moves its objects, in the model, on sheets and in blocks, as one step") {
  Doc d;
  d.change("SETUP", [&] {
    Value e = line(0, 0, 1, 1);
    e.set("layer", "WALLS");
    (void)d.add(e);
    Value b = make_object({{"name", "B"}, {"entities", Array{e}}});
    (void)d.define_block(b);
  });
  d.set_layer("WALLS", make_object({{"name", "W"}, {"color", 1}}), true);
  bool moved = false;
  d.entities().each([&](const cc::doc::Id&, const Value& e) { moved = e.get("layer") == Value("W"); });
  CHECK(moved);
  CHECK(d.blocks().get("B")->get("entities").as_array()[0].get("layer") == Value("W"));
  CHECK(d.layers().get("W")->get("color") == Value(1));
  CHECK(d.current.get("layer") == Value("W"));
  CHECK(d.undo() == std::optional<std::string>("RENAME LAYER"));
  CHECK(d.layers().has("WALLS"));
  CHECK(!d.layers().has("W"));
}

TEST_CASE("a deleted object comes back last, as a JS Map puts it back") {
  Doc d;
  cc::doc::Id first = 0;
  d.change("A", [&] {
    first = static_cast<cc::doc::Id>(d.add(line(0, 0, 1, 0))["id"].as_number());
    (void)d.add(line(0, 0, 2, 0));
  });
  d.change("ERASE", [&] { d.remove(first); });
  (void)d.undo();
  std::vector<cc::doc::Id> order;
  d.entities().each([&](const cc::doc::Id& id, const Value&) { order.push_back(id); });
  CHECK(order == std::vector<cc::doc::Id>{2, 1});
}

TEST_CASE("the document after load equals the JS Doc's on the public fixtures") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "doc";
  const fs::path corpus(CC_CORPUS_DIR);
  if (!fs::exists(golden) || !fs::exists(corpus / "fixtures")) {
    if (cc::base::env_var("CC_REQUIRE_CORPUS")) {
      FAIL("CC_REQUIRE_CORPUS is set but the public fixtures are missing");
    }
    SKIP("references or fixtures missing (cmake -DDEST=build/corpus -P tools/corpus.cmake)");
  }
  const cc::i18n::Catalog words = english();
  int checked = 0;
  for (const auto& entry : fs::directory_iterator(golden)) {
    const std::string name = entry.path().filename().string();
    const std::string dwg = name.substr(0, name.size() - 5);
    fs::path src = corpus / "fixtures/dwg" / dwg;
    if (!fs::exists(src)) {
      src = corpus / "fixtures/electrical" / dwg;
    }
    REQUIRE(fs::exists(src));
    const auto ref = cc::base::read_file(entry.path(), std::uint64_t{1} << 30U);
    REQUIRE(ref.has_value());
    const auto want = cc::base::parse_json(cc::base::as_text(*ref));
    REQUIRE(want.has_value());
    const auto bytes = cc::base::read_file(src, std::uint64_t{1} << 30U);
    REQUIRE(bytes.has_value());
    Doc d;
    if (dwg.ends_with(".dxf")) {  // dxf.js readDxf, the text as cadcraft.js decodes it
      auto drawing = cc::cad::read_dxf(cc::dxf::decode_text(*bytes), words,
                                       [](std::string_view t) { return cc::dxf::read_dxf_model(t); });
      REQUIRE(drawing.has_value());
      d.load(std::move(*drawing), words);
    } else {
      d.load(cc::cad::import_dwg(cc::dwg::read_dwg(*bytes),
                                 cc::base::as_text(std::span<const std::byte>(*bytes).first(6)), words),
             words);
    }
    std::vector<std::string> diffs;
    cc::test::compare(*want, d.dump(), "", diffs);
    INFO(dwg << ": " << (diffs.empty() ? std::string() : diffs.front()));
    CHECK(diffs.empty());
    ++checked;
  }
  CHECK(checked >= 4);
}

TEST_CASE("pictures (pictures.js): files sniffed by content, kept by base name, found by a prefixed name too") {
  // a 4 × 2 PNG's signature and IHDR are all sniffPicture reads
  std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 'I', 'H', 'D',
                                'R',  0,   0,   0,   4,    0,    0,    0,    2, 8, 2, 0,  0,   0};
  const auto sniffed = cc::doc::sniff_picture(png);
  CHECK((sniffed && sniffed->w == 4 && sniffed->h == 2));
  CHECK_FALSE(cc::doc::sniff_picture(std::vector<std::uint8_t>{'t', 'e', 'x', 't'}));
  const std::vector<std::uint8_t> jpg{0xff, 0xd8, 0xff, 0xc0, 0, 17, 8, 0, 3, 0, 5, 3, 0, 0, 0, 0};
  const auto info = cc::doc::jpeg_info(jpg);
  CHECK((info && info->w == 5 && info->components == 3));
  CHECK(cc::doc::picture_key("C:\\Logos\\Logo.PNG") == "logo.png");
  cc::doc::Doc d;
  CHECK(cc::doc::add_picture(d, "notes.txt", std::vector<std::uint8_t>{1, 2}).error() == "pic.not_picture");
  const auto rec = cc::doc::add_picture(d, "/x/Proj-Logo.png", png);
  REQUIRE(rec);
  CHECK((*rec)->name == "Proj-Logo.png");
  CHECK(cc::doc::find_picture(d, "D:\\PROJ-LOGO.PNG") == *rec);
  CHECK(cc::doc::find_picture(d, "logo.png") == *rec);   // eTransmit adds a prefix
  CHECK(cc::doc::find_picture(d, "lo.png") == nullptr);  // too short a stem to match inside another name
  CHECK(d.picture_version == 1);
  CHECK(cc::doc::free_picture_name(d) == "*I1");
}
