// SPDX-License-Identifier: Apache-2.0
// cc::cad — CadCraft 0.5's drawing geometry that saving and the commands share: font.js (Helvetica widths),
// geom.js (text layout, entity bounds, attribute texts), attrs.js (world attributes), leader.js (multileaders).
// Ported function for function, with JavaScript's arithmetic order, so a saved file gets the very same doubles.
//
// Entities are JS-shaped values (base::Value) with doc.js's fields; blocks come through a lookup so this library does
// not need the document (cc::doc is a layer above).
#pragma once

#include <array>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/value.hpp"

namespace cc::cad {

/// A block by name (doc.blocks.get), or nullptr.
using BlockLookup = std::function<const base::Value*(const std::string&)>;

/// geom.js FONT_EM: font size for a text height (Helvetica's capital height is 0.718 em).
inline constexpr double kFontEm = 1 / 0.718;
/// geom.js LINE_SPACING: × text height, baseline to baseline.
inline constexpr double kLineSpacing = 5.0 / 3.0;

/// font.js advance: the width of a string in 1/1000 em.
[[nodiscard]] double text_advance(std::string_view s);
/// geom.js textWidth(s, e): a line of e's text in drawing units.
[[nodiscard]] double text_width(std::string_view s, const base::Value& e);

/// geom.js textLayout(e): lines (wrapped to `ww`), their widths and baselines, the box in the text's own frame.
struct TextLayout {
  std::vector<std::string> lines;
  std::vector<double> widths;
  double box_x = 0, box_y = 0, box_w = 0, box_h = 0;
  std::vector<double> baselines;
};
[[nodiscard]] TextLayout text_layout(const base::Value& e);

struct P2 {
  double x = 0, y = 0;
};
/// geom.js textCorners(e): the box's corners in world coordinates, counter-clockwise from the lower left.
[[nodiscard]] std::array<P2, 4> text_corners(const base::Value& e);

/// geom.js bounds: { minX, minY, maxX, maxY }, empty = (+∞, +∞, −∞, −∞).
struct Bounds {
  double min_x = std::numeric_limits<double>::infinity();
  double min_y = std::numeric_limits<double>::infinity();
  double max_x = -std::numeric_limits<double>::infinity();
  double max_y = -std::numeric_limits<double>::infinity();
  [[nodiscard]] bool valid() const noexcept { return min_x <= max_x && min_y <= max_y; }
};
/// geom.js entityBounds(e, blocks).
[[nodiscard]] Bounds entity_bounds(const base::Value& e, const BlockLookup& blocks);

/// geom.js attrTexts(e, blk, all): a reference's attributes as text entities in the block's coordinates.
[[nodiscard]] base::Array attr_texts(const base::Value& e, const base::Value& blk, bool all = false);
/// attrs.js worldAttribs(e, blk): the non-constant attributes as world text entities with `tag` and `invisible`.
[[nodiscard]] base::Array world_attribs(const base::Value& e, const base::Value& blk);

/// attrs.js oneLine(s): runs of \r / \n → one space.
[[nodiscard]] std::string one_line(std::string_view s);

/// leader.js leaderStyle(e): MLEADER_DEFAULTS { asz 2.5, dogleg 2.5, gap 1 } with e.st over it.
[[nodiscard]] base::Value leader_style(const base::Value& e);
/// leader.js landing(e): where the landing starts and ends and where the text starts.
struct Landing {
  P2 start, end, text;
};
[[nodiscard]] Landing landing(const base::Value& e);
/// leader.js leaderGeometry(e): the polyline, the arrowhead (a solid hatch), the text.
[[nodiscard]] base::Array leader_geometry(const base::Value& e);

}  // namespace cc::cad
