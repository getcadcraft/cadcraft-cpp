// SPDX-License-Identifier: Apache-2.0
// The driver's vec.*, isect.normAng and ref.* ops (vec.js, refgeom.js; ops-stage2.mjs, ops-stage3.mjs).
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/partcraft/refgeom.hpp"
#include "cc/partcraft/vec.hpp"
#include "jsx.hpp"
#include "ops.hpp"

namespace cc::partcraft::ops {
namespace {

using base::Value;
namespace V = vec;

double num(const Value& v) {
  return base::js_to_number(v);
}

std::vector<double> numbers(const Value& v) {
  std::vector<double> out;
  if (v.is_array()) {
    out.reserve(v.as_array().size());
    for (const Value& x : v.as_array()) {
      out.push_back(num(x));
    }
  }
  return out;
}

/// The driver's lookup tables: res[tab][JSON.stringify(ref)], else "unresolved <ref>".
refgeom::Resolvers tables(const Value& res) {
  const auto look = [res](const char* tab, const Value& r) {
    std::string k = jsx::stringify(r, 0);
    const Value t = res.is_object() ? res.get(tab) : Value{};
    if (!t.is_object() || !t.has(k)) {
      throw JsError("unresolved " + k);
    }
    return t.get(k);
  };
  const auto dir_of = [](const Value& v) { return refgeom::Dir{V::v3(v.get("o")), V::v3(v.get("d"))}; };
  refgeom::Resolvers R;
  R.plane = [look](const Value& r) {
    const Value v = look("planes", r);
    refgeom::Plane p{V::frame(v), std::nullopt};
    if (v.get("center").truthy()) {
      p.center = V::v3(v.get("center"));
    }
    return p;
  };
  R.dir = [look, dir_of](const Value& r) { return dir_of(look("dirs", r)); };
  R.point = [look](const Value& r) { return V::v3(look("points", r)); };
  R.face = [look, dir_of](const Value& r) {
    const Value v = look("faces", r);
    refgeom::Face f;
    f.type = base::js_string(v.get("type"));
    if (!v.get("n").is_undefined()) {
      f.n = V::v3(v.get("n"));
    }
    if (!v.get("o").is_undefined()) {
      f.o = V::v3(v.get("o"));
    }
    if (v.get("axis").truthy()) {
      f.axis = dir_of(v.get("axis"));
    }
    f.r = num(v.get("r"));
    return f;
  };
  R.edge = [look, dir_of](const Value& r) {
    const Value v = look("edges", r);
    refgeom::Edge e;
    e.type = base::js_string(v.get("type"));
    e.a = V::v3(v.get("a"));
    e.b = V::v3(v.get("b"));
    if (v.get("axis").truthy()) {
      e.axis = dir_of(v.get("axis"));
    }
    return e;
  };
  R.fail = [](const std::string& m) { throw JsError(m); };
  return R;
}

}  // namespace

void add_ref(Table& t, const i18n::Catalog& words) {
  t["vec.basePlanes"] = [](Args) {
    return base::make_object({{"front", V::value(V::base_plane("front"))},
                              {"top", V::value(V::base_plane("top"))},
                              {"right", V::value(V::base_plane("right"))}});
  };
  t["vec.frameFor"] = [](Args a) { return V::value(V::frame_for(V::v3(arg(a, 0)), V::v3(arg(a, 1)))); };
  t["vec.norm"] = [](Args a) { return V::value(V::norm(V::v3(arg(a, 0)))); };
  t["vec.rotationAbout"] = [](Args a) {
    return V::value(V::rotation_about(V::v3(arg(a, 0)), V::v3(arg(a, 1)), num(arg(a, 2))));
  };
  t["vec.reflection"] = [](Args a) { return V::value(V::reflection(V::v3(arg(a, 0)), V::v3(arg(a, 1)))); };
  t["vec.matMul"] = [](Args a) { return V::value(V::mat_mul(V::m4(arg(a, 0)), V::m4(arg(a, 1)))); };
  t["vec.applyMat"] = [](Args a) { return V::value(V::apply_mat(V::m4(arg(a, 0)), V::v3(arg(a, 1)))); };
  t["vec.applyDir"] = [](Args a) { return V::value(V::apply_dir(V::m4(arg(a, 0)), V::v3(arg(a, 1)))); };
  t["vec.frameMatrix"] = [](Args a) { return V::value(V::frame_matrix(V::frame(arg(a, 0)))); };
  t["vec.toPlane"] = [](Args a) {
    const auto p = V::to_plane(V::frame(arg(a, 0)), V::v3(arg(a, 1)));
    return Value(base::Array{p[0], p[1]});
  };
  t["vec.toWorld"] = [](Args a) { return V::value(V::to_world(V::frame(arg(a, 0)), num(arg(a, 1)), num(arg(a, 2)))); };
  t["vec.flipFrame"] = [](Args a) { return V::value(V::flip_frame(V::frame(arg(a, 0)))); };
  t["vec.solveDense"] = [](Args a) {
    const Value& A = arg(a, 0);
    const std::vector<double> b = numbers(arg(a, 1));
    std::vector<std::vector<double>> rows;
    if (A.is_array()) {
      for (const Value& r : A.as_array()) {
        rows.push_back(numbers(r));
      }
    }
    if (rows.size() < b.size()) {
      throw_read(Value{}, std::to_string(rows.size()));
    }
    const auto x = V::solve_dense(rows, b);
    if (!x) {
      return Value(nullptr);
    }
    base::Array out;
    for (const double v : *x) {
      out.emplace_back(v);
    }
    return Value(std::move(out));
  };
  t["vec.fmt"] = [](Args a) {
    const Value& n = arg(a, 0);
    const Value dp = opt(a, 1);
    if (!n.is_number()) {  // Number.isFinite does not convert
      return Value("");
    }
    return Value(V::fmt(n.as_number(), dp.is_undefined() ? 4 : static_cast<int>(num(dp))));
  };
  t["isect.normAng"] = [](Args a) {
    constexpr double kTau = 2 * std::numbers::pi;
    double x = std::fmod(num(arg(a, 0)), kTau);
    if (x < 0) {
      x += kTau;
    }
    return Value(x);
  };
  t["ref.feature"] = [&words](Args a) {
    return refgeom::to_value(refgeom::ref_feature(arg(a, 0), tables(arg(a, 1)), words));
  };
  t["ref.planeQuad"] = [](Args a) {
    const Value& g = arg(a, 0);
    return refgeom::plane_quad(V::frame(g.get("frame")), V::v3(g.get("center")), num(arg(a, 1)));
  };
}

}  // namespace cc::partcraft::ops
