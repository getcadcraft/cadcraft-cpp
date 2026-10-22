// SPDX-License-Identifier: Apache-2.0
// SolidOps against manifold-3d 3.5.4, the WebAssembly CadCraft 0.5 runs: the cases of tools/parity/manifold.mjs with
// the wasm's results (tests/golden/js-0.5-manifold.json), every float compared as its bits.
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/value.hpp"
#include "cc/solid/cases.hpp"
#include "cc/solid/solid_ops.hpp"

using cc::base::Value;

TEST_CASE("SolidOps gives the manifold-3d 3.5.4 wasm's results bit for bit") {
  const auto bytes = cc::base::read_file(std::string(CC_GOLDEN_DIR) + "/js-0.5-manifold.json", std::size_t{1} << 26U);
  REQUIRE(bytes);
  const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());  // NOLINT
  const auto golden = cc::base::parse_json(text);
  REQUIRE(golden);
  const auto& cases = (*golden)["cases"].as_array();
  REQUIRE(cases.size() >= 20);
  std::size_t same = 0;
  std::size_t refused = 0;
  for (const Value& c : cases) {
    const Value got = cc::solid::run_case(c["steps"]);
    const Value& want = c["want"];
    if (want.has("error")) {
      ++refused;
    }
    if (cc::base::to_json(got) == cc::base::to_json(want)) {
      ++same;
    } else {
      UNSCOPED_INFO("case " << cc::base::to_json(c["name"]) << " seed " << cc::base::to_json(c["seed"]));
    }
  }
  CHECK(same == cases.size());
  CHECK(refused >= 1);  // the open mesh: refused as the wasm refuses it
}

TEST_CASE("SolidOps: two overlapping unit cubes") {
  const auto a = cc::solid::Solid::cube({1, 1, 1}, false);
  REQUIRE(a);
  const auto b = a->translate({0.5, 0, 0});
  REQUIRE(b);
  const auto u = a->add(*b);
  REQUIRE(u);
  CHECK(u->volume() == 1.5);
  const auto d = a->subtract(*b);
  REQUIRE(d);
  CHECK(d->volume() == 0.5);
  const std::vector<cc::solid::Solid> both = {*a, *b};
  const auto i = cc::solid::intersection_of(both);
  REQUIRE(i);
  CHECK(i->volume() == 0.5);
  CHECK(i->status() == "NoError");
}

TEST_CASE("SolidOps refuses what is not a closed manifold") {
  cc::solid::Mesh m;
  m.vert_properties = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  m.tri_verts = {0, 1, 2};
  const auto s = cc::solid::Solid::of_mesh(m);
  REQUIRE_FALSE(s);
  CHECK(s.error().message == "NotManifold");
  m.tri_verts = {0, 1, 7};
  CHECK_FALSE(cc::solid::Solid::of_mesh(m));  // an index past the vertices
  m.vert_properties = {0, 0, 0, 1, 0, 0, 0, 1};
  CHECK_FALSE(cc::solid::Solid::of_mesh(m));
}

TEST_CASE("the case runner refuses malformed steps") {
  const auto steps = cc::base::parse_json(R"([{"op":"add"}])");
  REQUIRE(steps);
  CHECK(cc::solid::run_case(*steps).has("bad"));
  const auto huge = cc::base::parse_json(R"([{"sphere":[1, 1000000]}])");
  REQUIRE(huge);
  CHECK(cc::solid::run_case(*huge).has("bad"));
}

// Fuzz target solid, 2026-10-21 (fuzz/corpus/solid/regress-*): Merge read the vertices of a triangle index past them
// (the libc++ hardening trap; the wasm reads arbitrary memory), and the constructor would have read past the runs.
TEST_CASE("SolidOps: hostile indices never reach Manifold's unchecked reads") {
  cc::solid::Mesh m;
  m.vert_properties = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
  m.tri_verts = {0, 1, 2, 0, 3, 1, 1, 3, 2, 2, 3, 9};  // 9: past the four vertices
  CHECK_FALSE(cc::solid::merge(m));
  CHECK(m.tri_verts.back() == 9);  // left as it was
  CHECK(cc::solid::Solid::of_mesh(m).error().message == "VertexOutOfBounds");
  m.tri_verts.back() = 0;
  m.merge_from_vert = {1};
  CHECK_FALSE(cc::solid::merge(m));  // merge vectors of different lengths
  m.merge_to_vert = {7};
  CHECK_FALSE(cc::solid::merge(m));  // a merge index past the vertices
  m.merge_from_vert.clear();
  m.merge_to_vert.clear();
  m.run_index = {0, 99};
  CHECK(cc::solid::Solid::of_mesh(m).error().message == "RunIndexWrongLength");
  m.run_index.clear();
  m.num_prop = 0;
  CHECK(cc::solid::Solid::of_mesh(m).error().message == "MissingPositionProperties");
  CHECK_FALSE(cc::solid::merge(m));
}

// Fuzz target solid, 2026-10-21 (fuzz/corpus/solid/regress-19e83f62960c): edges that do not pair up made Manifold's
// CreateHalfedges pair unrelated halfedges and its manifold check read past them. Refused as not a manifold first.
TEST_CASE("SolidOps: a mesh whose edges do not pair up is not a manifold") {
  cc::solid::Mesh m;
  m.vert_properties = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1};
  m.tri_verts = {0, 1, 2, 0, 3, 1, 1, 3, 2, 2, 3, 0, 1, 4, 2};  // a tetrahedron and one more triangle
  CHECK(cc::solid::Solid::of_mesh(m).error().message == "NotManifold");
  m.tri_verts.resize(12);  // the tetrahedron alone is a solid
  m.vert_properties.resize(12);
  const auto s = cc::solid::Solid::of_mesh(m);
  REQUIRE(s);
  CHECK(std::abs(s->volume()) > 0.16);  // inside out as wound here: Manifold keeps the sign
}
