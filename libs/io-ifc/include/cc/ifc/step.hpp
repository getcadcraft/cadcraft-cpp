// SPDX-License-Identifier: Apache-2.0
// cc::ifc — L3. ISO 10303-21 (the .ifc text format): the C++ port of cc-ifc's step.js (CadCraft 0.5, lib/ifc), written
// from ISO 10303-21:2016 there. One pass indexes every instance of the DATA section(s); an instance's parameters are
// parsed only when it is read.
//
//   scan_step(bytes)                  → StepIndex (ids, byte ranges, type names, header, errors) or a StepError when
//                                       the file is not Part 21 at all
//   parse_params(bytes, start, end)   → the instance's parameter list (Raw list) or a StepError, worded as the JS
//   parse_complex(bytes, start, end)  → the parts of a complex instance "(A(..)B(..))"
//
// Raw values are step.js's "raw" form: null ($), derived (*), number, string (decoded: '' \\ \S\ \X\ \X2\ \X4\, raw
// UTF-8 accepted), ref (#n), enum (.X.), bin ("0AF"), typed (IFCLABEL('x')), list (an aggregate). Strings are UTF-8,
// with lone UTF-16 surrogates as WTF-8 (the JS works in UTF-16).
//
// Where the JS would overflow V8's stack (aggregates nested thousands deep, a depth V8 does not fix: measured 3,000 to
// 6,000 levels), the parser stops at kMaxDepth with V8's words (docs/gaps.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"

namespace cc::ifc {

using Bytes = std::span<const std::byte>;

inline constexpr std::size_t kMaxDepth = 1000;

/// One parameter value.
struct Raw {
  enum class Kind : std::uint8_t { null, derived, number, string, ref, enumeration, binary, typed, list };
  Kind kind = Kind::null;
  double num = 0;          // number; ref (the instance number, a double as in the JS)
  std::string text;        // string; enumeration; binary; typed (the type name)
  std::vector<Raw> items;  // list; typed (one item: its value)

  [[nodiscard]] bool is_null() const noexcept { return kind == Kind::null; }
  [[nodiscard]] bool is_number() const noexcept { return kind == Kind::number; }
  [[nodiscard]] bool is_string() const noexcept { return kind == Kind::string; }
  [[nodiscard]] bool is_ref() const noexcept { return kind == Kind::ref; }
  [[nodiscard]] bool is_list() const noexcept { return kind == Kind::list; }
  [[nodiscard]] bool is_enum() const noexcept { return kind == Kind::enumeration; }
  [[nodiscard]] bool is_typed() const noexcept { return kind == Kind::typed; }
};
using RawList = std::vector<Raw>;

/// A thrown-in-the-JS StepError (or a RangeError of String.fromCodePoint, or V8's stack overflow), as its message.
struct StepError {
  std::string name = "StepError";
  std::string message;
};
template <class T>
using StepResult = std::expected<T, StepError>;

struct ScanError {
  double offset = 0;
  std::string message;
};

struct StepIndex {
  std::size_t count = 0;
  double max_id = 0;  // the largest instance number as the JS computes it (a double; Int32 ids wrap, this does not)
  std::vector<std::int32_t> ids;      // instance number of record i (Int32Array: wrapped)
  std::vector<std::uint32_t> starts;  // offset of the "(" after its keyword (or of the complex instance's "(")
  std::vector<std::uint32_t> ends;    // offset of its closing ";"
  std::vector<std::uint16_t> types;   // index into names (Uint16Array: wrapped); 0 = "" (complex)
  std::vector<std::string> names;     // upper-case keywords, names[0] = ""
  base::Value header;                 // {description, name, schema[], raw{}} as the JS (raw params as Values)
  std::string schema;                 // header.schema[0] || ''
  std::vector<ScanError> errors;
};

[[nodiscard]] StepResult<StepIndex> scan_step(Bytes bytes);
[[nodiscard]] StepResult<RawList> parse_params(Bytes bytes, std::size_t start, std::size_t end);

struct ComplexPart {
  std::string type;
  RawList params;
};
[[nodiscard]] StepResult<std::vector<ComplexPart>> parse_complex(Bytes bytes, std::size_t start, std::size_t end);

/// A raw value as the JS object the kit sees ({ref: n}, {enum: 'X'}, …).
[[nodiscard]] base::Value to_value(const Raw& r);
[[nodiscard]] base::Value to_value(const RawList& list);
/// Appends the JSON of a raw value (the parity kit's form) without building a Value.
void write_json(const Raw& r, std::string& out);
void write_json(const RawList& list, std::string& out);

// ---- the parity kit's ifc-step engine (tools/fork-parity lib/engines.mjs): NDJSON, line 1 the head
/// {schema, header, count, maxId, types, errors} as JSON.
[[nodiscard]] std::string step_head_json(const StepIndex& ix);
/// Appends `[id, "TYPE", params]` for record i (a complex one: `[id, "", [{type, params}…]]`; a line the parser
/// refuses:
/// `{"$error": message}` as its params), without the newline.
void step_line_json(Bytes bytes, const StepIndex& ix, std::size_t i, std::string& out);
/// Parses every record as the kit's engine does (the work the bench times); → how many parsed.
std::size_t step_parse_all(Bytes bytes, const StepIndex& ix);

}  // namespace cc::ifc
