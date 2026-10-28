// SPDX-License-Identifier: Apache-2.0
// The driver's sketch.* ops: sketch.js and solver.js.
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/partcraft/sketch.hpp"
#include "cc/partcraft/spline.hpp"
#include "jsx.hpp"
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

Value with_sketch(Value res, Value sk) {
  return base::make_object({{"res", std::move(res)}, {"sk", std::move(sk)}});
}

Value opts_of(Args a, std::size_t i) {
  const Value& o = arg(a, i);
  return o.truthy() ? o : Value::object();
}

}  // namespace

void add_sketch(Table& t, const i18n::Catalog& words) {
  const i18n::Catalog* w = &words;
  t["sketch.read"] = [w](Args a) { return ok_or_throw(sketch::read_sketch(arg(a, 0), *w)); };
  t["sketch.solve"] = [w](Args a) {
    Value sk = arg(a, 0);
    std::vector<std::pair<std::string, std::array<double, 2>>> pin;
    for (const auto& [k, v] : jsx::entries(arg(a, 1))) {
      const auto& arr = v.is_array() ? v.as_array() : base::Array{};
      pin.emplace_back(k, std::array<double, 2>{arr.empty() ? std::nan("") : base::js_to_number(arr[0]),
                                                arr.size() < 2 ? std::nan("") : base::js_to_number(arr[1])});
    }
    std::vector<std::pair<std::string, double>> pin_r;
    for (const auto& [k, v] : jsx::entries(arg(a, 2))) {
      pin_r.emplace_back(k, base::js_to_number(v));
    }
    Value res = ok_or_throw(sketch::solve_sketch(sk, pin, pin_r, *w));
    return with_sketch(std::move(res), std::move(sk));
  };
  t["sketch.analyze"] = [w](Args a) {
    const Value ts = opt(a, 1);
    return ok_or_throw(sketch::analyze(
        arg(a, 0), ts.is_undefined() ? std::nullopt : std::optional<double>(base::js_to_number(ts)), *w));
  };
  t["sketch.residuals"] = [w](Args a) {
    base::Array out;
    for (const auto& [cid, f] : ok_or_throw(sketch::residuals(arg(a, 0), *w))) {
      out.emplace_back(base::Array{cid, f});
    }
    return Value(std::move(out));
  };
  t["sketch.profile"] = [w](Args a) {
    const Value seg = arg(a, 1);
    return ok_or_throw(sketch::profile(arg(a, 0), seg.is_undefined() ? 64 : base::js_to_number(seg), *w));
  };
  t["sketch.measure"] = [](Args a) { return ok_or_throw(sketch::measure(arg(a, 0), arg(a, 1), arg(a, 2))); };
  t["sketch.matchSig"] = [](Args a) { return ok_or_throw(sketch::match_sig(arg(a, 0), arg(a, 1), arg(a, 2))); };
  t["sketch.applicable"] = [](Args a) { return ok_or_throw(sketch::applicable(arg(a, 0), arg(a, 1))); };
  t["sketch.addConstraint"] = [w](Args a) {
    Value sk = arg(a, 0);
    Value res = ok_or_throw(sketch::add_constraint(sk, arg(a, 1), arg(a, 2), opt(a, 3), opts_of(a, 4), *w));
    return with_sketch(std::move(res), std::move(sk));
  };
  t["sketch.addPoint"] = [](Args a) {
    Value sk = arg(a, 0);
    std::string id = sketch::add_point(sk, base::js_to_number(arg(a, 1)), base::js_to_number(arg(a, 2)), opts_of(a, 3));
    return with_sketch(Value(std::move(id)), std::move(sk));
  };
  t["sketch.addLine"] = [w](Args a) {
    Value sk = arg(a, 0);
    Value res = ok_or_throw(sketch::add_line(sk, arg(a, 1), arg(a, 2), opts_of(a, 3), *w));
    return with_sketch(std::move(res), std::move(sk));
  };
  t["sketch.addCircle"] = [w](Args a) {
    Value sk = arg(a, 0);
    Value res = ok_or_throw(sketch::add_circle(sk, arg(a, 1), arg(a, 2), opts_of(a, 3), *w));
    return with_sketch(std::move(res), std::move(sk));
  };
  t["sketch.addArc"] = [w](Args a) {
    Value sk = arg(a, 0);
    Value res = ok_or_throw(sketch::add_arc(sk, arg(a, 1), arg(a, 2), arg(a, 3), opts_of(a, 4), *w));
    return with_sketch(std::move(res), std::move(sk));
  };
  t["sketch.addRect"] = [w](Args a) {
    Value sk = arg(a, 0);
    Value res = ok_or_throw(sketch::add_rect(sk, arg(a, 1), arg(a, 2), opts_of(a, 3), *w));
    return with_sketch(std::move(res), std::move(sk));
  };
  t["sketch.addSpline"] = [w](Args a) {
    Value sk = arg(a, 0);
    Value res = ok_or_throw(sketch::add_spline(sk, arg(a, 1), opts_of(a, 2), *w));
    return with_sketch(std::move(res), std::move(sk));
  };
  t["sketch.removeEntities"] = [](Args a) {
    Value sk = arg(a, 0);
    Value res = ok_or_throw(sketch::remove_entities(sk, arg(a, 1)));
    return with_sketch(std::move(res), std::move(sk));
  };
  t["sketch.removeConstraints"] = [](Args a) {
    Value sk = arg(a, 0);
    sketch::remove_constraints(sk, arg(a, 1));
    return with_sketch(nullptr, std::move(sk));
  };
  t["sketch.filletCorner"] = [w](Args a) {
    Value sk = arg(a, 0);
    Value res = ok_or_throw(sketch::fillet_corner(sk, arg(a, 1), base::js_to_number(arg(a, 2)), *w));
    return with_sketch(std::move(res), std::move(sk));
  };
  t["sketch.describe"] = [w](Args a) { return sketch::describe(arg(a, 0), *w); };
  t["sketch.extent"] = [](Args a) { return Value(sketch::extent(arg(a, 0))); };
  t["sketch.bounds"] = [w](Args a) { return ok_or_throw(sketch::bounds(arg(a, 0), *w)); };
  t["sketch.insideLoops"] = [](Args a) {
    const auto p = spline::points_of(base::Array{arg(a, 1)});
    return Value(sketch::inside_loops(arg(a, 0), p.at(0)));
  };
  t["sketch.loopArea"] = [](Args a) { return Value(sketch::loop_area(arg(a, 0))); };
  t["sketch.loosePoints"] = [](Args a) { return ok_or_throw(sketch::loose_points(arg(a, 0))); };
  t["sketch.tessArc"] = [](Args a) {
    const Value seg = arg(a, 2);
    return ok_or_throw(sketch::tess_arc(arg(a, 0), sketch::by_id(arg(a, 0), arg(a, 1)),
                                        seg.is_undefined() ? 64 : base::js_to_number(seg)));
  };
  t["sketch.tessSpline"] = [w](Args a) {
    const Value per = arg(a, 2);
    return ok_or_throw(sketch::tess_spline(arg(a, 0), sketch::by_id(arg(a, 0), arg(a, 1)),
                                           per.is_undefined() ? 16 : base::js_to_number(per), *w));
  };
  t["sketch.endMeet"] = [](Args a) {
    return ok_or_throw(
        sketch::end_meet(arg(a, 0), sketch::by_id(arg(a, 0), arg(a, 1)), sketch::by_id(arg(a, 0), arg(a, 2))));
  };
}

}  // namespace cc::partcraft::ops
