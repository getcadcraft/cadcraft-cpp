// SPDX-License-Identifier: Apache-2.0
// Script ops for desckeys.js, clip.js and point-edit.js (tools/parity/civil/ops-civil.mjs has their JS twins).
#include "cc/civil/codes.hpp"
#include "cc/civil/script.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::Value;

std::string order_of(const Value& v) {
  return nullish(v) ? std::string("PNEZD") : str(v);
}

}  // namespace

void add_codes_ops(std::map<std::string, ScriptOp, std::less<>>& m) {
  m["dk.cleanKeys"] = [](const Array& a, ScriptCtx&) -> Value { return clean_desc_keys(arg(a, 0)); };
  m["dk.cleanLineCodes"] = [](const Array& a, ScriptCtx&) -> Value { return clean_line_codes(arg(a, 0)); };
  m["dk.parseDesc"] = [](const Array& a, ScriptCtx&) -> Value {
    return parse_desc(arg(a, 0), nullish(arg(a, 1)) ? Value() : arg(a, 1));
  };
  m["dk.groups"] = [](const Array& a, ScriptCtx&) -> Value {
    Array out;
    for (const Value& p : arr(arg(a, 1))) {
      out.emplace_back(group_of(arg(a, 0), p));
    }
    return out;
  };
  m["dk.figures"] = [](const Array& a, ScriptCtx&) -> Value {
    const Value o = arg(a, 1).truthy() ? arg(a, 1) : Value::object();
    const Value keys = field(o, "keys");
    const Value only_v = field(o, "only");
    std::vector<std::string> only;
    for (const Value& g : arr(only_v)) {
      only.push_back(str(g));
    }
    return figures(arg(a, 0), keys.is_undefined() ? Value::array() : keys, field(o, "lineCodes"),
                   only_v.truthy() ? &only : nullptr);
  };
  m["dk.keyStats"] = [](const Array& a, ScriptCtx&) -> Value { return key_stats(arg(a, 0), arg(a, 1)); };

  m["clip.pointsText"] = [](const Array& a, ScriptCtx&) -> Value {
    return points_text(arg(a, 0), order_of(arg(a, 1)));
  };
  m["clip.make"] = [](const Array& a, ScriptCtx&) -> Value { return make_clip(arg(a, 0), arg(a, 1), 0); };
  m["clip.read"] = [](const Array& a, ScriptCtx&) -> Value { return read_clip(arg(a, 0)); };
  m["clip.textPoints"] = [](const Array& a, ScriptCtx& c) -> Value {
    return text_points(arg(a, 0), order_of(arg(a, 1)), *c.words);
  };
  m["clip.choose"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value own = nullish(arg(a, 0)) ? Value(nullptr) : read_clip(arg(a, 0));
    return choose_paste(own, arg(a, 1), order_of(arg(a, 2)), *c.words);
  };
  m["clip.renumber"] = [](const Array& a, ScriptCtx&) -> Value { return renumber(arg(a, 0), arg(a, 1), arg(a, 2)); };
  m["clip.renumberText"] = [](const Array& a, ScriptCtx& c) -> Value {
    return renumber_text(arg(a, 0), arg(a, 1), *c.words);
  };
  m["clip.moveBy"] = [](const Array& a, ScriptCtx&) -> Value { return move_by(arg(a, 0), arg(a, 1), arg(a, 2)); };
  m["clip.centre"] = [](const Array& a, ScriptCtx&) -> Value { return centre(arg(a, 0)); };

  m["pe.check"] = [](const Array& a, ScriptCtx& c) -> Value {
    return check_point_edit(arg(a, 0), arg(a, 1), nullish(arg(a, 2)) ? Value() : arg(a, 2), *c.words);
  };
}

}  // namespace cc::civil
