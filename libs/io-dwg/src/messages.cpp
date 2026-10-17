// SPDX-License-Identifier: Apache-2.0
// Port of cc-dwg messages.js (the English report notes and errors). The notes are part of the model the parity gate
// compares, so the wording is the JS reference's, byte for byte; tools/gen-messages (see the commit) copies the table.
#include "messages.hpp"

#include <array>
#include <string>
#include <string_view>

#include "cc/base/value.hpp"

namespace cc::dwg {

namespace {

struct Message {
  std::string_view key;
  std::string_view other;  // the text, or the plural form
  const char* one;         // the singular form, or nullptr
};

constexpr auto kMessages = std::to_array<Message>({
    {"ccdwg.open.failed", "Could not open the file: {reason}", nullptr},
    {"ccdwg.open.too_short", "too short to be a DWG file ({n} bytes)", "too short to be a DWG file ({n} byte)"},
    {"ccdwg.open.version", "DWG version {version} is not supported (only R13 to R2018: AC1012–AC1032)", nullptr},
    {"ccdwg.open.not_dwg", "not a DWG file (it starts \"{start}\")", nullptr},
    {"ccdwg.open.r13_header", "damaged R13–R15 file header ({n} section locators)", nullptr},
    {"ccdwg.open.r13_locators", "damaged R13–R15 file: section locators point outside the file", nullptr},
    {"ccdwg.open.no_id", "damaged {version} file header (no AcFssFcAJMB id)", nullptr},
    {"ccdwg.open.no_section_map", "damaged {version} file: section map page {id} is not in the page map", nullptr},
    {"ccdwg.open.past_end", "damaged file: the {what} lies past the end of the file", nullptr},
    {"ccdwg.open.page_type", "damaged file: the {what} page has type 0x{type}", nullptr},
    {"ccdwg.open.runs_past", "damaged file: the {what} runs past the end of the file", nullptr},
    {"ccdwg.open.r2007_short", "R2007 file too short", nullptr},
    {"ccdwg.open.r2007_no_section_map", "damaged R2007 file: no section map page", nullptr},
    {"ccdwg.open.r2007_past_end", "damaged R2007 file: the {what} lies past the end", nullptr},
    {"ccdwg.part.page_map", "page map", nullptr},
    {"ccdwg.part.section_map", "section map", nullptr},
    {"ccdwg.read.password", "The drawing is password-protected (encrypted data); its sections cannot be read.",
     nullptr},
    {"ccdwg.read.codepage", "Code page {num} ({name}) is decoded as ANSI_1252.", nullptr},
    {"ccdwg.read.file_header_crc", "File header CRC does not match (file may be damaged).", nullptr},
    {"ccdwg.read.file_header_crc32", "File header CRC-32 does not match.", nullptr},
    {"ccdwg.read.r2007_header_bad", "The R2007 file header has uncorrectable errors.", nullptr},
    {"ccdwg.read.r2007_corrected", "Corrected {n} damaged bytes in the file header.",
     "Corrected {n} damaged byte in the file header."},
    {"ccdwg.read.uncorrectable", "The {what} has uncorrectable errors.", nullptr},
    {"ccdwg.read.section_map_damaged", "The section map is damaged ({n} pages for one section).", nullptr},
    {"ccdwg.read.section_encrypted", "Section {name} is encrypted and was skipped.", nullptr},
    {"ccdwg.read.section_failed", "Section {name} could not be read: {reason}", nullptr},
    {"ccdwg.read.system_checksum", "Checksum of the {what} does not match.", nullptr},
    {"ccdwg.read.page_checksum", "Checksum of a page of {name} does not match.", nullptr},
    {"ccdwg.read.pages_bad", "{n} pages of {name} could not be found or decoded.",
     "{n} page of {name} could not be found or decoded."},
    {"ccdwg.read.pages_missing", "{n} pages of {name} could not be found.", "{n} page of {name} could not be found."},
    {"ccdwg.read.header_failed", "Header variables could not be read: {reason}", nullptr},
    {"ccdwg.read.header_sentinel", "Header section sentinel is missing.", nullptr},
    {"ccdwg.read.header_crc", "Header section CRC does not match.", nullptr},
    {"ccdwg.read.header_early", "Header variables stop early: {reason}", nullptr},
    {"ccdwg.read.classes_failed", "Classes could not be read: {reason}", nullptr},
    {"ccdwg.read.classes_sentinel", "Classes section sentinel is missing.", nullptr},
    {"ccdwg.read.classes_crc", "Classes section CRC does not match.", nullptr},
    {"ccdwg.read.class_number", "Class list stops at an unexpected class number {num}.", nullptr},
    {"ccdwg.read.map_failed", "Object map could not be read: {reason}", nullptr},
    {"ccdwg.read.map_short", "Object map is cut short.", nullptr},
    {"ccdwg.read.map_crc", "{n} object map pages failed the CRC check.", "{n} object map page failed the CRC check."},
    {"ccdwg.read.map_entry", "Object map entry {handle} points at object {other}.", nullptr},
    {"ccdwg.read.object_failed", "Object {handle} could not be read: {reason}", nullptr},
    {"ccdwg.read.no_objects", "The file has no objects section.", nullptr},
    {"ccdwg.read.more_failed", "… {n} more objects could not be read.", "… {n} more object could not be read."},
    {"ccdwg.read.crc_failed", "{n} objects failed the CRC check (read anyway).",
     "{n} object failed the CRC check (read anyway)."},
    {"ccdwg.read.partial_type", "{n} {type} objects could only be read in part.",
     "{n} {type} object could only be read in part."},
    {"ccdwg.read.assembled_part", "The drawing could only be assembled in part: {reason}", nullptr},
    {"ccdwg.read.kept_common", "{n} damaged objects kept with only their common data.",
     "{n} damaged object kept with only its common data."},
    {"ccdwg.read.orphans", "{n} entities were not linked from their block and were added at its end.",
     "{n} entity was not linked from its block and was added at its end."},
    {"ccdwg.read.acds_empty", "The data storage section is empty.", nullptr},
    {"ccdwg.read.acds_no_sig", "The data storage section has no \"jard\" signature.", nullptr},
    {"ccdwg.read.acds_entry", "Data storage entry points at a \"{name}\" segment.", nullptr},
    {"ccdwg.read.acds_record_large", "Data storage record of {handle} is larger than its segment.", nullptr},
    {"ccdwg.read.acds_partial", "The data storage section could only be read in part: {reason}", nullptr},
    {"ccdwg.read.acis_no_storage",
     "{n} ACIS entities say their data is in the data storage section, which the file does not have.",
     "{n} ACIS entity says its data is in the data storage section, which the file does not have."},
    {"ccdwg.read.acis_no_record", "{n} ACIS entities have no record in the data storage section (kept empty).",
     "{n} ACIS entity has no record in the data storage section (kept empty)."},
    {"ccdwg.err.no_header", "no AcDb:Header section", nullptr},
    {"ccdwg.err.no_classes", "no AcDb:Classes section", nullptr},
    {"ccdwg.err.no_handles", "no AcDb:Handles section", nullptr},
    {"ccdwg.err.read_past_end", "read past the end ({at} > {end} bits)", nullptr},
    {"ccdwg.err.bitlong", "bad BITLONG code 11", nullptr},
    {"ccdwg.err.bitdouble", "bad BITDOUBLE code 11", nullptr},
    {"ccdwg.err.lz_before_start", "LZ back-reference before the start of the page", nullptr},
    {"ccdwg.err.lz_opcode", "LZ opcode 0x{op} is not used", nullptr},
    {"ccdwg.err.r2007_lz", "R2007 LZ back-reference before the start", nullptr},
    {"ccdwg.err.header_type", "unknown header type {type}", nullptr},
    {"ccdwg.err.offset_outside", "object offset outside the section", nullptr},
    {"ccdwg.err.object_past_end", "object of {n} bytes runs past the section end", nullptr},
    {"ccdwg.err.size_fields", "object size fields do not agree (damaged object)", nullptr},
    {"ccdwg.err.eed_code", "EED code {code}", nullptr},
    {"ccdwg.err.handle_count", "handle count {n} is larger than the handle stream", nullptr},
    {"ccdwg.err.count_large", "count {n} is larger than the data", nullptr},
    {"ccdwg.err.dictionary_size", "dictionary of {n} entries", nullptr},
    {"ccdwg.err.hatch_edge", "hatch edge type {type}", nullptr},
    {"ccdwg.err.xrecord_code", "XRECORD group code {code}", nullptr},
    {"ccdwg.err.value_type", "value data type {type}", nullptr},
    {"ccdwg.err.surface_code", "surface field code {code}", nullptr},
    {"ccdwg.err.acds_no_segment", "no file segment at {offset}", nullptr},
    {"ccdwg.err.acds_index", "the segment index is a \"{name}\" segment", nullptr},
    {"ccdwg.err.acds_segment", "segment {n} is not in the index", nullptr},
    {"ccdwg.err.acis_block", "ACIS block of {n} bytes is larger than the data", nullptr},
    {"ccdwg.err.acis_no_end", "ACIS data (version 2) has no end mark", nullptr},
    {"ccdwg.err.acis_version", "ACIS data version {version}", nullptr},
    {"ccdwg.err.wire_large", "wire of {n} points is larger than the data", nullptr},
    {"ccdwg.err.wires_large", "{n} wires are more than the data holds", nullptr},
    {"ccdwg.err.silhouettes_large", "{n} silhouettes are more than the data holds", nullptr},
    {"ccdwg.write.eed_dropped",
     "{n} extended-data blocks referring to objects this writer does not copy (materials, groups, …) were left out.",
     "{n} extended-data block referring to objects this writer does not copy (materials, groups, …) was left out."},
    {"ccdwg.write.true_colours",
     "{n} true colours became the nearest AutoCAD colour index (R2000 stores colour indexes only).",
     "{n} true colour became the nearest AutoCAD colour index (R2000 stores colour indexes only)."},
    {"ccdwg.write.chars_lost", "{n} characters outside the Basic Multilingual Plane (emoji and the like) became \"?\".",
     "{n} character outside the Basic Multilingual Plane (emoji and the like) became \"?\"."},
    {"ccdwg.write.nongraphic",
     "{n} non-graphical objects were copied from the source ({raw} bit for bit in the drawing's own version, {fields} "
     "from their fields).",
     "{n} non-graphical object was copied from the source ({raw} bit for bit in the drawing's own version, {fields} "
     "from their fields)."},
    {"ccdwg.write.not_copied", "Not copied (cc-dwg cannot write them in this version, or they depend on them): {list}.",
     nullptr},
    {"ccdwg.write.entities_dropped",
     "{n} {type} entities were left out: undecoded data can only be written back in the drawing's own version.",
     "{n} {type} entity was left out: undecoded data can only be written back in the drawing's own version."},
    {"ccdwg.write.proxied",
     "Written as proxies of their own class (undecoded data in another version, as AutoCAD saves them down): {list}.",
     nullptr},
    {"ccdwg.write.raw", "Copied bit for bit (custom or proxy entities): {list}.", nullptr},
    {"ccdwg.write.acis_empty", "{n} ACIS entities have no ACIS data (written empty).",
     "{n} ACIS entity has no ACIS data (written empty)."},
    {"ccdwg.write.acis_sat",
     "{n} ACIS entities keep their SAT text (as the data storage record; R2013+ files normally hold SAB there — SAT is "
     "not converted).",
     "{n} ACIS entity keeps its SAT text (as the data storage record; R2013+ files normally hold SAB there — SAT is "
     "not converted)."},
    {"ccdwg.write.acis_sab",
     "{n} ACIS entities keep their SAB data as ACIS version 2 in an R2000 file (AutoCAD 2000 itself writes SAT).",
     "{n} ACIS entity keeps its SAB data as ACIS version 2 in an R2000 file (AutoCAD 2000 itself writes SAT)."},
    {"ccdwg.write.nurbs_down",
     "{n} NURBS surfaces were written as plain SURFACE ({version} has no NURBSURFACE; as AutoCAD and the ODA File "
     "Converter save it down, with the ACAD_XREC_ROUNDTRIP marker that turns it back into a NURBS surface in R2013+).",
     "{n} NURBS surface was written as plain SURFACE ({version} has no NURBSURFACE; as AutoCAD and the ODA File "
     "Converter save it down, with the ACAD_XREC_ROUNDTRIP marker that turns it back into a NURBS surface in R2013+)."},
    {"ccdwg.write.nurbs_up",
     "{n} surfaces saved down from NURBSURFACE were written as NURBSURFACE again (its NURBS fields were not kept by "
     "the save-down).",
     "{n} surface saved down from NURBSURFACE was written as NURBSURFACE again (its NURBS fields were not kept by the "
     "save-down)."},
    {"ccdwg.write.embedded",
     "{n} embedded profile/path entities of a surface (an encoding cc-dwg does not know) were left out: it is kept "
     "only in the drawing's own version.",
     "{n} embedded profile/path entity of a surface (an encoding cc-dwg does not know) was left out: it is kept only "
     "in the drawing's own version."},
    {"ccdwg.write.table_fields", "{n} table cell fields could not be written and kept their text.",
     "{n} table cell field could not be written and kept its text."},
});

std::string fill_text(const base::Value& v) {
  if (v.is_string()) {
    return v.as_string();
  }
  if (v.is_number()) {
    std::string s;
    base::append_number(s, v.as_number());
    return s;
  }
  if (v.is_bool()) {
    return v.as_bool() ? "true" : "false";
  }
  return {};
}

}  // namespace

std::string msg(std::string_view key, std::initializer_list<Fill> fills) {
  const Message* m = nullptr;
  for (const Message& x : kMessages) {
    if (x.key == key) {
      m = &x;
      break;
    }
  }
  if (m == nullptr) {
    return std::string(key);
  }
  std::string_view text = m->other;
  if (m->one != nullptr) {
    for (const Fill& f : fills) {
      if (f.name == "n" && f.value.is_number() && f.value.as_number() == 1.0) {
        text = m->one;
      }
    }
  }
  // {{ → {, }} → }, {name} → the fill (kept as written when there is no such fill)
  std::string out;
  out.reserve(text.size() + 16);
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '{' && i + 1 < text.size() && text[i + 1] == '{') {
      out += '{';
      ++i;
      continue;
    }
    if (c == '}' && i + 1 < text.size() && text[i + 1] == '}') {
      out += '}';
      ++i;
      continue;
    }
    if (c == '{') {
      const std::size_t close = text.find('}', i);
      if (close != std::string_view::npos) {
        const std::string_view name = text.substr(i + 1, close - i - 1);
        const Fill* hit = nullptr;
        for (const Fill& f : fills) {
          if (f.name == name) {
            hit = &f;
          }
        }
        if (hit != nullptr && !hit->value.is_undefined() && !hit->value.is_null()) {
          out += fill_text(hit->value);
          i = close;
          continue;
        }
      }
    }
    out += c;
  }
  return out;
}

}  // namespace cc::dwg
