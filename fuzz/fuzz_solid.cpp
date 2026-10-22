// SPDX-License-Identifier: Apache-2.0
// SolidOps on hostile meshes: any bytes give a solid or a status — never a crash, a hang or a sanitizer report.
// Input starting with '[' is a case of tools/parity/manifold.mjs (JSON steps, cases.hpp); anything else is a small
// mesh: byte 0 the vertex count, then three signed bytes per vertex (quarter units), then triangles as byte triples
// (each index taken modulo the count, so most meshes are wrong in some way). A mesh that makes a solid is cut by a
// cube, joined with a moved copy of itself and split by a plane, and the results are meshed.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "cc/base/json.hpp"
#include "cc/solid/cases.hpp"
#include "cc/solid/solid_ops.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0 || size > (std::size_t{1} << 16)) {
    return 0;
  }
  const std::span<const std::uint8_t> in(data, size);
  if (in[0] == '[') {
    const std::string_view text(reinterpret_cast<const char*>(data), size);  // NOLINT
    cc::base::JsonLimits lim;
    lim.max_depth = 16;
    if (const auto steps = cc::base::parse_json(text, lim)) {
      (void)cc::solid::run_case(*steps);
    }
    return 0;
  }
  const std::size_t nv = in[0];
  if (nv == 0 || size < 1 + nv * 3) {
    return 0;
  }
  cc::solid::Mesh m;
  for (std::size_t i = 0; i < nv * 3; ++i) {
    m.vert_properties.push_back(static_cast<float>(static_cast<std::int8_t>(in[1 + i])) / 4.0F);
  }
  for (std::size_t i = 1 + nv * 3; i + 2 < size && m.tri_verts.size() < 3 * 512; i += 3) {
    for (std::size_t k = 0; k < 3; ++k) {
      m.tri_verts.push_back(static_cast<std::uint32_t>(in[i + k] % (nv + 1)));  // nv itself: out of bounds
    }
  }
  (void)cc::solid::merge(m);
  const auto s = cc::solid::Solid::of_mesh(m);
  if (!s) {
    return 0;
  }
  const auto cube = cc::solid::Solid::cube({3, 3, 3}, true);
  if (!cube) {
    return 0;
  }
  if (const auto d = s->subtract(*cube)) {
    (void)d->mesh();
  }
  if (const auto moved = s->translate({0.25, 0.5, 0.75})) {
    if (const auto u = s->add(*moved)) {
      (void)u->mesh();
      if (const auto parts = u->split_by_plane({0, 0, 1}, 0.1)) {
        (void)parts->first.mesh();
        (void)parts->second.mesh();
      }
    }
  }
  (void)s->volume();
  return 0;
}
