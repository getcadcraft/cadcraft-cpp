// SPDX-License-Identifier: Apache-2.0
// swsketch.js (CadCraft 0.5): a drawing to and from SolidWorks as one sketch. SolidWorks is not available here: nothing
// written here has been run in SolidWorks (the JS tests check the macro's structure and its API calls).
//
// Forward — to_swb: a plain-text VBA macro (.swb; Tools › Macro › Run in SolidWorks) that makes a new part, sets its
// length unit to the drawing's, opens a sketch on the Front Plane and draws the model's lines, circles, arcs, points
// and polylines, then adds the geometric constraints as sketch relations, the dimensional ones as named dimensions, the
// user parameters as global variables and every expression as an equation. Coordinates in metres (the API's unit).
//
// Backward — read_export / load_sketch: a "cadcraft-sw-export" v1 JSON (written by the SolidWorks-side macro) is
// untrusted input, checked and capped; one of its sketches becomes the drawing (model space, millimetres) with its
// relations as constraints, dimensions as dimensional constraints, global variables as parameters and equations on its
// dimensions as their expressions.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/doc/doc.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::doc::swsketch {

struct Counts {
  int lines = 0, arcs = 0, circles = 0, points = 0, relations = 0, dimensions = 0, globals = 0, equations = 0;
};
struct Swb {
  bool ok = false;
  std::string error;
  std::string text;              // the macro (CRLF)
  std::vector<std::byte> bytes;  // as written: Windows-1252, anything else "?"
  std::vector<std::string> notes;
  std::string summary;
  Counts counts;
};
/// docToSwb(doc, { name }): `date` is the day written in the macro's first line (YYYY-MM-DD).
[[nodiscard]] Swb to_swb(const Doc& doc, std::string_view name, std::string_view date, const i18n::Catalog& words);

/// swExpr(src, quote): an expression in SolidWorks' equation form, or why it has none.
struct SwExpr {
  bool ok = false;
  std::string text, error;
};
[[nodiscard]] SwExpr sw_expr(std::string_view src, const std::function<std::string(const std::string&)>& quote,
                             const i18n::Catalog& words);

/// `s.replace(/[^\w.-]+/g, '_')`: a file name's safe characters
[[nodiscard]] std::string safe_chars(std::string_view s);

struct Export {
  bool ok = false;
  std::string error;
  std::vector<std::string> sketches;  // their names, in the file's order
};
/// readSwExport(raw): untrusted export JSON (text, or already parsed)
[[nodiscard]] Export read_export_text(std::string_view raw, const i18n::Catalog& words);
[[nodiscard]] Export read_export(const base::Value& j, const i18n::Catalog& words);

struct Loaded {
  bool ok = false;
  std::string error, sketch;
  std::vector<std::string> notes;
  std::size_t read = 0, constraints = 0;
};
/// loadSwSketch(doc, exp, sketchName): that sketch (or the first) becomes the drawing; not undoable (opening a file)
[[nodiscard]] Loaded load_sketch(Doc& doc, const base::Value& exp, std::string_view sketch_name,
                                 const i18n::Catalog& words);

}  // namespace cc::doc::swsketch
