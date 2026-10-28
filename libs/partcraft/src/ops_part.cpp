// SPDX-License-Identifier: Apache-2.0
// The driver's part.* ops: part.js.
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/partcraft/part.hpp"
#include "ops.hpp"

namespace cc::partcraft::ops {
namespace {

using base::Value;

template <class T>
T ok_or_throw(base::Result<T> r) {
  if (!r) {
    throw JsError(r.error().message);
  }
  return std::move(*r);
}

Value with_part(std::string_view key, Value res, Value part) {
  Value o = Value::object();
  o.append(key, std::move(res));
  o.append("part", std::move(part));
  return o;
}

}  // namespace

void add_part(Table& t, const i18n::Catalog& words) {
  const i18n::Catalog* w = &words;
  t["part.read"] = [w](Args a) { return ok_or_throw(part::read_part(arg(a, 0), *w)); };
  t["part.toJSON"] = [](Args a) { return Value(part::to_json(arg(a, 0))); };
  t["part.fileVersion"] = [](Args a) { return Value(part::file_version(arg(a, 0))); };
  t["part.new"] = [](Args a) { return part::new_part(arg(a, 0)); };
  t["part.addFeature"] = [](Args a) {
    Value p = arg(a, 0);
    const Value props = arg(a, 2).truthy() ? arg(a, 2) : Value::object();
    Value f = ok_or_throw(part::add_feature(p, arg(a, 1), props));
    return with_part("f", std::move(f), std::move(p));
  };
  t["part.dependsOn"] = [](Args a) {
    const auto d = ok_or_throw(part::depends_on(arg(a, 0)));
    return Value(base::Array(d.begin(), d.end()));
  };
  t["part.dependents"] = [](Args a) { return ok_or_throw(part::dependents(arg(a, 0), arg(a, 1))); };
  t["part.canMove"] = [w](Args a) {
    return ok_or_throw(part::can_move(arg(a, 0), arg(a, 1), base::js_to_number(arg(a, 2)), *w));
  };
  t["part.moveFeature"] = [w](Args a) {
    Value p = arg(a, 0);
    Value res = ok_or_throw(part::move_feature(p, arg(a, 1), base::js_to_number(arg(a, 2)), *w));
    return with_part("res", std::move(res), std::move(p));
  };
  t["part.deleteFeatures"] = [](Args a) {
    Value p = arg(a, 0);
    Value names = ok_or_throw(part::delete_features(p, arg(a, 1)));
    return with_part("names", std::move(names), std::move(p));
  };
  t["part.builtCount"] = [](Args a) { return Value(part::built_count(arg(a, 0))); };
  t["part.readDrawing"] = [w](Args a) { return ok_or_throw(part::read_drawing(arg(a, 0), *w)); };
}

}  // namespace cc::partcraft::ops
