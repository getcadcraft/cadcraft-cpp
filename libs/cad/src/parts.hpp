// SPDX-License-Identifier: Apache-2.0
// cc::cad internals: the small CadCraft 0.5 modules dwg-import.js calls — attrs.js (attdef, resolveAttribs),
// sheets.js (matchPaper), panes.js + dwg-vports.js (vportsFrom), pictures.js (pictureBlock), xclip.js (clipFromFilter),
// model3d.js (face3dFromDwg, read3dFromDwg, split3d), gcons.js (constraint tags) and dyn-blocks.js (reading side).
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "cc/i18n/i18n.hpp"
#include "util.hpp"

namespace cc::cad::detail {

// ---- attrs.js
struct AttdefFlags {
  Value tag;
  std::string prompt;
  std::string def;
  bool invisible = false, constant = false, preset = false;
};
/// attdef(t, …): an attribute definition from a text entity (block coordinates).
Value attdef(const Value& t, const AttdefFlags& f);
/// resolveAttribs(lists, blocks): the `_attribs` readers left on references → attrs (+ attloc). `blocks` = the block
/// list (a Map by name: a later block of the same name wins).
void resolve_attribs(const std::vector<Array*>& lists, const Array& blocks);

// ---- sheets.js
/// matchPaper(w, h, unit) → the standard paper's key, or "" when none matches within 0.5 %.
std::string match_paper(double w, double h, std::string_view unit);

// ---- panes.js / dwg-vports.js
/// vportsFrom(records): the *Active VPORT records → panes.js shape, or null.
Value vports_from(const Array& records);

// ---- pictures.js / xclip.js
Value picture_block(const std::string& name, const Value& pic, const Value* clip);
Value clip_from_filter(const Value& o);

// ---- model3d.js
Value face3d_from_dwg(const Value& e, const Value& base);
/// read3dFromDwg(db, drawing): CadCraft's 3D JSON from block CADCRAFT_3D, then split3d (3D faces out of the 2D
/// drawing). Notes through `words`.
void read3d_from_dwg(const Value& db, Value& drawing, const i18n::Catalog& words);

// ---- gcons.js
/// tagFromXdata(conv, xdata): a converted entity (not a list) tagged with CadCraft's CCREF / CCPARAM xdata.
void tag_from_xdata(Value& conv, const Value& xdata);
/// parametricText(ents) for the CADCRAFT_PARAMETRIC block.
std::string parametric_text(const Array& ents);
bool is_param_block(const Value& name);
bool is_m3d_block(const Value& name);

// ---- dyn-blocks.js
/// hexKey: upper case, leading zeros dropped (one kept); "" for undefined / null.
std::string hex_key(const Value& h);
struct DynHooks {
  std::function<Value(const Value&)> convert;    // dwg-import's converter (an entity, a list or null)
  std::function<Value(const Value&)> attdef_of;  // its ATTDEF reader (a definition or null)
  Value def_name;                                // the definition's name for a copy, or undefined
  const Value* record = nullptr;                 // the block's BLOCK_RECORD row, or nullptr
};
/// dynBlockFromDwg: a dynamic file block → { entities, attdefs, dyn }, or undefined for a plain block.
Value dyn_block_from_dwg(const Value& b, const DynHooks& hooks);
Value dyn_ref_from_dwg(const Value& e);
Value dyn_source(const Value& db);

}  // namespace cc::cad::detail
