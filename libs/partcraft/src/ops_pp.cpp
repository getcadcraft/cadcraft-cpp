// SPDX-License-Identifier: Apache-2.0
// The driver's pp.* ops: params-part.js.
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cad/params.hpp"
#include "cc/partcraft/params_part.hpp"
#include "jsx.hpp"
#include "ops.hpp"

namespace cc::partcraft::ops {
namespace {

using base::Value;
namespace pr = cad::params;

template <class T>
T ok_or_throw(base::Result<T> r) {
  if (!r) {
    throw JsError(r.error().message);
  }
  return std::move(*r);
}

Value with_part(Value res, Value part) {
  return base::make_object({{"res", std::move(res)}, {"part", std::move(part)}});
}

/// paramsOf(part): part.params, made when missing.
pr::Params params_of(const Value& part) {
  const Value p = part.get("params");
  return p.truthy() ? pr::from_value(p) : pr::Params{};
}
void store(Value& part, const pr::Params& p) {
  part.set("params", pr::to_value(p));
}

Value edit_value(const pr::Edit& e) {
  if (e.ok) {
    return base::make_object({{"ok", true}});
  }
  Value o = base::make_object({{"ok", false}, {"error", e.error}});
  if (e.cycle) {
    o.append("cycle", true);
  }
  return o;
}

std::vector<std::pair<std::string, double>> overrides_of(const Value& v) {
  std::vector<std::pair<std::string, double>> out;
  for (const auto& [k, x] : jsx::entries(v)) {
    out.emplace_back(k, base::js_to_number(x));
  }
  return out;
}

}  // namespace

void add_pp(Table& t, const i18n::Catalog& words) {
  const i18n::Catalog* w = &words;
  t["pp.targets"] = [w](Args a) {
    Value part = arg(a, 0);
    Value out = Value::object();
    for (const pp::Target& x : pp::targets(part, *w)) {
      out.set(x.key, pp::target_value(x));
    }
    return out;
  };
  t["pp.apply"] = [w](Args a) {
    Value part = arg(a, 0);
    const auto ov = overrides_of(arg(a, 1));
    Value res = pp::apply_part_params(part, arg(a, 1).truthy() ? &ov : nullptr, *w);
    return with_part(std::move(res), std::move(part));
  };
  t["pp.setLink"] = [w](Args a) {
    Value part = arg(a, 0);
    pr::Params p = params_of(part);
    pr::Linked r;
    {
      pp::PartAdapter ad(part, *w);
      r = pr::set_link(p, ad.adapter(), base::js_string(arg(a, 1)), base::js_string(arg(a, 2)), *w);
    }
    store(part, p);
    Value res = r.ok ? base::make_object({{"ok", true}, {"value", r.value}})
                     : base::make_object({{"ok", false}, {"error", r.error}});
    return with_part(std::move(res), std::move(part));
  };
  t["pp.setVar"] = [w](Args a) {
    Value part = arg(a, 0);
    pr::Params p = params_of(part);
    const pr::Edit e =
        pr::set_var(p, base::js_string(arg(a, 1)), base::js_string(arg(a, 2)), std::nullopt, std::nullopt, *w);
    store(part, p);
    return with_part(edit_value(e), std::move(part));
  };
  t["pp.driveBack"] = [w](Args a) {
    Value part = arg(a, 0);
    const bool had = part.get("params").truthy();
    pr::Params p = params_of(part);
    const std::string key = base::js_string(arg(a, 1));
    std::optional<std::string> prefer;
    if (arg(a, 3).truthy()) {
      prefer = base::js_string(arg(a, 3));
    }
    const pr::Evaluated ev0 = pr::evaluate_vars(p, nullptr, *w);
    pr::Drive r;
    {
      pp::PartAdapter ad(part, *w);
      r = pr::drive_back(p, ad.adapter(), key, base::js_to_number(arg(a, 2)), prefer, *w);
    }
    if (had) {
      store(part, p);
    }
    Value res;
    if (r.direct) {
      res = base::make_object({{"ok", true}, {"direct", true}});
    } else if (!r.ok) {
      res = base::make_object({{"ok", false}, {"error", r.error}});
      if (r.has_choices) {
        res.append("choices", base::Array(r.choices.begin(), r.choices.end()));
      }
    } else if (!r.link.empty()) {
      res = base::make_object({{"ok", true}, {"link", r.link}, {"from", r.from_text}, {"to", r.to_text}});
    } else {
      const auto from = ev0.value(r.var);
      res = base::make_object({{"ok", true}, {"var", r.var}, {"from", from ? Value(*from) : Value()}, {"to", r.to}});
    }
    return with_part(std::move(res), std::move(part));
  };
  t["pp.resolveTarget"] = [w](Args a) {
    Value part = arg(a, 0);
    const auto k = pp::resolve_target(part, arg(a, 1), *w);
    return k ? Value(*k) : Value(nullptr);
  };
  t["pp.targetName"] = [w](Args a) {
    Value part = arg(a, 0);
    return Value(pp::target_name(part, base::js_string(arg(a, 1)), *w));
  };
  t["pp.report"] = [w](Args a) {
    Value part = arg(a, 0);
    return pp::params_report(part, *w);
  };
  t["pp.pruneDeleted"] = [w](Args a) {
    Value before = arg(a, 0);
    Value after = arg(a, 1);
    Value res = pp::prune_deleted(before, after, *w);
    return base::make_object({{"res", std::move(res)}, {"after", std::move(after)}});
  };
  t["pp.stepRollback"] = [w](Args a) {
    Value part = arg(a, 0);
    Value res = pp::step_rollback(part, base::js_to_number(arg(a, 1)), *w);
    return with_part(std::move(res), std::move(part));
  };
  t["pp.rollTo"] = [w](Args a) {
    Value part = arg(a, 0);
    Value res = pp::roll_to(part, arg(a, 1), *w);
    return with_part(std::move(res), std::move(part));
  };
  t["pp.rollbackWords"] = [w](Args a) { return Value(pp::rollback_words(arg(a, 0), *w)); };
  t["pp.fieldInUse"] = [](Args a) { return Value(pp::field_in_use(arg(a, 0), arg(a, 1))); };
  t["pp.readPartParams"] = [w](Args a) {
    Value part = arg(a, 0);
    return ok_or_throw(pp::read_part_params(arg(a, 1), part, *w));
  };
}

}  // namespace cc::partcraft::ops
