// SPDX-License-Identifier: Apache-2.0
// The 3D model of a drawing: a file's faces in (fromFile → meshDef), the 3DFACEs and CadCraft's JSON out (forFile3d),
// against CadCraft 0.5's model3d.js on the same faces (the expected values come from Node).
#include "cc/m3d/model.hpp"

#include <array>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/value.hpp"
#include "cc/m3d/drawing3d.hpp"

using cc::base::Array;
using cc::base::Value;

namespace {

Value pt(double x, double y, double z) {
  return {Array{x, y, z}};
}

}  // namespace

TEST_CASE("a file's faces become one group and go back out as model3d.js writes them") {
  const auto face = [](std::initializer_list<Value> p) { return cc::base::make_object({{"pts", Array(p)}}); };
  Array faces = {
      face({pt(0, 0, 0), pt(1000, 0, 0), pt(1000, 1000, 0), pt(0, 1000, 0)}),
      face({pt(0, 0, 0), pt(1000, 1000, 0), pt(0, 1000, 0), pt(0, 0, 0)}),
      face({pt(2000, 0, 0), pt(3000, 0, 0), pt(2200, 200, 0), pt(2000, 1000, 0)}),
      face({pt(0, 0, 500), pt(1000, 0, 500), pt(1000, 1000, 900), pt(0, 1000, 500)}),
  };
  const Value view =
      cc::base::make_object({{"eye", pt(1, 2, 3)}, {"target", pt(0, 0, 0)}, {"persp", true}, {"fov", 40}});
  const Value in3d = cc::base::make_object({{"text", nullptr}, {"faces", std::move(faces)}, {"view", view}});
  cc::m3d::Model m;
  const auto got = cc::m3d::from_file(m, in3d, "millimeters", "Faces from the file");
  CHECK(got.loose == 4);
  const auto f = cc::m3d::for_file3d(m, "millimeters");
  REQUIRE(f.has_value());
  const cc::m3d::File3d& ff = f.value();  // NOLINT(bugprone-unchecked-optional-access): REQUIRE above
  const std::vector<std::string> want = {
      R"(CC3D:1:{"format":"modelcraft","version":1,"units":"mm","nextId":2,"defs":[{"id":"root","kind":"root","name":"Model","next":2,"verts":[],"edges":[],"faces":[],"insts":[{"id":1,"def":"d1","m":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}]},{"id":"d1")",
      R"(CC3D:2:,"kind":"group","name":"Faces\u0020from\u0020the\u0020file","next":32,"verts":[[1,0,0,0],[2,1,0,0],[3,1,1,0],[4,0,1,0],[12,2,0,0],[13,3,0,0],[14,2.2,0.2,0],[15,2,1,0],[21,0,0,0.5],[22,1,0,0.5],[23,1,1,0.9],[24,0,1,0.5]],"edges":[[)",
      R"(CC3D:3:6,1,2],[7,2,3],[8,3,4,1],[9,4,1,1],[11,1,3],[17,12,13],[18,13,14],[19,14,15],[20,15,12],[26,21,22],[27,22,23],[28,23,21],[30,23,24],[31,24,21]],"faces":[{"id":5,"loops":[[1,2,3,4]]},{"id":10,"loops":[[1,3,4]]},{"id":16,"loops":[[1)",
      R"(CC3D:4:2,13,14,15]]},{"id":25,"loops":[[21,22,23]]},{"id":29,"loops":[[21,23,24]]}],"insts":[]}],"guides":[],"dims":[],"scenes":[]})",
      R"(CCCAM:{"eye":[1,2,3],"target":[0,0,0],"persp":true,"fov":40})",
  };
  CHECK(ff.points == want);
  struct F {
    std::array<cc::m3d::Vec3, 4> pts;
    int invis;
  };
  const std::vector<F> want_faces = {
      {{{cc::m3d::Vec3{0.0, 0.0, 0.0}, cc::m3d::Vec3{1000.0, 0.0, 0.0}, cc::m3d::Vec3{1000.0, 1000.0, 0.0},
         cc::m3d::Vec3{0.0, 1000.0, 0.0}}},
       0},
      {{{cc::m3d::Vec3{0.0, 0.0, 0.0}, cc::m3d::Vec3{1000.0, 1000.0, 0.0}, cc::m3d::Vec3{0.0, 1000.0, 0.0},
         cc::m3d::Vec3{0.0, 1000.0, 0.0}}},
       0},
      {{{cc::m3d::Vec3{2200.0, 200.0, 0.0}, cc::m3d::Vec3{2000.0, 1000.0, 0.0}, cc::m3d::Vec3{2000.0, 0.0, 0.0},
         cc::m3d::Vec3{2000.0, 0.0, 0.0}}},
       8},
      {{{cc::m3d::Vec3{2000.0, 0.0, 0.0}, cc::m3d::Vec3{3000.0, 0.0, 0.0}, cc::m3d::Vec3{2200.0, 200.0, 0.0},
         cc::m3d::Vec3{2200.0, 200.0, 0.0}}},
       8},
      {{{cc::m3d::Vec3{0.0, 0.0, 500.0}, cc::m3d::Vec3{1000.0, 0.0, 500.0}, cc::m3d::Vec3{1000.0, 1000.0, 900.0},
         cc::m3d::Vec3{1000.0, 1000.0, 900.0}}},
       0},
      {{{cc::m3d::Vec3{0.0, 0.0, 500.0}, cc::m3d::Vec3{1000.0, 1000.0, 900.0}, cc::m3d::Vec3{0.0, 1000.0, 500.0},
         cc::m3d::Vec3{0.0, 1000.0, 500.0}}},
       0},
  };
  REQUIRE(ff.faces.size() == want_faces.size());
  for (std::size_t i = 0; i < want_faces.size(); ++i) {
    INFO(i);
    CHECK(ff.faces[i].pts == want_faces[i].pts);
    CHECK(ff.faces[i].invis == want_faces[i].invis);
  }
}

TEST_CASE("an empty model writes nothing") {
  const cc::m3d::Model m;
  CHECK_FALSE(cc::m3d::for_file3d(m, "").has_value());
}

// Fuzz target m3d, 2026-10-21 (fuzz/corpus/m3d/regress-00521f74bbbe): a polygon of no points read pts[0] past the end,
// and a closed curve of no points divided by zero.
TEST_CASE("the kernel takes empty polygons and curves without reading past them") {
  cc::m3d::Model m;
  CHECK(cc::m3d::add_polygon(m.root(), std::vector<cc::m3d::Vec3>{}).empty());
  CHECK(cc::m3d::tag_curve(m.root(), std::vector<cc::m3d::Vec3>{}, "circle", true) == "circle:1");
  CHECK(cc::m3d::ngon_pts({0, 0, 0}, {0, 0, 1}, {1, 0, 0}, 1, 0).empty());
}

// Fuzz target m3d, 2026-10-22 (fuzz/corpus/m3d/regress-timeout-section): with no words set, every section plane's
// default name is the same key, and looking for a free one never ended.
TEST_CASE("section planes get a name without the catalog") {
  cc::m3d::set_words(nullptr);
  cc::m3d::Model m;
  (void)m.add_section({0, 0, 0}, {0, 0, 1});
  (void)m.add_section({0, 0, 0}, {0, 0, 1});
  CHECK(m.sections.size() == 2);
}

// Fuzz target m3d, 2026-10-27 (fuzz/corpus/m3d/regress-restore-*): a model holding a placement of a definition that is
// not there cannot be restored from its own snapshot; the failed restore moved the definitions out first and left
// null ones behind, which the next edit dereferenced. Now the model keeps every definition it had.
TEST_CASE("a refused restore leaves the model as it was") {
  cc::m3d::Model m;
  (void)m.place(m.root(), "d99", cc::m3d::ident());  // a placement of nothing
  // the JS's restore throws here too (its load refuses the snapshot); the edit guard reports it
  CHECK_THROWS((void)m.change("x", [](cc::m3d::Model& /*mm*/) {
    return cc::base::Result<void>(cc::base::fail(cc::base::Errc::invalid, "no"));
  }));
  CHECK(m.def("root") != nullptr);
  CHECK(m.root().insts.size() == 1);
}

// Fuzz target m3d, 2026-10-27: a definition placed twice inside each of 32 levels makes 2^32 placements; a walk stops
// after 250,000 of them (docs/gaps.md) instead of running for hours.
TEST_CASE("a walk over an exponential nesting stops at its budget") {
  cc::m3d::Model m;
  std::string inner;
  for (int level = 0; level < 32; ++level) {
    cc::m3d::Def& d = m.new_def("group");
    if (inner.empty()) {
      (void)cc::m3d::v_add(d, {0, 0, 0});
    } else {
      (void)m.place(d, inner, cc::m3d::ident());
      (void)m.place(d, inner, cc::m3d::translation({1, 0, 0}));
    }
    inner = d.id;
  }
  (void)m.place(m.root(), inner, cc::m3d::ident());
  std::size_t visits = 0;
  m.walk([&](const cc::m3d::Def& /*d*/, const cc::m3d::Mat4& /*m*/) { ++visits; });
  CHECK(visits <= 250001);
  CHECK(visits > 1000);
}
