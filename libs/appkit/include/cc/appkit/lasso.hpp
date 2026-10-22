// SPDX-License-Identifier: Apache-2.0
// lasso-core.js (0.5; byte-identical in cadcraft, civilcraft, detailcraft, markupcraft, takeoffcraft): lasso
// selection — window (wholly inside), crossing (inside or touching), fence (what the open path crosses).
#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cc::appkit {

struct LPt {
  double x = 0, y = 0;
};
struct LBox {
  double min_x = 0, min_y = 0, max_x = 0, max_y = 0;
};
using LSeg = std::array<double, 4>;

/// lassoMode(start, cur, minPx): "window" | "crossing" | "" (not yet).
[[nodiscard]] std::string lasso_mode(LPt start, LPt cur, double min_px = 6);
/// nextMode(mode)
[[nodiscard]] std::string next_mode(std::string_view mode);
/// thin(pts, minStep)
[[nodiscard]] std::vector<LPt> thin(const std::vector<LPt>& pts, double min_step);
/// pointInPoly(p, pts): even-odd.
[[nodiscard]] bool point_in_poly(LPt p, const std::vector<LPt>& pts);
/// segsCross: do segments AB and CD touch.
[[nodiscard]] bool segs_cross(double ax, double ay, double bx, double by, double cx, double cy, double dx, double dy);

struct LassoItem {
  std::optional<LBox> box;  // no box: never picked
  std::vector<LSeg> segs;
  bool filled = false;
};
/// lassoSelect(items, pts, mode, …): the indices of the items picked, in order.
[[nodiscard]] std::vector<std::size_t> lasso_select(const std::vector<LassoItem>& items, const std::vector<LPt>& pts,
                                                    std::string_view mode);

}  // namespace cc::appkit
