// SPDX-License-Identifier: Apache-2.0
#include "cc/doc/linetypes.hpp"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cad/save_parts.hpp"

namespace cc::doc {

using base::Value;

namespace {

/// linetypes.js STANDARD (inches): name → pattern; "2" half size, "X2" double size
const std::vector<std::pair<std::string, std::vector<double>>>& standard_linetypes() {
  static const auto kV = [] {
    const std::vector<std::pair<std::string, std::vector<double>>> base_lt = {
        {"DASHED", {0.5, -0.25}},
        {"HIDDEN", {0.25, -0.125}},
        {"CENTER", {1.25, -0.25, 0.25, -0.25}},
        {"PHANTOM", {1.25, -0.25, 0.25, -0.25, 0.25, -0.25}},
        {"DOT", {0, -0.25}},
        {"DASHDOT", {0.5, -0.25, 0, -0.25}},
        {"BORDER", {0.5, -0.25, 0.5, -0.25, 0, -0.25}},
        {"DIVIDE", {0.5, -0.25, 0, -0.25, 0, -0.25}},
    };
    std::vector<std::pair<std::string, std::vector<double>>> out;
    const auto X = [](double k, const std::vector<double>& p) {
      std::vector<double> o;
      o.reserve(p.size());
      for (const double v : p) {
        o.push_back(v * k);
      }
      return o;
    };
    for (const auto& [n, p] : base_lt) {
      out.emplace_back(n, p);
      out.emplace_back(n + "2", X(0.5, p));
      out.emplace_back(n + "X2", X(2, p));
    }
    return out;
  }();
  return kV;
}

}  // namespace

Value pattern_of(const Doc& doc, const std::string& name) {
  const std::string low = base::js_to_lower(name);
  if (name.empty() || low == "continuous" || low == "bylayer" || low == "byblock") {
    return {nullptr};
  }
  const Value* own = doc.linetypes.get(name);
  if (own == nullptr) {
    const std::string up = base::js_to_upper(name);
    doc.linetypes.each([&](const std::string&, const Value& l) {
      if (own == nullptr && base::js_to_upper(base::js_string(l.get("name"))) == up) {
        own = &l;
      }
    });
  }
  if (own != nullptr) {
    const Value p = own->get("pattern");
    bool ok = p.is_array() && p.as_array().size() >= 2;
    double sum = 0;
    bool neg = false;
    if (ok) {
      for (const Value& v : p.as_array()) {
        ok = ok && v.is_number() && std::isfinite(v.as_number());
        sum += std::abs(base::js_to_number(v));
        neg = neg || base::js_to_number(v) < 0;
      }
    }
    return ok && sum > 1e-12 && neg ? p : Value(nullptr);
  }
  const std::string up = base::js_to_upper(name);
  for (const auto& [n, pat] : standard_linetypes()) {
    if (n == up) {
      const bool metric = cad::hatch_is_metric(Value::object(), doc.meta);
      base::Array out;
      for (const double v : pat) {
        out.emplace_back(metric ? v * 25.4 : v);
      }
      return out;
    }
  }
  return {nullptr};
}

}  // namespace cc::doc
