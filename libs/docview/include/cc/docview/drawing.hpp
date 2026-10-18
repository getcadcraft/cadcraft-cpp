// SPDX-License-Identifier: Apache-2.0
// cc::docview — a CadCraft document as the renderer's draw lists (cc::drawing::Drawing): walk() per space (the model,
// then each sheet with its viewports), strokes as polylines, fills as rings, each text line as one text. The viewer
// builds it again after every change of the document (an edit, an undo, a layer turned off).
#pragma once

#include "cc/doc/doc.hpp"
#include "cc/drawing/compile.hpp"

namespace cc::docview {

struct DrawingOptions {
  double arc_steps = 128;  // line steps for a whole turn of an arc (fewer for a part)
};

[[nodiscard]] drawing::Drawing to_drawing(const doc::Doc& d, const DrawingOptions& o = {});

}  // namespace cc::docview
