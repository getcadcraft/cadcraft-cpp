// SPDX-License-Identifier: Apache-2.0
// Port of cc-dwg header-vars.js: the AcDb:Header variables in file order (ODA spec v5.4.1 ch. 9). The table is
// generated from the JS module (one row per JS row, same order) so the two never drift.
#include <array>
#include <string_view>

#include "cc/base/checked.hpp"
#include "messages.hpp"
#include "record.hpp"

namespace cc::dwg {

namespace {

struct Row {
  std::string_view name;
  std::string_view type;
  int from;
  int to;
};

constexpr auto kRows = std::to_array<Row>({
    {"REQUIREDVERSIONS", "BLL", 27, 99},
    {"UNKNOWN_1", "BD", 12, 99},
    {"UNKNOWN_2", "BD", 12, 99},
    {"UNKNOWN_3", "BD", 12, 99},
    {"UNKNOWN_4", "BD", 12, 99},
    {"UNKNOWN_5", "TV", 12, 99},
    {"UNKNOWN_6", "TV", 12, 99},
    {"UNKNOWN_7", "TV", 12, 99},
    {"UNKNOWN_8", "TV", 12, 99},
    {"UNKNOWN_9", "BL", 12, 99},
    {"UNKNOWN_10", "BL", 12, 99},
    {"UNKNOWN_11", "BS", 12, 14},
    {"CURRENT_VIEWPORT_ENTITY_HEADER", "H", 12, 15},
    {"DIMASO", "B", 12, 99},
    {"DIMSHO", "B", 12, 99},
    {"DIMSAV", "B", 12, 14},
    {"PLINEGEN", "B", 12, 99},
    {"ORTHOMODE", "B", 12, 99},
    {"REGENMODE", "B", 12, 99},
    {"FILLMODE", "B", 12, 99},
    {"QTEXTMODE", "B", 12, 99},
    {"PSLTSCALE", "B", 12, 99},
    {"LIMCHECK", "B", 12, 99},
    {"BLIPMODE", "B", 12, 14},
    {"UNKNOWN_12", "B", 18, 99},
    {"USRTIMER", "B", 12, 99},
    {"SKPOLY", "B", 12, 99},
    {"ANGDIR", "B", 12, 99},
    {"SPLFRAME", "B", 12, 99},
    {"ATTREQ", "B", 12, 14},
    {"ATTDIA", "B", 12, 14},
    {"MIRRTEXT", "B", 12, 99},
    {"WORLDVIEW", "B", 12, 99},
    {"WIREFRAME", "B", 12, 14},
    {"TILEMODE", "B", 12, 99},
    {"PLIMCHECK", "B", 12, 99},
    {"VISRETAIN", "B", 12, 99},
    {"DELOBJ", "B", 12, 14},
    {"DISPSILH", "B", 12, 99},
    {"PELLIPSE", "B", 12, 99},
    {"PROXYGRAPHICS", "BS", 12, 99},
    {"DRAGMODE", "BS", 12, 14},
    {"TREEDEPTH", "BS", 12, 99},
    {"LUNITS", "BS", 12, 99},
    {"LUPREC", "BS", 12, 99},
    {"AUNITS", "BS", 12, 99},
    {"AUPREC", "BS", 12, 99},
    {"OSMODE", "BS", 12, 14},
    {"ATTMODE", "BS", 12, 99},
    {"COORDS", "BS", 12, 14},
    {"PDMODE", "BS", 12, 99},
    {"PICKSTYLE", "BS", 12, 14},
    {"UNKNOWN_13", "BL", 18, 99},
    {"UNKNOWN_14", "BL", 18, 99},
    {"UNKNOWN_15", "BL", 18, 99},
    {"USERI1", "BS", 12, 99},
    {"USERI2", "BS", 12, 99},
    {"USERI3", "BS", 12, 99},
    {"USERI4", "BS", 12, 99},
    {"USERI5", "BS", 12, 99},
    {"SPLINESEGS", "BS", 12, 99},
    {"SURFU", "BS", 12, 99},
    {"SURFV", "BS", 12, 99},
    {"SURFTYPE", "BS", 12, 99},
    {"SURFTAB1", "BS", 12, 99},
    {"SURFTAB2", "BS", 12, 99},
    {"SPLINETYPE", "BS", 12, 99},
    {"SHADEDGE", "BS", 12, 99},
    {"SHADEDIF", "BS", 12, 99},
    {"UNITMODE", "BS", 12, 99},
    {"MAXACTVP", "BS", 12, 99},
    {"ISOLINES", "BS", 12, 99},
    {"CMLJUST", "BS", 12, 99},
    {"TEXTQLTY", "BS", 12, 99},
    {"LTSCALE", "BD", 12, 99},
    {"TEXTSIZE", "BD", 12, 99},
    {"TRACEWID", "BD", 12, 99},
    {"SKETCHINC", "BD", 12, 99},
    {"FILLETRAD", "BD", 12, 99},
    {"THICKNESS", "BD", 12, 99},
    {"ANGBASE", "BD", 12, 99},
    {"PDSIZE", "BD", 12, 99},
    {"PLINEWID", "BD", 12, 99},
    {"USERR1", "BD", 12, 99},
    {"USERR2", "BD", 12, 99},
    {"USERR3", "BD", 12, 99},
    {"USERR4", "BD", 12, 99},
    {"USERR5", "BD", 12, 99},
    {"CHAMFERA", "BD", 12, 99},
    {"CHAMFERB", "BD", 12, 99},
    {"CHAMFERC", "BD", 12, 99},
    {"CHAMFERD", "BD", 12, 99},
    {"FACETRES", "BD", 12, 99},
    {"CMLSCALE", "BD", 12, 99},
    {"CELTSCALE", "BD", 12, 99},
    {"MENU", "TV", 12, 99},
    {"TDCREATE", "TD", 12, 99},
    {"TDUPDATE", "TD", 12, 99},
    {"UNKNOWN_16", "BL", 18, 99},
    {"UNKNOWN_17", "BL", 18, 99},
    {"UNKNOWN_18", "BL", 18, 99},
    {"TDINDWG", "TS", 12, 99},
    {"TDUSRTIMER", "TS", 12, 99},
    {"CECOLOR", "CMC", 12, 99},
    {"HANDSEED", "HS", 12, 99},
    {"CLAYER", "H", 12, 99},
    {"TEXTSTYLE", "H", 12, 99},
    {"CELTYPE", "H", 12, 99},
    {"CMATERIAL", "H", 21, 99},
    {"DIMSTYLE", "H", 12, 99},
    {"CMLSTYLE", "H", 12, 99},
    {"PSVPSCALE", "BD", 15, 99},
    {"PINSBASE", "3BD", 12, 99},
    {"PEXTMIN", "3BD", 12, 99},
    {"PEXTMAX", "3BD", 12, 99},
    {"PLIMMIN", "2RD", 12, 99},
    {"PLIMMAX", "2RD", 12, 99},
    {"PELEVATION", "BD", 12, 99},
    {"PUCSORG", "3BD", 12, 99},
    {"PUCSXDIR", "3BD", 12, 99},
    {"PUCSYDIR", "3BD", 12, 99},
    {"PUCSNAME", "H", 12, 99},
    {"PUCSORTHOREF", "H", 15, 99},
    {"PUCSORTHOVIEW", "BS", 15, 99},
    {"PUCSBASE", "H", 15, 99},
    {"PUCSORGTOP", "3BD", 15, 99},
    {"PUCSORGBOTTOM", "3BD", 15, 99},
    {"PUCSORGLEFT", "3BD", 15, 99},
    {"PUCSORGRIGHT", "3BD", 15, 99},
    {"PUCSORGFRONT", "3BD", 15, 99},
    {"PUCSORGBACK", "3BD", 15, 99},
    {"INSBASE", "3BD", 12, 99},
    {"EXTMIN", "3BD", 12, 99},
    {"EXTMAX", "3BD", 12, 99},
    {"LIMMIN", "2RD", 12, 99},
    {"LIMMAX", "2RD", 12, 99},
    {"ELEVATION", "BD", 12, 99},
    {"UCSORG", "3BD", 12, 99},
    {"UCSXDIR", "3BD", 12, 99},
    {"UCSYDIR", "3BD", 12, 99},
    {"UCSNAME", "H", 12, 99},
    {"UCSORTHOREF", "H", 15, 99},
    {"UCSORTHOVIEW", "BS", 15, 99},
    {"UCSBASE", "H", 15, 99},
    {"UCSORGTOP", "3BD", 15, 99},
    {"UCSORGBOTTOM", "3BD", 15, 99},
    {"UCSORGLEFT", "3BD", 15, 99},
    {"UCSORGRIGHT", "3BD", 15, 99},
    {"UCSORGFRONT", "3BD", 15, 99},
    {"UCSORGBACK", "3BD", 15, 99},
    {"DIMPOST", "TV", 15, 99},
    {"DIMAPOST", "TV", 15, 99},
    {"DIMTOL", "B", 12, 14},
    {"DIMLIM", "B", 12, 14},
    {"DIMTIH", "B", 12, 14},
    {"DIMTOH", "B", 12, 14},
    {"DIMSE1", "B", 12, 14},
    {"DIMSE2", "B", 12, 14},
    {"DIMALT", "B", 12, 14},
    {"DIMTOFL", "B", 12, 14},
    {"DIMSAH", "B", 12, 14},
    {"DIMTIX", "B", 12, 14},
    {"DIMSOXD", "B", 12, 14},
    {"DIMALTD", "RC", 12, 14},
    {"DIMZIN", "RC", 12, 14},
    {"DIMSD1", "B", 12, 14},
    {"DIMSD2", "B", 12, 14},
    {"DIMTOLJ", "RC", 12, 14},
    {"DIMJUST", "RC", 12, 14},
    {"DIMFIT", "RC", 12, 14},
    {"DIMUPT", "B", 12, 14},
    {"DIMTZIN", "RC", 12, 14},
    {"DIMALTZ", "RC", 12, 14},
    {"DIMALTTZ", "RC", 12, 14},
    {"DIMTAD", "RC", 12, 14},
    {"DIMUNIT", "BS", 12, 14},
    {"DIMAUNIT", "BS", 12, 14},
    {"DIMDEC", "BS", 12, 14},
    {"DIMTDEC", "BS", 12, 14},
    {"DIMALTU", "BS", 12, 14},
    {"DIMALTTD", "BS", 12, 14},
    {"DIMTXSTY", "H", 12, 14},
    {"DIMSCALE", "BD", 12, 99},
    {"DIMASZ", "BD", 12, 99},
    {"DIMEXO", "BD", 12, 99},
    {"DIMDLI", "BD", 12, 99},
    {"DIMEXE", "BD", 12, 99},
    {"DIMRND", "BD", 12, 99},
    {"DIMDLE", "BD", 12, 99},
    {"DIMTP", "BD", 12, 99},
    {"DIMTM", "BD", 12, 99},
    {"DIMFXL", "BD", 21, 99},
    {"DIMJOGANG", "BD", 21, 99},
    {"DIMTFILL", "BS", 21, 99},
    {"DIMTFILLCLR", "CMC", 21, 99},
    {"DIMTOL", "B", 15, 99},
    {"DIMLIM", "B", 15, 99},
    {"DIMTIH", "B", 15, 99},
    {"DIMTOH", "B", 15, 99},
    {"DIMSE1", "B", 15, 99},
    {"DIMSE2", "B", 15, 99},
    {"DIMTAD", "BS", 15, 99},
    {"DIMZIN", "BS", 15, 99},
    {"DIMAZIN", "BS", 15, 99},
    {"DIMARCSYM", "BS", 21, 99},
    {"DIMTXT", "BD", 12, 99},
    {"DIMCEN", "BD", 12, 99},
    {"DIMTSZ", "BD", 12, 99},
    {"DIMALTF", "BD", 12, 99},
    {"DIMLFAC", "BD", 12, 99},
    {"DIMTVP", "BD", 12, 99},
    {"DIMTFAC", "BD", 12, 99},
    {"DIMGAP", "BD", 12, 99},
    {"DIMPOST", "T", 12, 14},
    {"DIMAPOST", "T", 12, 14},
    {"DIMBLK_NAME", "T", 12, 14},
    {"DIMBLK1_NAME", "T", 12, 14},
    {"DIMBLK2_NAME", "T", 12, 14},
    {"DIMALTRND", "BD", 15, 99},
    {"DIMALT", "B", 15, 99},
    {"DIMALTD", "BS", 15, 99},
    {"DIMTOFL", "B", 15, 99},
    {"DIMSAH", "B", 15, 99},
    {"DIMTIX", "B", 15, 99},
    {"DIMSOXD", "B", 15, 99},
    {"DIMCLRD", "CMC", 12, 99},
    {"DIMCLRE", "CMC", 12, 99},
    {"DIMCLRT", "CMC", 12, 99},
    {"DIMADEC", "BS", 15, 99},
    {"DIMDEC", "BS", 15, 99},
    {"DIMTDEC", "BS", 15, 99},
    {"DIMALTU", "BS", 15, 99},
    {"DIMALTTD", "BS", 15, 99},
    {"DIMAUNIT", "BS", 15, 99},
    {"DIMFRAC", "BS", 15, 99},
    {"DIMLUNIT", "BS", 15, 99},
    {"DIMDSEP", "BS", 15, 99},
    {"DIMTMOVE", "BS", 15, 99},
    {"DIMJUST", "BS", 15, 99},
    {"DIMSD1", "B", 15, 99},
    {"DIMSD2", "B", 15, 99},
    {"DIMTOLJ", "BS", 15, 99},
    {"DIMTZIN", "BS", 15, 99},
    {"DIMALTZ", "BS", 15, 99},
    {"DIMALTTZ", "BS", 15, 99},
    {"DIMUPT", "B", 15, 99},
    {"DIMATFIT", "BS", 15, 99},
    {"DIMFXLON", "B", 21, 99},
    {"DIMTXTDIRECTION", "B", 24, 99},
    {"DIMALTMZF", "BD", 24, 99},
    {"DIMALTMZS", "TV", 24, 99},
    {"DIMMZF", "BD", 24, 99},
    {"DIMMZS", "TV", 24, 99},
    {"DIMTXSTY", "H", 15, 99},
    {"DIMLDRBLK", "H", 15, 99},
    {"DIMBLK", "H", 15, 99},
    {"DIMBLK1", "H", 15, 99},
    {"DIMBLK2", "H", 15, 99},
    {"DIMLTYPE", "H", 21, 99},
    {"DIMLTEX1", "H", 21, 99},
    {"DIMLTEX2", "H", 21, 99},
    {"DIMLWD", "BS", 15, 99},
    {"DIMLWE", "BS", 15, 99},
    {"BLOCK_CONTROL_OBJECT", "H", 12, 99},
    {"LAYER_CONTROL_OBJECT", "H", 12, 99},
    {"STYLE_CONTROL_OBJECT", "H", 12, 99},
    {"LINETYPE_CONTROL_OBJECT", "H", 12, 99},
    {"VIEW_CONTROL_OBJECT", "H", 12, 99},
    {"UCS_CONTROL_OBJECT", "H", 12, 99},
    {"VPORT_CONTROL_OBJECT", "H", 12, 99},
    {"APPID_CONTROL_OBJECT", "H", 12, 99},
    {"DIMSTYLE_CONTROL_OBJECT", "H", 12, 99},
    {"VPORT_ENTITY_HEADER_CONTROL_OBJECT", "H", 12, 15},
    {"DICTIONARY_ACAD_GROUP", "H", 12, 99},
    {"DICTIONARY_ACAD_MLINESTYLE", "H", 12, 99},
    {"DICTIONARY_NAMED_OBJECTS", "H", 12, 99},
    {"TSTACKALIGN", "BS", 15, 99},
    {"TSTACKSIZE", "BS", 15, 99},
    {"HYPERLINKBASE", "TV", 15, 99},
    {"STYLESHEET", "TV", 15, 99},
    {"DICTIONARY_LAYOUTS", "H", 15, 99},
    {"DICTIONARY_PLOTSETTINGS", "H", 15, 99},
    {"DICTIONARY_PLOTSTYLES", "H", 15, 99},
    {"DICTIONARY_MATERIALS", "H", 18, 99},
    {"DICTIONARY_COLORS", "H", 18, 99},
    {"DICTIONARY_VISUALSTYLE", "H", 21, 99},
    {"UNKNOWN_HANDLE_R2013", "H", 27, 99},
    {"FLAGS", "BL", 15, 99},
    {"INSUNITS", "BS", 15, 99},
    {"CEPSNTYPE", "BS", 15, 99},
    {"CPSNID", "H", 15, 99},
    {"FINGERPRINTGUID", "TV", 15, 99},
    {"VERSIONGUID", "TV", 15, 99},
    {"SORTENTS", "RC", 18, 99},
    {"INDEXCTL", "RC", 18, 99},
    {"HIDETEXT", "RC", 18, 99},
    {"XCLIPFRAME", "RC", 18, 99},
    {"DIMASSOC", "RC", 18, 99},
    {"HALOGAP", "RC", 18, 99},
    {"OBSCUREDCOLOR", "BS", 18, 99},
    {"INTERSECTIONCOLOR", "BS", 18, 99},
    {"OBSCUREDLTYPE", "RC", 18, 99},
    {"INTERSECTIONDISPLAY", "RC", 18, 99},
    {"PROJECTNAME", "TV", 18, 99},
    {"BLOCK_RECORD_PAPER_SPACE", "H", 12, 99},
    {"BLOCK_RECORD_MODEL_SPACE", "H", 12, 99},
    {"LTYPE_BYLAYER", "H", 12, 99},
    {"LTYPE_BYBLOCK", "H", 12, 99},
    {"LTYPE_CONTINUOUS", "H", 12, 99},
    {"CAMERADISPLAY", "B", 21, 99},
    {"UNKNOWN_19", "BL", 21, 99},
    {"UNKNOWN_20", "BL", 21, 99},
    {"UNKNOWN_21", "BD", 21, 99},
    {"STEPSPERSEC", "BD", 21, 99},
    {"STEPSIZE", "BD", 21, 99},
    {"3DDWFPREC", "BD", 21, 99},
    {"LENSLENGTH", "BD", 21, 99},
    {"CAMERAHEIGHT", "BD", 21, 99},
    {"SOLIDHIST", "RC", 21, 99},
    {"SHOWHIST", "RC", 21, 99},
    {"PSOLWIDTH", "BD", 21, 99},
    {"PSOLHEIGHT", "BD", 21, 99},
    {"LOFTANG1", "BD", 21, 99},
    {"LOFTANG2", "BD", 21, 99},
    {"LOFTMAG1", "BD", 21, 99},
    {"LOFTMAG2", "BD", 21, 99},
    {"LOFTPARAM", "BS", 21, 99},
    {"LOFTNORMALS", "RC", 21, 99},
    {"LATITUDE", "BD", 21, 99},
    {"LONGITUDE", "BD", 21, 99},
    {"NORTHDIRECTION", "BD", 21, 99},
    {"TIMEZONE", "BL", 21, 99},
    {"LIGHTGLYPHDISPLAY", "RC", 21, 99},
    {"TILEMODELIGHTSYNCH", "RC", 21, 99},
    {"DWFFRAME", "RC", 21, 99},
    {"DGNFRAME", "RC", 21, 99},
    {"UNKNOWN_22", "B", 21, 99},
    {"INTERFERECOLOR", "CMC", 21, 99},
    {"INTERFEREOBJVS", "H", 21, 99},
    {"INTERFEREVPVS", "H", 21, 99},
    {"DRAGVS", "H", 21, 99},
    {"CSHADOW", "RC", 21, 99},
    {"SHADOWPLANELOCATION", "BD", 21, 99},
    {"UNKNOWN_24", "BS", 14, 18},
    {"UNKNOWN_25", "BS", 14, 18},
    {"UNKNOWN_26", "BS", 14, 18},
    {"UNKNOWN_27", "BS", 14, 18},
});

Value read_one(Bits& r, Bits& hr, std::string_view type) {
  if (type == "B") {
    return r.B();
  }
  if (type == "BS") {
    return r.BS();
  }
  if (type == "BL") {
    return r.BL();
  }
  if (type == "BLL") {
    return r.BLL();
  }
  if (type == "BD") {
    return r.BD();
  }
  if (type == "RC") {
    return r.RC();
  }
  if (type == "RS") {
    return r.RS();
  }
  if (type == "RL") {
    return r.RL();
  }
  if (type == "TV") {
    return r.TV();
  }
  if (type == "T") {
    return r.T();
  }
  if (type == "H") {
    return handle_hex(hr.H());
  }
  if (type == "HS") {
    return handle_hex(r.H());
  }
  if (type == "CMC") {
    return r.CMC();
  }
  if (type == "3BD") {
    return r.BD3();
  }
  if (type == "2RD") {
    return r.RD2();
  }
  if (type == "TD" || type == "TS") {
    const double d = r.BL();
    const double ms = r.BL();
    return d + ms / 86400000;
  }
  throw DecodeError(msg("ccdwg.err.header_type", {{"type", std::string(type)}}));
}

}  // namespace

HeaderResult decode_header_vars(Bits& r, int ver, Bits* handles) {
  Bits& hr = handles != nullptr ? *handles : r;
  HeaderResult out;
  Value& vars = out.vars;
  try {
    for (const Row& e : kRows) {
      if (ver < e.from || ver > e.to) {
        continue;
      }
      if (e.name == "CPSNID" && vars["CEPSNTYPE"].num_or(-1) != 3) {
        continue;
      }
      vars.set(e.name, read_one(r, hr, e.type));
    }
  } catch (const std::exception& err) {
    out.error = error_text(err);
  }
  if (const Value* fl = vars.find("FLAGS"); fl != nullptr && !fl->is_undefined()) {
    const auto f = static_cast<std::uint32_t>(base::clamp_cast<std::int32_t>(fl->num_or(0)));
    vars.set("CELWEIGHT", f & 0x1FU);
    vars.set("ENDCAPS", (f & 0x60U) >> 5U);
    vars.set("JOINSTYLE", (f & 0x180U) >> 7U);
    vars.set("LWDISPLAY", (f & 0x200U) != 0 ? 0 : 1);
    vars.set("XEDIT", (f & 0x400U) != 0 ? 0 : 1);
    vars.set("EXTNAMES", (f & 0x800U) != 0 ? 1 : 0);
    vars.set("PSTYLEMODE", (f & 0x2000U) != 0 ? 1 : 0);
    vars.set("OLESTARTUP", (f & 0x4000U) != 0 ? 1 : 0);
  }
  return out;
}

}  // namespace cc::dwg
