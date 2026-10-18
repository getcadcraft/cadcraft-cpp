// SPDX-License-Identifier: Apache-2.0
// What dxf.js (R12) and dxf2000.js (R2000) share: the group-code lines, their two number formats, their two text
// encodings, and linetypes.js patternOf (DWG saving uses it too). The JS's String(v) and toPrecision(15) exactly.
#pragma once

#include <string>
#include <string_view>

#include "cc/base/value.hpp"
#include "cc/doc/doc.hpp"

namespace cc::save::detail {

using base::Value;

/// lines.push(String(code).padStart(3, ' ')); lines.push(String(v)) — joined with "\r\n", and one after the last.
class DxfLines {
 public:
  DxfLines() { s_.reserve(1U << 16U); }
  void w(int code, std::string_view v) {
    code_line(code);
    s_ += v;
    s_ += "\r\n";
  }
  void w(int code, const char* v) { w(code, std::string_view(v)); }
  void w(int code, const std::string& v) { w(code, std::string_view(v)); }
  /// String(v) of any value.
  void w(int code, const Value& v);
  /// String(n) of a number.
  void w(int code, double n);
  void w(int code, int n);
  /// Where the next value line starts (to put $HANDSEED's value in at the end).
  [[nodiscard]] std::size_t value_at() const noexcept { return s_.size() + 5; }
  /// Replaces the `len` characters at `at` (a value written earlier) with `v`.
  void patch(std::size_t at, std::size_t len, std::string_view v) { s_.replace(at, len, v); }
  [[nodiscard]] std::string take() { return std::move(s_); }

 private:
  std::string s_;
  void code_line(int code);
};

/// String(n) of a number (Number#toString).
[[nodiscard]] std::string num_str(double n);
/// Number(n.toPrecision(15)): n rounded to 15 significant digits, an exact half away from zero.
[[nodiscard]] double round15(double n);

/// dxf.js f(n): '0.0' under 1e-12, else Number(n.toPrecision(15)).toString(). A value that is not a number throws
/// (as the JS's n.toPrecision does) unless it reads as 0 (null, false, '').
[[nodiscard]] std::string f12(double n);
[[nodiscard]] std::string f12(const Value& n);
/// dxf2000.js f(n): the same for a finite number; anything else '0.0'.
[[nodiscard]] std::string f2k(double n);
[[nodiscard]] std::string f2k(const Value& n);

/// dxf.js encodeText(s): ⌀ ° ± back to %%c %%d %%p, then every UTF-16 unit outside 0x20–0x7e as \U+XXXX.
[[nodiscard]] std::string encode_r12(std::string_view s);
/// dxf2000.js enc(s): runs of CR / LF → one space, then every UTF-16 unit outside 0x20–0x7e as \U+XXXX.
[[nodiscard]] std::string enc2000(std::string_view s);

/// linetypes.js patternOf(doc, name): the dash pattern of a linetype in drawing units, or null when continuous.
[[nodiscard]] Value pattern_of(const doc::Doc& doc, const std::string& name);

/// pictures.js pictureRefs(doc): references to picture blocks in the model and the sheets.
[[nodiscard]] double picture_refs(const doc::Doc& doc);
/// The sheets' viewports with their own layer overrides (VP.3: DWG keeps them, DXF does not).
[[nodiscard]] double viewport_overrides(const doc::Doc& doc);

/// s.slice(0, n) in UTF-16 units.
[[nodiscard]] std::string utf16_prefix(std::string_view s, std::size_t n);

/// /^standard$/i of a value.
[[nodiscard]] bool is_standard(const Value& v);

}  // namespace cc::save::detail
