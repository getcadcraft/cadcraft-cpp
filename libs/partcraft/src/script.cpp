// SPDX-License-Identifier: Apache-2.0
#include "cc/partcraft/script.hpp"

#include <exception>
#include <string>

#include "ops.hpp"

namespace cc::partcraft {
namespace {

ops::Table table(const i18n::Catalog& words) {
  ops::Table x;
  ops::add_shapes(x);
  ops::add_params(x, words);
  ops::add_modules(x, words);
  ops::add_sketch(x, words);
  ops::add_pp(x, words);
  ops::add_part(x, words);
  return x;
}

}  // namespace

base::Value run_script(const base::Value& script, const i18n::Catalog& words) {
  const ops::Table ops_table = table(words);
  base::Array out;
  const base::Value calls = script.is_object() ? script.get("calls") : base::Value{};
  if (!calls.is_array()) {
    return out;
  }
  for (const base::Value& c : calls.as_array()) {
    const base::Value op = c.is_object() ? c.get("op") : base::Value{};
    const std::string name = op.is_string() ? op.as_string() : base::js_string(op);
    const auto it = ops_table.find(name);
    if (it == ops_table.end()) {
      out.push_back(base::make_object({{"err", "unknown op " + name}}));
      continue;
    }
    const base::Value a = c.get("args");
    const base::Array none;
    const base::Array& args = a.is_array() ? a.as_array() : none;
    try {
      out.push_back(base::make_object({{"ok", it->second(args)}}));
    } catch (const std::exception& e) {
      out.push_back(base::make_object({{"err", std::string(e.what())}}));
    }
  }
  return out;
}

}  // namespace cc::partcraft
