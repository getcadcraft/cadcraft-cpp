// SPDX-License-Identifier: Apache-2.0
// refgeom.js: the reference plane, axis and point features worked out from what they reference, through resolvers
// the caller gives (the mesh kernel's, or the parity driver's lookup tables). Kernel-free.
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"
#include "cc/partcraft/vec.hpp"

namespace cc::partcraft::refgeom {

/// A resolved plane: its frame and, when it has one, the centre the view draws it about.
struct Plane {
  vec::Frame frame;
  std::optional<vec::V3> center;
};
struct Dir {
  vec::V3 o{}, d{};
};
/// A face as the exact kernel describes it: type "plane" (n, o), "cylinder" / "cone" (axis, r), anything else.
struct Face {
  std::string type;
  std::optional<vec::V3> n, o;
  std::optional<Dir> axis;
  double r = 0;
};
/// An edge: type "line" / "circle" (axis) / anything else, from a to b. (The exact kernel's near / mid / cross are not
/// given by any resolver the fork has, so the modes needing them say so, as the JS does without them.)
struct Edge {
  std::string type;
  vec::V3 a{}, b{};
  std::optional<Dir> axis;
};

/// R: each resolver throws (through fail) when it cannot resolve its reference.
struct Resolvers {
  std::function<Plane(const base::Value&)> plane;
  std::function<Dir(const base::Value&)> dir;
  std::function<vec::V3(const base::Value&)> point;
  std::function<Face(const base::Value&)> face;
  std::function<Edge(const base::Value&)> edge;
  std::function<void(const std::string&)> fail;  // throws
};

/// refFeature's result: {kind:'point', p} | {kind:'axis', o, d} | {kind:'plane', frame, center}.
struct Geom {
  std::string kind;
  vec::V3 p{}, o{}, d{};
  vec::Frame frame;
  vec::V3 center{};
};

[[nodiscard]] Geom ref_feature(const base::Value& f, const Resolvers& R, const i18n::Catalog& words);
[[nodiscard]] base::Value to_value(const Geom& g);
/// rotate(v, u, a): v about the unit axis u by a radians (Rodrigues).
[[nodiscard]] vec::V3 rotate(const vec::V3& v, const vec::V3& u, double a);
/// planeQuad(g, s): the corners of a plane's quad (half size s) about its centre.
[[nodiscard]] base::Value plane_quad(const vec::Frame& frame, const vec::V3& center, double s);

}  // namespace cc::partcraft::refgeom
