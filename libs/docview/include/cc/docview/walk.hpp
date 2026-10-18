// SPDX-License-Identifier: Apache-2.0
// cc::docview — L4. What a CadCraft drawing draws as (CadCraft 0.5's walk.js, the code behind its screen and its
// plots): blocks, dimensions and multileaders expanded, colours resolved (by layer, by block), hidden and frozen layers
// left out, lines cut into the dashes of their linetype, pattern hatches as their lines, polyline widths as filled
// bands.
//
//   cc::docview::walk(doc, entities, out, {.m = …, .plot = false});
//   out.stroke(color, prims, lw, o)  lines and arcs (geom.js primitives), transformed and dashed; lw in mm (-3 default)
//   out.fill(color, loops, solid, o) rings [x, y, x, y, …]
//   out.text(color, e, o)            a text entity, transformed
//   out.point / out.viewport / out.mask / out.infinite
// Colours are "ink" or "#rrggbb". `o` = { alpha (1 − transparency), wipe (paint in the background colour) }.
//
// Not ported yet (docs/gaps.md): pictures (pictures.js, out.picture), clipped references (XCLIP, xclip.js clipOut),
// per-viewport layer overrides (VP.3).
#pragma once

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/geom.hpp"
#include "cc/doc/doc.hpp"

namespace cc::docview {

struct Opts {
  std::optional<double> alpha;  // 0–1; none = opaque
  bool wipe = false;
};

struct Out {
  std::function<void(const std::string& color, const std::vector<cad::Prim>& prims, double lw, const Opts& o)> stroke;
  std::function<void(const std::string& color, const std::vector<std::vector<double>>& loops, bool solid,
                     const Opts& o)>
      fill;
  std::function<void(const std::string& color, const base::Value& e, const Opts& o)> text;
  std::function<void(const std::string& color, double x, double y)> point;
  std::function<void(const base::Value& e)> viewport;
  /// A text's background mask, before its text (absent: it comes as a solid fill).
  std::function<void(const std::string& color, const std::vector<double>& loop, bool wipe)> mask;
  /// An xline / ray after the transform, for the output to clip (absent: its ±FAR stand-in as a stroke).
  std::function<void(const std::string& color, const base::Value& e, double lw, const Opts& o)> infinite;
};

struct WalkOptions {
  std::optional<cad::Mat2> m;    // a transform for everything
  bool plot = false;             // also leave out layers set not to plot
  std::set<std::string> frozen;  // upper-case layer names frozen in the viewport being drawn
};

/// walk.js walk(doc, entities, out, { m, plot, frozen }).
void walk(const doc::Doc& doc, const std::vector<const base::Value*>& entities, const Out& out,
          const WalkOptions& opts = {});

/// walk() over one space as plain events, as tools/parity/walk.mjs records the JS's: ["s", colour, lw, alpha, prims],
/// ["f", colour, solid, alpha, wipe, loops], ["t", colour, alpha, entity], ["p", colour, x, y], ["v", id],
/// ["m", colour, wipe, loop], ["i", colour, lw, alpha, type, x, y, dx, dy]; a primitive is ["L", x1, y1, x2, y2] or
/// ["A", cx, cy, r, a0, a1, full, rev].
[[nodiscard]] base::Value walk_events(const doc::Doc& doc, const doc::EntityMap& space);

/// doc.js resolveColor(doc, e, byBlock, parentLayer): "ink" or "#rrggbb".
[[nodiscard]] std::string resolve_color(const doc::Doc& doc, const base::Value& e, const base::Value& by_block,
                                        const base::Value& parent_layer);
/// linetypes.js resolveLt / resolveLw.
[[nodiscard]] base::Value resolve_lt(const doc::Doc& doc, const base::Value& e, const base::Value& by_block,
                                     const base::Value& parent_layer);
[[nodiscard]] double resolve_lw(const doc::Doc& doc, const base::Value& e, const base::Value& by_block,
                                const base::Value& parent_layer);
/// linetypes.js dashPrims(prims, pattern, scale): the dashes of a pattern along a run of primitives.
[[nodiscard]] std::vector<cad::Prim> dash_prims(const std::vector<cad::Prim>& prims, const std::vector<double>& pattern,
                                                double scale);
/// hatch.js hatchLines(e, doc): a pattern hatch's lines, or nothing (solid, unknown pattern, too dense).
[[nodiscard]] std::optional<std::vector<cad::Prim>> hatch_lines(const base::Value& e, const doc::Doc& doc);

}  // namespace cc::docview
