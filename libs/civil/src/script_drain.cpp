// SPDX-License-Identifier: Apache-2.0
// Script ops for pipes.js and hydro.js (tools/parity/civil/ops-civil.mjs has their JS twins).
#include <memory>

#include "cc/civil/drain.hpp"
#include "cc/civil/script.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::make_object;
using base::Value;

std::string units_of(const Value& v) {
  return v.is_string() && v.as_string() == "m" ? "m" : "ft";
}
std::shared_ptr<Surface> surface_or_null(const ScriptCtx& c, const Value& name) {
  return nullish(name) ? nullptr : reg_get<Surface>(c, str(name));
}

}  // namespace

void add_drain_ops(std::map<std::string, ScriptOp, std::less<>>& m) {
  m["pipes.analyze"] = [](const Array& a, ScriptCtx& c) -> Value {
    return analyze_network(arg(a, 0), surface_or_null(c, arg(a, 1)).get(), units_of(arg(a, 2)), *c.words);
  };
  m["pipes.autoInverts"] = [](const Array& a, ScriptCtx& c) -> Value {
    std::optional<std::vector<Value>> only;
    const Value o = field(arg(a, 3), "only");
    if (o.truthy()) {
      only = std::vector<Value>(arr(o).begin(), arr(o).end());
    }
    return auto_inverts(arg(a, 0), surface_or_null(c, arg(a, 1)).get(), units_of(arg(a, 2)), only, *c.words);
  };
  m["pipes.flowOrder"] = [](const Array& a, ScriptCtx&) -> Value {
    const auto o = flow_order(arg(a, 0));
    if (!o) {
      return nullptr;
    }
    Array ids;
    for (const Value& p : *o) {
      ids.push_back(field(p, "id"));
    }
    return ids;
  };
  m["pipes.defaultNetwork"] = [](const Array& a, ScriptCtx& c) -> Value {
    return default_network(units_of(arg(a, 0)), *c.words);
  };

  m["hydro.defaults"] = [](const Array& a, ScriptCtx&) -> Value { return hydro_defaults(units_of(arg(a, 0))); };
  m["hydro.clean"] = [](const Array& a, ScriptCtx&) -> Value { return clean_hydro(arg(a, 0), units_of(arg(a, 1))); };
  m["hydro.cleanCatchment"] = [](const Array& a, ScriptCtx&) -> Value { return clean_catchment(arg(a, 0)); };
  m["hydro.area"] = [](const Array& a, ScriptCtx&) -> Value { return catchment_area(arg(a, 0), units_of(arg(a, 1))); };
  m["hydro.idf"] = [](const Array& a, ScriptCtx&) -> Value { return idf(num(arg(a, 0)), arg(a, 1)); };
  m["hydro.manningFull"] = [](const Array& a, ScriptCtx&) -> Value {
    const Manning r = manning_full(num(arg(a, 0)), num(arg(a, 1)), num(arg(a, 2)), units_of(arg(a, 3)));
    return make_object({{"Q", r.Q}, {"V", r.V}, {"A", r.A}});
  };
  m["hydro.partFull"] = [](const Array& a, ScriptCtx&) -> Value {
    const PartFull r = part_full(num(arg(a, 0)), num(arg(a, 1)));
    return make_object({{"A", r.A}, {"P", r.P}, {"T", r.T}});
  };
  m["hydro.normalDepth"] = [](const Array& a, ScriptCtx&) -> Value {
    const auto y = normal_depth(num(arg(a, 0)), num(arg(a, 1)), num(arg(a, 2)), num(arg(a, 3)), units_of(arg(a, 4)));
    return y ? Value(*y) : Value(nullptr);
  };
  m["hydro.flows"] = [](const Array& a, ScriptCtx& c) -> Value {
    auto f = rational_flows(arg(a, 0), units_of(arg(a, 1)), *c.words);
    c.keep(f);
    return flows_value(*f);
  };
  m["hydro.size"] = [](const Array& a, ScriptCtx& c) -> Value {
    const auto f = reg_get<Flows>(c, str(arg(a, 1)));
    if (!f) {
      throw ScriptError{"Cannot read properties of undefined (reading 'hydro')"};
    }
    return size_pipes(arg(a, 0), *f, units_of(arg(a, 2)), field(arg(a, 3), "sizes"));
  };
  m["hydro.grade"] = [](const Array& a, ScriptCtx& c) -> Value {
    const auto f = reg_get<Flows>(c, str(arg(a, 1)));
    if (!f) {
      throw ScriptError{"Cannot read properties of undefined (reading 'hydro')"};
    }
    return hydraulic_grade(arg(a, 0), *f, units_of(arg(a, 2)), surface_or_null(c, arg(a, 3)).get(), *c.words);
  };
}

}  // namespace cc::civil
