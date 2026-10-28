// SPDX-License-Identifier: Apache-2.0
// cc::partcraft::threads — PartCraft 0.5's threads.js (WL.29), the part that is not the mesh kernel: the ISO 261
// coarse metric and ISO 2904 trapezoidal tables, the basic profiles (ISO 68-1, ISO 2901), the size that fits a round
// face, the helical solid's cross-section square to the axis, and the callouts. The solid itself (boltSolid,
// threadTool) waits for the mesh kernel (W2's SolidOps).
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/error.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::partcraft::threads {

/// The basic profile's diameters: major d, minor d1, pitch d2, the pitch p and the thread depth.
struct Dims {
  double d, p, d1, d2, depth;
};
/// The profile over one pitch as (u, r) corners, crest first.
using Corners = std::vector<std::array<double, 2>>;

/// threadDims(form, size): nothing for an unknown form or size; an error where the JS throws (a size that is an
/// Object.prototype name: "row is not iterable").
[[nodiscard]] base::Result<std::optional<Dims>> thread_dims(std::string_view form, std::string_view size);
/// The table's sizes in order (ISO_METRIC / ISO_TRAPEZOIDAL keys).
[[nodiscard]] std::vector<std::string_view> sizes(std::string_view form);
/// sizeLabel(form, size): "M10", "Tr16×4", "Tr8×1.5".
[[nodiscard]] std::string size_label(std::string_view form, std::string_view size);
/// sizeFor(form, dia, external): the size whose major (a shaft) or tap-drill (a bore) diameter is nearest, within 12 %.
[[nodiscard]] std::optional<std::string_view> size_for(std::string_view form, double dia, bool external);
/// profileCorners(form, size, shift): the profile moved radially by `shift`.
[[nodiscard]] base::Result<std::optional<Corners>> profile_corners(std::string_view form, std::string_view size,
                                                                   double shift = 0);
/// profileAt(corners, u): the profile's radius at u (periodic in the pitch).
[[nodiscard]] double profile_at(const Corners& corners, double u);
/// boltSection(corners, {hand, segments}): the cross-section square to the axis as one CCW polygon.
[[nodiscard]] std::vector<std::array<double, 2>> bolt_section(const Corners& corners, bool left = false,
                                                              double segments = 96);

struct CalloutOptions {
  bool external = false;
  double length = 0;
  bool through = false;
  bool modelled = false;
};
/// threadCallout(form, size, {external, length, through, modelled}): "M10×1.5 - 6g, thread 20 long" ("" when unknown).
[[nodiscard]] base::Result<std::string> thread_callout(std::string_view form, std::string_view size,
                                                       const CalloutOptions& o, const i18n::Catalog& words);

}  // namespace cc::partcraft::threads
