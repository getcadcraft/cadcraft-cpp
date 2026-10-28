// SPDX-License-Identifier: Apache-2.0
// model.js (cc-ifc, CadCraft 0.5) in C++: see cc/ifc/model.hpp. step.js's writing half (formatString, formatReal,
// formatValue) is here too, beside save().
#include "cc/ifc/model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/reader.hpp"
#include "cc/base/value.hpp"

namespace cc::ifc {
namespace {

constexpr std::size_t kCacheMax = 250000;
constexpr std::uint32_t kNoSlot = 0xFFFFFFFFU;

/// A ref's number as a map key (JS Map keys are the numbers; an absurd one never names a line).
Id ref_id(double n) {
  return base::clamp_cast<Id>(n);
}

std::string hex_pad(std::uint32_t v, std::size_t width) {
  std::string h = base::hex_upper(v);
  if (h.size() < width) {
    h.insert(0, width - h.size(), '0');
  }
  return h;
}

// NOLINTNEXTLINE(misc-no-recursion) — bounded by the parser's depth cap
void walk_refs(const Raw& v, Id from, std::unordered_map<Id, std::vector<Id>>& refs) {
  if (v.kind == Raw::Kind::list) {
    for (const Raw& x : v.items) {
      walk_refs(x, from, refs);
    }
  } else if (v.kind == Raw::Kind::ref) {
    refs[ref_id(v.num)].push_back(from);
  } else if (v.kind == Raw::Kind::typed && !v.items.empty()) {
    walk_refs(v.items[0], from, refs);
  }
}

}  // namespace

// ---------------------------------------------------------------- Part 21 text

std::string format_string(std::string_view s) {
  std::string out = "'";
  std::string wide;
  auto end_wide = [&] {
    if (!wide.empty()) {
      out += "\\X2\\" + wide + "\\X0\\";
      wide.clear();
    }
  };
  const std::vector<std::uint16_t> u = base::to_utf16(s);
  for (std::size_t i = 0; i < u.size(); ++i) {
    std::uint32_t cp = u[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < u.size() && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
      cp = 0x10000U + ((cp - 0xD800U) << 10U) + (u[i + 1] - 0xDC00U);
      ++i;
    }
    if (cp >= 0x20 && cp <= 0x7E) {
      end_wide();
      if (cp == '\'') {
        out += "''";
      } else if (cp == '\\') {
        out += "\\\\";
      } else {
        out += static_cast<char>(cp);
      }
      continue;
    }
    if (cp > 0xFFFF) {
      end_wide();
      out += "\\X4\\" + hex_pad(cp, 8) + "\\X0\\";
      continue;
    }
    wide += hex_pad(cp, 4);
  }
  end_wide();
  return out + "'";
}

base::Result<std::string> format_real(double x) {
  if (!std::isfinite(x)) {
    return base::fail(base::Errc::invalid, "A real value must be finite");
  }
  if (x == 0) {
    x = 0;  // -0 → 0
  }
  std::string s;
  base::append_number(s, x);
  const std::size_t e = s.find('e');
  if (e != std::string::npos) {
    std::string m = s.substr(0, e);
    std::string ex = s.substr(e + 1);
    if (const std::size_t p = ex.find('+'); p != std::string::npos) {
      ex.erase(p, 1);
    }
    return (m.contains('.') ? m : m + ".") + "E" + ex;
  }
  return s.contains('.') ? s : s + ".";
}

namespace {
base::Result<std::string> format_integer(double x) {
  if (!std::isfinite(x)) {
    return base::fail(base::Errc::invalid, "An integer value must be finite");
  }
  std::string s;
  base::append_number(s, base::js_round(x) == 0 ? 0.0 : base::js_round(x));
  return s;
}
}  // namespace

// NOLINTNEXTLINE(misc-no-recursion) — bounded by the value's depth
base::Result<std::string> format_value(const Raw& v, const std::function<bool(std::string_view)>& real_at,
                                       const std::function<bool(std::string_view)>& real_type,
                                       const std::string& path) {
  switch (v.kind) {
    case Raw::Kind::null: return std::string("$");
    case Raw::Kind::derived: return std::string("*");
    case Raw::Kind::number:
      if (real_at(path)) {
        return format_real(v.num);
      }
      return std::isfinite(v.num) && v.num == std::trunc(v.num) ? format_integer(v.num) : format_real(v.num);
    case Raw::Kind::string: return format_string(v.text);
    case Raw::Kind::list: {
      std::string out = "(";
      for (std::size_t k = 0; k < v.items.size(); ++k) {
        auto x = format_value(v.items[k], real_at, real_type, path + "/" + std::to_string(k));
        if (!x) {
          return x;
        }
        if (k > 0) {
          out += ',';
        }
        out += *x;
      }
      return out + ")";
    }
    case Raw::Kind::ref: {
      std::string out = "#";
      base::append_number(out, v.num);
      return out;
    }
    case Raw::Kind::enumeration: return "." + v.text + ".";
    case Raw::Kind::binary: return "\"" + v.text + "\"";
    case Raw::Kind::typed: {
      const bool is_real = real_type(v.text);
      Raw inner;
      const Raw& value = v.items.empty() ? inner : v.items[0];
      auto x = format_value(value, [is_real](std::string_view) { return is_real; }, real_type, "");
      if (!x) {
        return x;
      }
      return v.text + "(" + *x + ")";
    }
  }
  return base::fail(base::Errc::invalid, "Cannot write value");
}

// ---------------------------------------------------------------- the file

StepResult<std::unique_ptr<IfcFile>> IfcFile::open(std::vector<std::byte> bytes) {
  auto ix = scan_step(bytes);
  if (!ix) {
    return std::unexpected(std::move(ix.error()));
  }
  return std::make_unique<IfcFile>(std::move(bytes), std::move(*ix));
}

IfcFile::IfcFile(std::vector<std::byte> bytes, StepIndex ix)
    : bytes_(std::move(bytes)),
      ix_(std::move(ix)),
      max_id_(ix_.max_id),
      slots_(ix_.max_id <= static_cast<double>(ix_.count) * 4 + 1000000) {
  const SchemaPick pick = schema_for(ix_.schema);
  schema_ = pick.schema;
  guessed_ = pick.guessed;
  if (slots_) {
    slot_.assign(static_cast<std::size_t>(std::max(0.0, ix_.max_id)) + 1, kNoSlot);
    for (std::size_t i = 0; i < ix_.count; ++i) {
      if (ix_.ids[i] >= 0) {
        slot_[static_cast<std::size_t>(ix_.ids[i])] = static_cast<std::uint32_t>(i);
      }
    }
  } else {
    rec_.reserve(ix_.count);
    for (std::size_t i = 0; i < ix_.count; ++i) {
      rec_[ix_.ids[i]] = i;
    }
  }
  cache_.resize(ix_.count);
}

std::ptrdiff_t IfcFile::rec(Id id) const {
  if (slots_) {
    if (id < 0 || static_cast<std::uint64_t>(id) >= slot_.size()) {
      return -1;
    }
    const std::uint32_t r = slot_[static_cast<std::size_t>(id)];
    return r == kNoSlot ? -1 : static_cast<std::ptrdiff_t>(r);
  }
  const auto it = rec_.find(id);
  return it == rec_.end() ? -1 : static_cast<std::ptrdiff_t>(it->second);
}

std::optional<std::string_view> IfcFile::type(Id id) const {
  if (!gone_.empty() && gone_.contains(id)) {
    return std::nullopt;
  }
  if (!edits_.empty()) {
    if (const auto e = edits_.find(id); e != edits_.end()) {
      return e->second.type;
    }
  }
  const std::ptrdiff_t r = rec(id);
  if (r < 0) {
    return std::nullopt;
  }
  return ix_.names[ix_.types[static_cast<std::size_t>(r)]];
}

Params IfcFile::params(Id id) {
  if (!gone_.empty() && gone_.contains(id)) {
    return nullptr;
  }
  if (!edits_.empty()) {
    if (const auto e = edits_.find(id); e != edits_.end()) {
      return e->second.params;
    }
  }
  const std::ptrdiff_t r = rec(id);
  if (r < 0) {
    return nullptr;
  }
  const auto ri = static_cast<std::size_t>(r);
  if (cache_[ri]) {
    return cache_[ri];
  }
  if (ix_.types[ri] == 0) {
    return nullptr;  // complex: use complex(id)
  }
  auto p = parse_params(bytes_, ix_.starts[ri], ix_.ends[ri]);
  if (!p) {
    throw ParseFailure(std::move(p.error()));
  }
  if (cached_ >= kCacheMax) {
    std::ranges::fill(cache_, nullptr);
    cached_ = 0;
  }
  Params out = std::make_shared<const RawList>(std::move(*p));
  cache_[ri] = out;
  ++cached_;
  return out;
}

std::optional<std::vector<ComplexPart>> IfcFile::complex(Id id) const {
  if (gone_.contains(id) || edits_.contains(id)) {
    return std::nullopt;
  }
  const std::ptrdiff_t r = rec(id);
  if (r < 0 || ix_.types[static_cast<std::size_t>(r)] != 0) {
    return std::nullopt;
  }
  auto p = parse_complex(bytes_, ix_.starts[static_cast<std::size_t>(r)], ix_.ends[static_cast<std::size_t>(r)]);
  if (!p) {
    throw ParseFailure(std::move(p.error()));
  }
  return std::move(*p);
}

std::vector<Id> IfcFile::new_ids() const {
  std::vector<Id> out;
  for (const Id id : edit_order_) {
    if (rec(id) < 0) {
      out.push_back(id);
    }
  }
  std::ranges::sort(out);
  return out;
}

std::vector<Id> IfcFile::ids() const {
  std::vector<Id> out;
  out.reserve(ix_.count);
  for (std::size_t i = 0; i < ix_.count; ++i) {
    const Id id = ix_.ids[i];
    if (!gone_.contains(id) && (!edits_.contains(id) || rec(id) == static_cast<std::ptrdiff_t>(i))) {
      out.push_back(id);
    }
  }
  for (const Id id : new_ids()) {
    out.push_back(id);
  }
  return out;
}

void IfcFile::index_types() {
  if (by_type_ready_) {
    return;
  }
  std::vector<std::vector<Id>> lists(ix_.names.size());
  for (std::size_t i = 0; i < ix_.count; ++i) {
    lists[ix_.types[i]].push_back(ix_.ids[i]);
  }
  by_type_ready_ = true;
  for (std::size_t t = 0; t < ix_.names.size(); ++t) {
    by_type_.emplace(ix_.names[t], std::move(lists[t]));
  }
}

std::vector<Id> IfcFile::of_type(std::string_view up, bool inherited) {
  std::vector<std::string_view> one{up};
  const std::vector<std::string_view>& types = inherited ? schema_->subtypes(up) : one;
  index_types();
  std::vector<Id> out;
  for (const std::string_view t : types) {
    if (const auto it = by_type_.find(t); it != by_type_.end()) {
      for (const Id id : it->second) {
        if (!gone_.contains(id) && !edits_.contains(id)) {
          out.push_back(id);
        }
      }
    }
  }
  if (!edits_.empty()) {
    const std::unordered_set<std::string_view> want(types.begin(), types.end());
    for (const Id id : edit_order_) {
      if (want.contains(edits_.at(id).type)) {
        out.push_back(id);
      }
    }
  }
  if (types.size() > 1 || !edits_.empty()) {
    std::ranges::sort(out);
  }
  return out;
}

std::vector<std::pair<std::string, std::size_t>> IfcFile::types_present() {
  index_types();
  std::vector<std::pair<std::string, std::size_t>> out;
  std::unordered_map<std::string, std::size_t> at;
  for (const std::string& t : ix_.names) {  // the Map's order: names in first-seen order
    if (t.empty()) {
      continue;
    }
    std::size_t n = 0;
    for (const Id id : by_type_.at(t)) {
      if (!gone_.contains(id) && !edits_.contains(id)) {
        ++n;
      }
    }
    if (n > 0) {
      at.emplace(t, out.size());
      out.emplace_back(t, n);
    }
  }
  for (const Id id : edit_order_) {
    const std::string& t = edits_.at(id).type;
    if (const auto it = at.find(t); it != at.end()) {
      ++out[it->second].second;
    } else {
      at.emplace(t, out.size());
      out.emplace_back(t, 1);
    }
  }
  return out;
}

AttrValue IfcFile::attr(Id id, std::string_view name) {
  const auto t = type(id);
  if (!t || t->empty()) {
    return {};
  }
  const Entity* e = schema_->entity(*t);
  if (e == nullptr) {
    return {};
  }
  const std::ptrdiff_t k = e->at(name);
  if (k < 0) {
    return {};
  }
  Params p = params(id);
  if (!p || static_cast<std::size_t>(k) >= p->size()) {
    return {};
  }
  const Raw* v = &(*p)[static_cast<std::size_t>(k)];
  return AttrValue{std::move(p), v};
}

const std::vector<Id>& IfcFile::refs_to(Id id) {
  static const std::vector<Id> kNone;
  if (!refs_ || refs_version_ != version_) {
    std::unordered_map<Id, std::vector<Id>> refs;
    for (const Id i : ids()) {
      if (Params p = params(i)) {
        for (const Raw& v : *p) {
          walk_refs(v, i, refs);
        }
      } else if (auto parts = complex(i)) {
        for (const ComplexPart& q : *parts) {
          for (const Raw& v : q.params) {
            walk_refs(v, i, refs);
          }
        }
      }
    }
    refs_ = std::move(refs);
    refs_version_ = version_;
  }
  const auto it = refs_->find(id);
  return it == refs_->end() ? kNone : it->second;
}

const std::unordered_map<Id, std::vector<Id>>& IfcFile::inverse(std::string_view up, std::string_view attr_name,
                                                                bool inherited) {
  std::string key = std::string(up) + "." + std::string(attr_name) + (inherited ? "+" : "");
  if (inv_version_ != version_) {
    inv_.clear();
    inv_version_ = version_;
  }
  if (const auto it = inv_.find(key); it != inv_.end()) {
    return it->second;
  }
  std::unordered_map<Id, std::vector<Id>> m;
  for (const Id id : of_type(up, inherited)) {
    const AttrValue v = attr(id, attr_name);
    if (v.undefined()) {
      continue;
    }
    auto add = [&](const Raw& x) {
      if (x.kind == Raw::Kind::ref) {
        m[ref_id(x.num)].push_back(id);
      }
    };
    if (v->kind == Raw::Kind::list) {
      for (const Raw& x : v->items) {
        add(x);
      }
    } else {
      add(*v);
    }
  }
  return inv_.emplace(std::move(key), std::move(m)).first->second;
}

// ---------------------------------------------------------------- writing

base::Result<Id> IfcFile::set(double id, std::string_view type, RawList params) {
  if (!std::isfinite(id) || id != std::trunc(id) || id <= 0 || id > 9007199254740991.0) {
    return base::fail(base::Errc::invalid, "A line id must be a positive whole number.");
  }
  const auto n = static_cast<Id>(id);
  gone_.erase(n);
  if (!edits_.contains(n)) {
    edit_order_.push_back(n);
  }
  edits_[n] = Edit{base::js_to_upper(type), std::make_shared<const RawList>(std::move(params))};
  if (const std::ptrdiff_t r = rec(n); r >= 0 && cache_[static_cast<std::size_t>(r)]) {
    cache_[static_cast<std::size_t>(r)] = nullptr;
    --cached_;
  }
  max_id_ = std::max(id, max_id_);
  ++version_;
  return n;
}

bool IfcFile::remove(Id id) {
  if (!has(id)) {
    return false;
  }
  if (edits_.erase(id) > 0) {
    std::erase(edit_order_, id);
  }
  if (const std::ptrdiff_t r = rec(id); r >= 0 && cache_[static_cast<std::size_t>(r)]) {
    cache_[static_cast<std::size_t>(r)] = nullptr;
    --cached_;
  }
  if (rec(id) >= 0) {
    gone_.insert(id);
  }
  ++version_;
  return true;
}

bool IfcFile::real_at(std::string_view type, std::size_t k) const {
  const Entity* e = schema_->entity(type);
  if (e == nullptr || k >= e->attrs.size()) {
    return false;
  }
  const std::string_view b = e->attrs[k].t.base;
  return b == "r" || b == "n";
}

base::Result<std::string> IfcFile::format_line(Id id, std::string_view type, const RawList& params) const {
  std::string out = "#" + std::to_string(id) + "=" + std::string(type) + "(";
  auto real_type = [this](std::string_view n) { return schema_->is_real_type(n); };
  for (std::size_t k = 0; k < params.size(); ++k) {
    const bool real = real_at(type, k);
    auto x = format_value(params[k], [real](std::string_view) { return real; }, real_type);
    if (!x) {
      return x;
    }
    if (k > 0) {
      out += ',';
    }
    out += *x;
  }
  return out + ");";
}

base::Result<std::string> IfcFile::header_text(std::string_view time_stamp) const {
  auto find = [this](std::string_view k) -> const RawList* {
    for (const auto& [name, v] : ix_.header_raw) {
      if (name == k) {
        return &v;
      }
    }
    return nullptr;
  };
  auto str = [](std::string s) {
    Raw r;
    r.kind = Raw::Kind::string;
    r.text = std::move(s);
    return r;
  };
  auto list = [](RawList items) {
    Raw r;
    r.kind = Raw::Kind::list;
    r.items = std::move(items);
    return r;
  };
  const auto no_real = [](std::string_view) { return false; };
  const auto real_type = [](std::string_view) { return true; };
  auto join = [&](const RawList& vs) -> base::Result<std::string> {
    std::string out;
    for (std::size_t k = 0; k < vs.size(); ++k) {
      auto x = format_value(vs[k], no_real, real_type);
      if (!x) {
        return x;
      }
      if (k > 0) {
        out += ',';
      }
      out += *x;
    }
    return out;
  };
  const RawList* d = find("FILE_DESCRIPTION");
  const RawList desc = d != nullptr ? *d : RawList{list({str("ViewDefinition [CoordinationView]")}), str("2;1")};
  const RawList* nm = find("FILE_NAME");
  RawList name =
      nm != nullptr ? *nm : RawList{str(""), str(""), list({str("")}), list({str("")}), str(""), str(""), str("")};
  if (!time_stamp.empty()) {
    while (name.size() < 2) {
      name.emplace_back();  // name[1] = … on a shorter list: the hole is written as $
    }
    name[1] = str(std::string(time_stamp));
  }
  const RawList* sc = find("FILE_SCHEMA");
  const RawList schema =
      sc != nullptr ? *sc : RawList{list({str(ix_.schema.empty() ? std::string(schema_->name()) : ix_.schema)})};
  std::string out;
  for (const auto& [label, vs] : {std::pair<std::string_view, const RawList*>{"FILE_DESCRIPTION", &desc},
                                  {"FILE_NAME", &name},
                                  {"FILE_SCHEMA", &schema}}) {
    auto j = join(*vs);
    if (!j) {
      return j;
    }
    out += std::string(label) + "(" + *j + ");\n";
  }
  // the others in Object.entries order: array-index keys first, ascending, then as first seen
  std::vector<const std::pair<std::string, RawList>*> rest;
  for (const auto& e : ix_.header_raw) {
    if (e.first != "FILE_DESCRIPTION" && e.first != "FILE_NAME" && e.first != "FILE_SCHEMA") {
      rest.push_back(&e);
    }
  }
  std::ranges::stable_sort(rest, [](const auto* a, const auto* b) {
    const bool ia = base::js_index_key(a->first);
    const bool ib = base::js_index_key(b->first);
    if (ia != ib) {
      return ia;
    }
    return ia && (a->first.size() != b->first.size() ? a->first.size() < b->first.size() : a->first < b->first);
  });
  for (const auto* e : rest) {
    auto j = join(e->second);
    if (!j) {
      return j;
    }
    out += e->first + "(" + *j + ");\n";
  }
  return out;
}

base::Result<std::string> IfcFile::save(std::string_view time_stamp) {
  std::string out = "ISO-10303-21;\nHEADER;\n";
  auto h = header_text(time_stamp);
  if (!h) {
    return h;
  }
  out += *h;
  out += "ENDSEC;\nDATA;\n";
  const std::string_view text = base::as_text(bytes_);
  for (std::size_t i = 0; i < ix_.count; ++i) {
    const Id id = ix_.ids[i];
    if (gone_.contains(id)) {
      continue;
    }
    if (const auto e = edits_.find(id); e != edits_.end()) {
      if (rec(id) == static_cast<std::ptrdiff_t>(i)) {
        auto l = format_line(id, e->second.type, *e->second.params);
        if (!l) {
          return l;
        }
        out += *l + "\n";
      }
      continue;
    }
    out += "#" + std::to_string(id) + "=" + ix_.names[ix_.types[i]];
    out += text.substr(ix_.starts[i], ix_.ends[i] + 1 - ix_.starts[i]);
    out += '\n';
  }
  for (const Id id : new_ids()) {
    const Edit& e = edits_.at(id);
    auto l = format_line(id, e.type, *e.params);
    if (!l) {
      return l;
    }
    out += *l + "\n";
  }
  out += "ENDSEC;\nEND-ISO-10303-21;\n";
  return out;
}

}  // namespace cc::ifc
