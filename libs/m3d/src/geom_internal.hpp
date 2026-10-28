// SPDX-License-Identifier: Apache-2.0
// model.js's private helpers, shared by the kernel's source files.
#pragma once

#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cc/m3d/model.hpp"

namespace cc::m3d::detail {

/// loopSegs(f): every loop segment [a, b].
[[nodiscard]] std::vector<std::pair<Id, Id>> loop_segs(const Face& f);
/// vertFaces(def): vertex → the faces using it, in face order.
[[nodiscard]] std::unordered_map<Id, OrderedSet<Id>> vert_faces(const Def& d);
[[nodiscard]] double area2(std::span<const Vec2> poly);
[[nodiscard]] bool in_poly(const Vec2& pt, std::span<const Vec2> poly);
[[nodiscard]] bool in_region(const Vec2& pt, const std::vector<std::vector<Vec2>>& loops);
/// samplePoint: a point well inside a region (outer + holes), the centroid of its largest triangle.
[[nodiscard]] std::optional<Vec2> sample_point(const std::vector<std::vector<Vec2>>& loops);
[[nodiscard]] std::vector<std::vector<Vec2>> loops2(const Def& d, const Face& f, const Frame& fr);
/// candidatePlanes: the planes an edge could close a face on.
[[nodiscard]] std::vector<Plane> candidate_planes(const Def& d, std::span<const Id> eids);

}  // namespace cc::m3d::detail
