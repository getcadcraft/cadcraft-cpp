// SPDX-License-Identifier: Apache-2.0
// schema.js (cc-ifc, CadCraft 0.5) in C++: see cc/ifc/schema.hpp.
#include "cc/ifc/schema.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cc/base/js.hpp"

namespace cc::ifc {

// Not in an anonymous namespace: Schema::Tables (a class member) names them.
struct EntRow {  // NOLINT(misc-use-internal-linkage)
  std::string_view upper;
  std::string_view name;
  std::string_view parent;
  bool abstract;
  std::string_view attrs;
  std::string_view derived;
};
struct TypRow {  // NOLINT(misc-use-internal-linkage)
  std::string_view upper;
  std::string_view name;
  std::string_view code;
  std::string_view members;
};

namespace {
#include "schema_tables.inc"

std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  for (;;) {
    const std::size_t k = s.find(sep, i);
    out.push_back(s.substr(i, k == std::string_view::npos ? std::string_view::npos : k - i));
    if (k == std::string_view::npos) {
      return out;
    }
    i = k + 1;
  }
}

std::array<std::uint32_t, 256> make_crc() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t n = 0; n < 256; ++n) {
    std::uint32_t c = n;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
    }
    t[n] = c;
  }
  return t;
}

struct CodeNames {
  std::mutex lock;
  std::unordered_map<std::uint32_t, std::string> names;
};
CodeNames& code_names() {
  static CodeNames c;
  return c;
}

}  // namespace

struct Schema::Tables {
  std::string_view source;
  std::span<const EntRow> ent;
  std::span<const TypRow> typ;
};

AttrType parse_attr_code(std::string_view t) {
  AttrType a;
  std::size_t i = 0;
  while (i < t.size() && t[i] == '[') {
    ++a.agg;
    ++i;
  }
  a.opt = !t.empty() && t.back() == '?';
  std::string_view body = t.substr(i);
  if (a.opt) {
    // t.slice(i, -1): an empty body when the code is only "[…?"
    body = i < t.size() ? t.substr(i, t.size() - 1 - i) : std::string_view();
  }
  const std::size_t k = body.find(':');
  if (k == std::string_view::npos) {
    a.base = body;
  } else {
    a.base = body.substr(0, k);
    a.tname = body.substr(k + 1);
  }
  return a;
}

std::uint32_t crc32_name(std::string_view s) {
  static const std::array<std::uint32_t, 256> kCrc = make_crc();
  std::uint32_t c = 0xFFFFFFFFU;
  bool ascii = true;
  for (const char ch : s) {
    ascii = ascii && static_cast<unsigned char>(ch) < 0x80;
  }
  if (ascii) {
    for (const char ch : s) {
      c = kCrc[(c ^ static_cast<unsigned char>(ch)) & 0xFFU] ^ (c >> 8U);
    }
  } else {
    for (const std::uint16_t u : base::to_utf16(s)) {
      c = kCrc[(c ^ u) & 0xFFU] ^ (c >> 8U);
    }
  }
  return c ^ 0xFFFFFFFFU;
}

std::uint32_t type_code(std::string_view name) {
  const std::string up = base::js_to_upper(name);
  const std::uint32_t c = crc32_name(up);
  CodeNames& cn = code_names();
  const std::scoped_lock guard(cn.lock);
  cn.names.try_emplace(c, up);
  return c;
}

std::optional<std::string> name_of_code(std::uint32_t code) {
  (void)schema_named("IFC2X3");  // every schema's names are known codes (CONSTANTS)
  (void)schema_named("IFC4");
  (void)schema_named("IFC4X3");
  static const bool kHeader = [] {
    for (const std::string_view h : {"FILE_DESCRIPTION", "FILE_NAME", "FILE_SCHEMA"}) {
      (void)type_code(h);
    }
    return true;
  }();
  (void)kHeader;
  CodeNames& cn = code_names();
  const std::scoped_lock guard(cn.lock);
  const auto it = cn.names.find(code);
  if (it == cn.names.end()) {
    return std::nullopt;
  }
  return it->second;
}

Schema::Schema(const Tables& t, std::string_view name) : name_(name), source_(t.source) {
  std::unordered_map<std::string_view, const EntRow*> rows;
  std::unordered_map<std::string_view, std::vector<std::string_view>> children;
  for (const EntRow& r : t.ent) {
    rows.emplace(r.upper, &r);
    ent_order_.push_back(r.upper);
    if (!r.parent.empty()) {
      children[r.parent].push_back(r.upper);
    }
    (void)type_code(r.upper);
  }
  for (const TypRow& r : t.typ) {
    typs_.emplace(r.upper, Typ{r.name, r.code, r.members});
    typ_order_.push_back(r.upper);
    (void)type_code(r.upper);
  }
  // entities, supertypes first (a parent chain is short and acyclic in the tables)
  // NOLINTNEXTLINE(misc-no-recursion) — a supertype chain is short and acyclic in the generated tables
  auto build = [&](auto& self, std::string_view up) -> const Entity* {
    if (const auto it = ents_.find(up); it != ents_.end()) {
      return &it->second;
    }
    const auto rit = rows.find(up);
    if (rit == rows.end()) {
      return nullptr;
    }
    const EntRow& row = *rit->second;
    const Entity* parent = row.parent.empty() ? nullptr : self(self, row.parent);
    Entity e;
    e.name = row.name;
    e.upper = row.upper;
    e.code = crc32_name(row.upper);
    e.parent = row.parent;
    e.abstract = row.abstract;
    if (parent != nullptr) {
      e.attrs = parent->attrs;
      e.derived = parent->derived;
    }
    if (!row.attrs.empty()) {
      for (const std::string_view s : split(row.attrs, ';')) {
        const std::size_t bar = s.find('|');
        const std::string_view n = s.substr(0, bar);
        const std::string_view code = bar == std::string_view::npos ? std::string_view() : s.substr(bar + 1);
        e.attrs.push_back(Attr{n, parse_attr_code(code)});
      }
    }
    if (!row.derived.empty()) {
      for (const std::string_view d : split(row.derived, ';')) {
        e.derived.insert(d);
      }
    }
    for (std::size_t k = 0; k < e.attrs.size(); ++k) {
      e.index[e.attrs[k].name] = k;
    }
    return &ents_.emplace(up, std::move(e)).first->second;
  };
  for (const EntRow& r : t.ent) {
    (void)build(build, r.upper);
  }
  // subtype lists: depth first, children in table order
  for (const EntRow& r : t.ent) {
    std::vector<std::string_view> s;
    std::vector<std::string_view> stack{r.upper};
    while (!stack.empty()) {
      const std::string_view n = stack.back();
      stack.pop_back();
      s.push_back(n);
      if (const auto it = children.find(n); it != children.end()) {
        for (const std::string_view c : std::views::reverse(it->second)) {
          stack.push_back(c);
        }
      }
    }
    subtypes_.emplace(r.upper, std::move(s));
  }
}

const Entity* Schema::entity(std::string_view up) const {
  const auto it = ents_.find(up);
  return it == ents_.end() ? nullptr : &it->second;
}

bool Schema::is_a(std::string_view up, std::string_view ancestor) const {
  const Entity* e = entity(up);
  while (e != nullptr) {
    if (up == ancestor) {
      return true;
    }
    up = e->parent;
    e = up.empty() ? nullptr : entity(up);
  }
  return false;
}

const std::vector<std::string_view>& Schema::subtypes(std::string_view up) const {
  static const std::vector<std::string_view> kNone;
  const auto it = subtypes_.find(up);
  return it == subtypes_.end() ? kNone : it->second;
}

std::optional<std::string_view> Schema::type_code(std::string_view up) const {
  const auto it = typs_.find(up);
  if (it == typs_.end() || it->second.code.empty()) {
    return std::nullopt;
  }
  return it->second.code;
}

std::optional<std::string_view> Schema::type_name(std::string_view up) const {
  const auto it = typs_.find(up);
  if (it == typs_.end() || it->second.name.empty()) {
    return std::nullopt;
  }
  return it->second.name;
}

std::vector<std::string_view> Schema::select_members(std::string_view up) const {
  const auto it = typs_.find(up);
  if (it == typs_.end() || it->second.code != "S") {
    return {};
  }
  return split(it->second.members, ';');
}

bool Schema::is_real_type(std::string_view up) const {
  const auto c = type_code(up);
  if (!c) {
    return false;
  }
  std::size_t i = 0;
  while (i < c->size() && (*c)[i] == '[') {
    ++i;
  }
  return i + 1 == c->size() && ((*c)[i] == 'r' || (*c)[i] == 'n');
}

std::optional<std::string_view> Schema::pretty(std::string_view up) const {
  if (const Entity* e = entity(up)) {
    return e->name;
  }
  const auto it = typs_.find(up);
  if (it == typs_.end() || it->second.name.empty()) {
    return std::nullopt;
  }
  return it->second.name;
}

const Schema& schema_named(std::string_view key) {
  static const Schema kIfc2x3(Schema::Tables{kIfc2x3Source, kIfc2x3Ent, kIfc2x3Typ}, "IFC2X3");
  static const Schema kIfc4(Schema::Tables{kIfc4Source, kIfc4Ent, kIfc4Typ}, "IFC4");
  static const Schema kIfc4x3(Schema::Tables{kIfc4x3Source, kIfc4x3Ent, kIfc4x3Typ}, "IFC4X3");
  if (key == "IFC2X3") {
    return kIfc2x3;
  }
  if (key == "IFC4X3") {
    return kIfc4x3;
  }
  return kIfc4;
}

SchemaPick schema_for(std::string_view file_schema) {
  const std::string raw(base::js_trim(base::js_to_upper(file_schema)));
  std::string_view key = "IFC4";
  bool guessed = false;
  if (raw.starts_with("IFC2X3")) {
    key = "IFC2X3";
  } else if (raw.starts_with("IFC4X3")) {
    key = "IFC4X3";
  } else if (!raw.starts_with("IFC4")) {
    guessed = true;
  }
  return SchemaPick{&schema_named(key), guessed};
}

}  // namespace cc::ifc
