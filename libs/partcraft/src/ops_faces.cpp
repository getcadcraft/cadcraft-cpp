// SPDX-License-Identifier: Apache-2.0
// The driver's faces.* and mf.* ops (faces.js, meshfaces.js; ops-stage3.mjs).
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/partcraft/meshfaces.hpp"
#include "cc/partcraft/vec.hpp"
#include "ops.hpp"

namespace cc::partcraft::ops {
namespace {

using base::Value;
namespace MF = meshfaces;

/// Int32Array.from(list): ToInt32 of each entry.
std::vector<std::int32_t> int32s(const Value& v) {
  std::vector<std::int32_t> out;
  if (v.is_array()) {
    out.reserve(v.as_array().size());
    for (const Value& x : v.as_array()) {
      const double d = base::js_to_number(x);
      if (!std::isfinite(d)) {
        out.push_back(0);
        continue;
      }
      double m = std::fmod(std::trunc(d), 4294967296.0);
      if (m < 0) {
        m += 4294967296.0;
      }
      out.push_back(static_cast<std::int32_t>(static_cast<std::uint32_t>(m)));
    }
  }
  return out;
}

MF::Loop2 loop2(const Value& v) {
  MF::Loop2 out;
  if (v.is_array()) {
    for (const Value& p : v.as_array()) {
      out.push_back({base::js_to_number(p.is_array() && !p.as_array().empty() ? p.as_array()[0] : Value{}),
                     base::js_to_number(p.is_array() && p.as_array().size() > 1 ? p.as_array()[1] : Value{})});
    }
  }
  return out;
}
Value loop_value(const MF::Loop2& l) {
  base::Array out;
  out.reserve(l.size());
  for (const auto& p : l) {
    out.emplace_back(base::Array{p[0], p[1]});
  }
  return out;
}

}  // namespace

void add_faces(Table& t) {
  t["faces.planarGroupsMesh"] = [](Args a) {
    const Value tol = opt(a, 1);
    const MF::Mesh m = MF::mesh_of(arg(a, 0));
    return MF::groups_value(MF::planar_groups_mesh(m, tol.is_undefined() ? 1e-5 : base::js_to_number(tol)), true);
  };
  t["faces.faceGroups"] = [](Args a) {
    const MF::Mesh m = MF::mesh_of(arg(a, 0));
    if (!m.tri_face && !m.indices.empty()) {
      throw_read(Value{}, "0");
    }
    MF::Mesh mm = m;
    if (!mm.tri_face) {
      mm.tri_face.emplace();
    }
    return MF::groups_value(MF::face_groups(mm), false);
  };
  t["faces.loops"] = [](Args a) {
    const MF::Mesh m = MF::mesh_of(arg(a, 0));
    base::Array out;
    for (const MF::Group& g : MF::planar_groups(m).groups) {
      base::Array raw;
      for (const auto& l : MF::boundary_loops(m, g.tris)) {
        base::Array r;
        r.reserve(l.size());
        for (const std::uint32_t v : l) {
          r.emplace_back(static_cast<double>(v));
        }
        raw.emplace_back(std::move(r));
      }
      const MF::Loops2D l2 = MF::face_loops_2d(m, g);
      base::Array loops;
      for (const auto& l : l2.loops) {
        loops.push_back(loop_value(l));
      }
      out.push_back(base::make_object(
          {{"raw", std::move(raw)}, {"frame", vec::value(l2.frame)}, {"loops", std::move(loops)}, {"area", l2.area}}));
    }
    return Value(std::move(out));
  };
  t["faces.dropCollinear"] = [](Args a) {
    const Value tol = opt(a, 1);
    return loop_value(MF::drop_collinear(loop2(arg(a, 0)), tol.is_undefined() ? 1e-6 : base::js_to_number(tol)));
  };
  t["mf.namedFaces"] = [](Args a) {
    const MF::Mesh m = MF::mesh_of(arg(a, 0));
    const Value seg = opt(a, 3);
    return MF::named_value(
        MF::named_faces(m, int32s(arg(a, 1)), arg(a, 2), seg.is_undefined() ? 64 : base::js_to_number(seg)));
  };
  t["mf.moveSurfs"] = [](Args a) { return MF::move_surfs(arg(a, 0), vec::m4(arg(a, 1)), base::js_string(arg(a, 2))); };
}

}  // namespace cc::partcraft::ops
