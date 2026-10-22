// SPDX-License-Identifier: Apache-2.0
// The driver's shapes.* ops: lib/shapes (sections.js, parts.js, items.js).
#include <string>

#include "cc/base/js.hpp"
#include "cc/shapes/items.hpp"
#include "cc/shapes/parts.hpp"
#include "cc/shapes/sections.hpp"
#include "ops.hpp"

namespace cc::partcraft::ops {
namespace {

shapes::Loop loop_of(const base::Value& v) {
  shapes::Loop l;
  if (!v.is_array()) {
    return l;
  }
  for (const base::Value& p : v.as_array()) {
    const auto num = [&](std::size_t i) {
      return p.is_array() && i < p.as_array().size() ? base::js_to_number(p.as_array()[i])
                                                     : std::numeric_limits<double>::quiet_NaN();
    };
    l.push_back({num(0), num(1)});
  }
  return l;
}

const shapes::Section* named(const base::Value& n) {
  return n.is_string() ? shapes::find_section(n.as_string()) : nullptr;
}

base::Value rows_value(const std::vector<const shapes::Section*>& rows) {
  base::Array out;
  out.reserve(rows.size());
  for (const auto* s : rows) {
    out.push_back(shapes::to_value(*s));
  }
  return out;
}

/// The JS takes any segment count, and runs out of memory (or time) on a huge one; the fork stops at 4096 (an arc of
/// 4096 pieces, a tube of 32768 points: far past what a drawing asks for). docs/gaps.md.
constexpr double kMaxSeg = 4096;
double seg_of(const base::Value& v) {
  const double seg = v.is_undefined() ? 6 : base::js_to_number(v);
  if (seg > kMaxSeg) {
    throw JsError("seg over 4096 (the fork's cap)");
  }
  return seg;
}

/// makePart's opts as the driver hands them over: null = left out, "$null" = a real null.
base::Value part_opts(const base::Value& v) {
  if (v.is_null()) {
    return {};
  }
  if (v.is_string() && v.as_string() == "$null") {
    return nullptr;
  }
  return v;
}

}  // namespace

void add_shapes(Table& t) {
  t["shapes.constants"] = [](Args) {
    return base::make_object({{"STEEL_DENSITY", shapes::kSteelDensity},
                              {"IN", shapes::kIn},
                              {"LBFT", shapes::kLbFt},
                              {"PARTS_STEEL_DENSITY", shapes::kSteelDensity}});
  };
  t["shapes.sources"] = [](Args) { return shapes::sources_value(); };
  t["shapes.families"] = [](Args) { return shapes::families_value(); };
  t["shapes.sectionRows"] = [](Args) {
    base::Array out;
    for (const auto& s : shapes::section_rows()) {
      out.push_back(shapes::to_value(s));
    }
    return base::Value(std::move(out));
  };
  t["shapes.findSection"] = [](Args a) {
    const shapes::Section* s = named(arg(a, 0));
    return s ? shapes::to_value(*s) : base::Value(nullptr);
  };
  t["shapes.familyRows"] = [](Args a) {
    const base::Value& id = arg(a, 0);
    return id.is_string() ? rows_value(shapes::family_rows(id.as_string())) : base::Value(base::Array{});
  };
  t["shapes.displayName"] = [](Args a) { return base::Value(shapes::display_name(base::js_string(arg(a, 0)))); };
  t["shapes.exactArea"] = [](Args a) { return base::Value(shapes::exact_area(shapes::from_value(arg(a, 0)))); };
  t["shapes.outline"] = [](Args a) {
    return shapes::loops_value(shapes::outline(shapes::from_value(arg(a, 0)), seg_of(opt(a, 1))));
  };
  t["shapes.bbox"] = [](Args a) { return shapes::box_value(shapes::bbox(loop_of(arg(a, 0)))); };
  t["shapes.shoelace"] = [](Args a) { return base::Value(shapes::shoelace(loop_of(arg(a, 0)))); };
  t["shapes.loopLength"] = [](Args a) { return base::Value(shapes::loop_length(loop_of(arg(a, 0)))); };
  t["shapes.partStandards"] = [](Args) { return shapes::standards_value(); };
  t["shapes.partFamilies"] = [](Args) { return shapes::families_part_value(); };
  t["shapes.findFamily"] = [](Args a) {
    const base::Value& id = arg(a, 0);
    if (id.is_string()) {
      for (const base::Value& f : shapes::families_part_value().as_array()) {
        if (f.get("id").as_string() == id.as_string()) {
          return f;
        }
      }
    }
    return base::Value(nullptr);
  };
  t["shapes.partSizes"] = [](Args a) {
    const base::Value& id = arg(a, 0);
    return id.is_string() ? shapes::part_sizes(id.as_string()) : base::Value(base::Array{});
  };
  t["shapes.partOptions"] = [](Args a) {
    const base::Value& id = arg(a, 0);
    return id.is_string() ? shapes::part_options(id.as_string(), arg(a, 1)) : base::Value::object();
  };
  t["shapes.makePart"] = [](Args a) {
    const base::Value& id = arg(a, 0);
    return shapes::make_part(id.is_string() ? id.as_string() : std::string_view{"\x01"}, arg(a, 1),
                             part_opts(arg(a, 2)));
  };
  t["shapes.librarySet"] = [](Args) { return shapes::library_set(); };
  t["shapes.partTags"] = [](Args) { return shapes::part_tags(); };
  t["shapes.cc0"] = [](Args) { return shapes::cc0(); };
  t["shapes.memberLoops"] = [](Args a) {
    const shapes::Section* s = named(arg(a, 0));
    if (!s) {
      throw_read(base::Value(nullptr), "kind");
    }
    return shapes::loops_value(shapes::member_loops(*s));
  };
  t["shapes.sectionItem"] = [](Args a) {
    const shapes::Section* s = named(arg(a, 0));
    if (!s) {
      throw_read(base::Value(nullptr), "source");
    }
    auto r = shapes::section_item(*s, opt(a, 1));
    if (!r) {
      throw JsError(r.error().message);
    }
    return std::move(*r);
  };
  t["shapes.partItem"] = [](Args a) {
    const base::Value& id = arg(a, 0);
    base::Value r = shapes::make_part(id.is_string() ? id.as_string() : std::string_view{"\x01"}, arg(a, 1), opt(a, 2));
    return r.get("ok").truthy() ? shapes::part_item(r.get("part")) : r;
  };
  t["shapes.localItem"] = [](Args a) { return shapes::local_item(arg(a, 0)); };
}

}  // namespace cc::partcraft::ops
