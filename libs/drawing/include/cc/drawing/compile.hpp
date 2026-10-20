// SPDX-License-Identifier: Apache-2.0
// cc::drawing — L2. A drawing model (cc::dwg::read_dwg, cc::dxf::read_dxf_model) → what the 2D view draws: the C++
// port of BimCraft 0.5's dwg-compile.js, at parity with it (tools/parity/compile.mjs). Typed output, so the renderer
// reads plain arrays; to_json() writes the JS result's shape (Maps as arrays of their values, typed arrays as arrays,
// non-finite numbers as null) for the parity check.
//
//   Drawing d = compile_drawing(model, {.version_tag = "AC1032"});
//   d.spaces  model first, then layouts in tab order; each a list of batches (one layer × colour × line weight):
//             `lines` (flat x0,y0,x1,y1,… polylines), `fills` (rings + pattern flag), `texts`, all in world units
//   d.layers  sorted as the JS sorts them (localeCompare, numeric)
//
// Block inserts are expanded with their transforms; layer "0" and BYBLOCK colour inherit from the insert; linked
// drawings (xrefs) are drawn from their own models when passed in. Unsupported entity types are counted, never guessed.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/value.hpp"

namespace cc::drawing {

/// An affine map [a, b, c, d, e, f]: x' = a x + c y + e, y' = b x + d y + f.
using Affine = std::array<double, 6>;

struct Bounds {
  double min_x = std::numeric_limits<double>::infinity();
  double min_y = std::numeric_limits<double>::infinity();
  double max_x = -std::numeric_limits<double>::infinity();
  double max_y = -std::numeric_limits<double>::infinity();
};

struct Text {
  double x = 0;
  double y = 0;
  double h = 0;
  double rot = 0;
  std::string s;
  std::optional<double> ha;  // 0 left, 1 centre, 2 right (absent where the JS gives undefined)
  double va = 0;             // 0 baseline, 1 bottom, 2 middle, 3 top
  bool mirror = false;
};

struct Fill {
  std::vector<std::vector<double>> rings;
  bool pattern = false;
};

struct Batch {
  std::string layer;
  std::string color;  // "ink" (ACI 7: black on paper, white on a dark screen) or "#rrggbb"
  double lw = 0;      // mm
  std::vector<std::vector<double>> lines;
  std::vector<Fill> fills;
  std::vector<Text> texts;
};

struct Viewport {
  double cx = 0, cy = 0, w = 0, h = 0, vx = 0, vy = 0, view_height = 0, twist = 0;
  std::vector<std::string> frozen;  // layer handles
};

struct Picture {
  Affine m{};  // pixel (x right, y down) → world
  double w = 0;
  double h = 0;
  std::string ref;
  std::string layer;
};

/// A picture's pixels (0.5 drawPictures, from walk.js out.picture): its file, one pixel along its width (u) and up (v)
/// from its lower-left corner, its size in pixels, its frame (the clip: x, y pairs) and its alpha.
struct Bitmap {
  std::string file;
  double x = 0, y = 0, ux = 0, uy = 0, vx = 0, vy = 0, w = 0, h = 0;
  std::vector<double> clip;
  double alpha = 1;
};

struct Space {
  std::string key;
  std::string name;
  std::string kind;  // "model" | "paper"
  std::vector<Batch> batches;
  Bounds bounds;
  std::optional<Bounds> paper;  // paper spaces only (null when the layout has no limits)
  std::string paper_unit;       // paper spaces only: "mm" | "in"
  std::vector<Viewport> viewports;
  std::vector<Picture> images;
  std::vector<Bitmap> bitmaps;  // documents only (their files are with the document)
};

struct Layer {
  std::string name;
  std::string color;
  bool on = true;
  double lw = 0.25;
  bool plot = true;
};

struct ImageRef {
  std::string ref;
  std::string name;
  double uses = 0;
};

struct Stats {
  double entities = 0;
  std::vector<std::pair<std::string, double>> unsupported;  // in first-seen order
  bool truncated = false;
  double prims = 0;
  double xref_entities = 0;
};

struct Drawing {
  std::string version;
  std::string release;
  std::string units;
  std::vector<std::pair<std::string, bool>> xrefs;  // name, loaded
  std::vector<ImageRef> images;
  std::vector<Layer> layers;
  std::vector<Space> spaces;
  Stats stats;
};

struct Xref {
  std::string key;                     // the xref block name in lower case
  const base::Value* model = nullptr;  // its read model (must outlive compile_drawing)
};

struct CompileOptions {
  std::string version_tag;  // the DWG's first six bytes ("AC1032"); empty for DXF
  std::vector<Xref> xrefs;
  /// Not in the JS (which runs out of memory or time instead): caps on the coordinates kept (default 256 MB of
  /// doubles) and the entities visited, far beyond any real drawing; past them the drawing is marked truncated.
  double max_coords = 32'000'000;
  double max_visits = 20'000'000;
};

[[nodiscard]] Drawing compile_drawing(const base::Value& model, const CompileOptions& options = {});

/// The JS result as plain JSON (cdiff.mjs's plain(): Maps → value arrays, Sets and typed arrays → arrays, then a JSON
/// round trip, so ±Infinity and NaN are null).
void write_json(const Drawing& d, std::string& out);

/// AutoCAD Color Index → "ink" or "#rrggbb" (dwg-compile.js aciColor).
[[nodiscard]] std::string aci_color(double index);

}  // namespace cc::drawing
