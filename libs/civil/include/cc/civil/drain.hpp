// SPDX-License-Identifier: Apache-2.0
// CivilCraft 0.5's pipes.js (gravity pipe networks: inverts, cover, the user's rules, automatic inverts) and hydro.js
// (catchments, the rational method, Manning sizing, hydraulic and energy grade lines). Networks come from project files
// and are read as the JS reads them.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/civil/jsmap.hpp"
#include "cc/civil/tin.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::civil {

[[nodiscard]] base::Value default_network(std::string_view units, const i18n::Catalog& words);
/// analyzeNetwork(net, surface, units) as the parity view: { structs, pipes, problems }
[[nodiscard]] base::Value analyze_network(const base::Value& net, const Surface* surface, std::string_view units,
                                          const i18n::Catalog& words);
/// autoInverts(net, surface, units, { only }) → { pipes, problems }; only = nullopt: every pipe
[[nodiscard]] base::Value auto_inverts(const base::Value& net, const Surface* surface, std::string_view units,
                                       const std::optional<std::vector<base::Value>>& only, const i18n::Catalog& words);
/// flowOrder(net): pipe ids in flow order, or nullopt on a loop
[[nodiscard]] std::optional<std::vector<base::Value>> flow_order(const base::Value& net);

// ---- hydro.js
[[nodiscard]] base::Value hydro_defaults(std::string_view units);
[[nodiscard]] base::Value clean_hydro(const base::Value& o, std::string_view units);
[[nodiscard]] base::Value clean_catchment(const base::Value& o);
[[nodiscard]] double catchment_area(const base::Value& pts, std::string_view units);
[[nodiscard]] double idf(double tc, const base::Value& h);
struct Manning {
  double Q, V, A;
};
[[nodiscard]] Manning manning_full(double D, double S, double n, std::string_view units);
struct PartFull {
  double A, P, T;
};
[[nodiscard]] PartFull part_full(double y, double D);
[[nodiscard]] std::optional<double> normal_depth(double Q, double D, double S, double n, std::string_view units);

struct PipeFlow {
  double CA, tc, i, Q, travel, len, S;
};
struct StructFlow {
  double CA = 0, tc = 0;
};
struct Flows {
  JsMap<StructFlow> structs;
  JsMap<PipeFlow> pipes;
  base::Value problems = base::Value::array();
  base::Value hydro;
};
[[nodiscard]] std::shared_ptr<Flows> rational_flows(const base::Value& net, std::string_view units,
                                                    const i18n::Catalog& words);
[[nodiscard]] base::Value flows_value(const Flows& f);
/// sizePipes(net, flows, units, { sizes })
[[nodiscard]] base::Value size_pipes(const base::Value& net, const Flows& flows, std::string_view units,
                                     const base::Value& sizes);
/// hydraulicGrade(net, flows, units, surface) as the parity view: { pipes, structs, problems }
[[nodiscard]] base::Value hydraulic_grade(const base::Value& net, const Flows& flows, std::string_view units,
                                          const Surface* surface, const i18n::Catalog& words);

}  // namespace cc::civil
