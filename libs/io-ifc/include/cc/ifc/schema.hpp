// SPDX-License-Identifier: Apache-2.0
// cc::ifc schema — cc-ifc's schema.js: the IFC schemas cc-ifc knows (IFC2X3, IFC4, IFC4X3; tables generated from
// buildingSMART's EXPRESS files, src/schema_tables.inc) as lookups: entity → every explicit attribute in file order
// (supertypes first), subtype tests, defined types, and the numeric type codes (CRC-32 of the upper-case name, the
// numbers the apps' IFC constants use).
//
//   schema_for("IFC4")           IFC2X3 | IFC4 | IFC4X3 (any IFC4X3_* name) → {schema, guessed}; unknown → IFC4,
//   guessed s.entity("IFCWALL")          → Entity* (name "IfcWall", upper, code, parent, abstract, attrs [{name, t}],
//   derived) s.is_a("IFCWALLSTANDARDCASE", "IFCWALL") → true s.subtypes("IFCWALL")        → IFCWALL and every entity
//   under it, depth first in table order s.type_code("IFCLENGTHMEASURE") → "r"   (attribute codes: E S e s r i n b l x,
//   '[' per aggregate level) type_code(name) / name_of_code(code)   CRC-32 and back (every schema name, the header
//   entities, and any name
//                                          type_code has seen)
// Schemas are built once, on first use, and immutable after: safe to share between threads.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cc::ifc {

/// An attribute's type code parsed: "[[r:IFCLENGTHMEASURE?" → agg 2, base "r", tname "IFCLENGTHMEASURE", opt.
struct AttrType {
  int agg = 0;
  std::string_view base;
  std::string_view tname;  // empty = none (null in the JS)
  bool opt = false;
};
[[nodiscard]] AttrType parse_attr_code(std::string_view t);

struct Attr {
  std::string_view name;
  AttrType t;
};

struct Entity {
  std::string_view name;   // "IfcWall"
  std::string_view upper;  // "IFCWALL"
  std::uint32_t code = 0;
  std::string_view parent;  // empty = none
  bool abstract = false;
  std::vector<Attr> attrs;
  std::unordered_set<std::string_view> derived;
  std::unordered_map<std::string_view, std::size_t> index;  // attribute name → position (the last one of a name)
  /// The position of an attribute, or -1.
  [[nodiscard]] std::ptrdiff_t at(std::string_view attr) const {
    const auto it = index.find(attr);
    return it == index.end() ? -1 : static_cast<std::ptrdiff_t>(it->second);
  }
};

class Schema {
 public:
  struct Tables;
  explicit Schema(const Tables& t, std::string_view name);

  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  [[nodiscard]] std::string_view source() const noexcept { return source_; }
  [[nodiscard]] bool has(std::string_view up) const { return ents_.contains(up); }
  [[nodiscard]] const Entity* entity(std::string_view up) const;
  [[nodiscard]] bool is_a(std::string_view up, std::string_view ancestor) const;
  [[nodiscard]] const std::vector<std::string_view>& subtypes(std::string_view up) const;
  /// A defined type's underlying code ("r", "s", "[r", "e", "S" …), or nullopt.
  [[nodiscard]] std::optional<std::string_view> type_code(std::string_view up) const;
  [[nodiscard]] std::optional<std::string_view> type_name(std::string_view up) const;
  [[nodiscard]] std::vector<std::string_view> select_members(std::string_view up) const;
  /// Is a value of defined type `up` written as a REAL?
  [[nodiscard]] bool is_real_type(std::string_view up) const;
  /// "IfcWall" for "IFCWALL" (entities and types), or nullopt.
  [[nodiscard]] std::optional<std::string_view> pretty(std::string_view up) const;
  /// Every entity / defined type name, in the table's order.
  [[nodiscard]] const std::vector<std::string_view>& entity_names() const noexcept { return ent_order_; }
  [[nodiscard]] const std::vector<std::string_view>& type_names() const noexcept { return typ_order_; }

 private:
  struct Typ {
    std::string_view name;
    std::string_view code;
    std::string_view members;
  };
  std::string_view name_;
  std::string_view source_;
  std::unordered_map<std::string_view, Entity> ents_;
  std::unordered_map<std::string_view, std::vector<std::string_view>> subtypes_;
  std::unordered_map<std::string_view, Typ> typs_;
  std::vector<std::string_view> ent_order_;
  std::vector<std::string_view> typ_order_;
};

struct SchemaPick {
  const Schema* schema = nullptr;
  bool guessed = false;  // the file's schema was none cc-ifc knows: IFC4 is used
};
/// schema.js schemaFor.
[[nodiscard]] SchemaPick schema_for(std::string_view file_schema);
/// The schema of that key ("IFC2X3", "IFC4", "IFC4X3").
[[nodiscard]] const Schema& schema_named(std::string_view key);

/// CRC-32 of a string's UTF-16 code units' low bytes (schema.js crc32; IFC names are ASCII).
[[nodiscard]] std::uint32_t crc32_name(std::string_view s);
/// typeCode(name): the CRC-32 of the upper-case name; the name is remembered for name_of_code.
[[nodiscard]] std::uint32_t type_code(std::string_view name);
/// nameOfCode(code): the upper-case name, or nullopt.
[[nodiscard]] std::optional<std::string> name_of_code(std::uint32_t code);

}  // namespace cc::ifc
