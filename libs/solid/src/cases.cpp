// SPDX-License-Identifier: Apache-2.0
// tools/parity/manifold.mjs's cases through SolidOps (cases.hpp).
#include "cc/solid/cases.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <expected>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/solid/solid_ops.hpp"

namespace cc::solid {

namespace {

using base::Value;

constexpr std::size_t kMaxSteps = 10000;
constexpr std::size_t kMaxNumbers = std::size_t{1} << 26U;

std::string hex(std::uint64_t v, int digits) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string s(static_cast<std::size_t>(digits), '0');
  for (int i = digits - 1; i >= 0; --i) {
    s[static_cast<std::size_t>(i)] = kDigits[v & 15U];
    v >>= 4U;
  }
  return s;
}

// A refused mesh ends a case with its status (as the wasm throws ManifoldError); anything else wrong is "bad".
struct Stop {
  bool refused = false;
  std::string why;
};
template <class T>
using R = std::expected<T, Stop>;
std::unexpected<Stop> bad(std::string why) {
  return std::unexpected(Stop{.refused = false, .why = std::move(why)});
}

R<double> num(const Value& v) {
  if (!v.is_number() || !std::isfinite(v.as_number())) {
    return bad("a number is wrong");
  }
  return v.as_number();
}
R<int> count(const Value& v, int hi) {
  const auto d = num(v);
  if (!d || *d < -1 || *d > hi || *d != std::floor(*d)) {
    return bad("a count is wrong");
  }
  return static_cast<int>(*d);
}
R<std::array<double, 3>> vec3(const Value& v) {
  if (!v.is_array() || v.as_array().size() < 3) {
    return bad("a vector is wrong");
  }
  std::array<double, 3> out{};
  for (std::size_t i = 0; i < 3; ++i) {
    const auto d = num(v.as_array()[i]);
    if (!d) {
      return std::unexpected(d.error());
    }
    out.at(i) = *d;
  }
  return out;
}
R<std::vector<double>> nums(const Value& v, std::size_t lo, std::size_t hi) {
  if (!v.is_array() || v.as_array().size() < lo || v.as_array().size() > hi) {
    return bad("a list is wrong");
  }
  std::vector<double> out;
  out.reserve(v.as_array().size());
  for (const Value& x : v.as_array()) {
    if (!x.is_number() || !std::isfinite(x.as_number())) {
      return bad("a number is wrong");
    }
    out.push_back(x.as_number());
  }
  return out;
}
template <class T>
R<T> lift(base::Result<T> r) {
  if (!r) {
    return bad(r.error().message);
  }
  return std::move(*r);
}

R<Value> describe(const Solid& s) {
  auto g = lift(s.mesh());
  if (!g) {
    return std::unexpected(g.error());
  }
  std::string vp;
  vp.reserve(g->vert_properties.size() * 8);
  for (const float f : g->vert_properties) {
    vp += hex(std::bit_cast<std::uint32_t>(f), 8);
  }
  std::string tv;
  for (std::size_t i = 0; i < g->tri_verts.size(); ++i) {
    if (i != 0) {
      tv += ',';
    }
    tv += std::to_string(g->tri_verts[i]);
  }
  return base::make_object({{"status", s.status()},
                            {"empty", s.is_empty()},
                            {"volume", hex(std::bit_cast<std::uint64_t>(s.volume()), 16)},
                            {"numProp", Value(g->num_prop)},
                            {"vp", std::move(vp)},
                            {"tv", std::move(tv)}});
}

// The stack, and every solid made so far: the JS harness keeps each Manifold it made until the case ends, so the
// lazy CSG tree sees the same sharing (Manifold flattens a nested operation only when no other handle holds it).
struct Stack {
  std::vector<Solid> st, made;
  void push_back(Solid s) {
    made.push_back(s);
    st.push_back(std::move(s));
  }
};

R<Solid> pop(Stack& stack) {
  auto& st = stack.st;
  if (st.empty()) {
    return bad("the stack is empty");
  }
  Solid s = std::move(st.back());
  st.pop_back();
  return s;
}

R<void> push(Stack& st, R<Solid> s) {
  if (!s) {
    return std::unexpected(s.error());
  }
  st.push_back(std::move(*s));
  return {};
}

R<void> mesh_step(const Value& s, const Value& m, Stack& st) {
  const auto vp = nums(m.get("vp"), 0, kMaxNumbers);
  const auto tv = nums(m.get("tv"), 0, kMaxNumbers);
  if (!vp || !tv) {
    return bad("a mesh is wrong");
  }
  Mesh mesh;
  mesh.vert_properties.reserve(vp->size());
  for (const double d : *vp) {
    mesh.vert_properties.push_back(static_cast<float>(d));
  }
  for (const double d : *tv) {
    if (d < 0 || d > 4294967295.0 || d != std::floor(d)) {
      return bad("an index is wrong");
    }
    mesh.tri_verts.push_back(static_cast<std::uint32_t>(d));
  }
  if (s.get("merge").truthy()) {
    (void)merge(mesh);
  }
  auto r = Solid::of_mesh(mesh);
  if (!r) {
    return std::unexpected(Stop{.refused = true, .why = r.error().message});
  }
  st.push_back(std::move(*r));
  return {};
}

R<void> step(const Value& s, Stack& st) {
  if (const Value* m = s.find("mesh")) {
    return mesh_step(s, *m, st);
  }
  if (const Value* c = s.find("cube")) {
    const auto v = vec3(*c);
    return v ? push(st, lift(Solid::cube(*v, s.get("center").truthy()))) : std::unexpected(v.error());
  }
  if (const Value* y = s.find("cylinder")) {
    if (!y->is_array() || y->as_array().size() != 5) {
      return bad("a cylinder is wrong");
    }
    const auto& a = y->as_array();
    const auto h = num(a[0]);
    const auto r1 = num(a[1]);
    const auto r2 = num(a[2]);
    const auto n = count(a[3], 256);
    if (!h || !r1 || !r2 || !n) {
      return bad("a cylinder is wrong");
    }
    return push(st, lift(Solid::cylinder(*h, *r1, *r2, *n, a[4].truthy())));
  }
  if (const Value* p = s.find("sphere")) {
    if (!p->is_array() || p->as_array().size() != 2) {
      return bad("a sphere is wrong");
    }
    const auto r = num(p->as_array()[0]);
    const auto n = count(p->as_array()[1], 256);
    if (!r || !n) {
      return bad("a sphere is wrong");
    }
    return push(st, lift(Solid::sphere(*r, *n)));
  }
  if (const Value* t = s.find("translate")) {
    const auto v = vec3(*t);
    auto top = pop(st);
    if (!v || !top) {
      return bad("a translate is wrong");
    }
    return push(st, lift(top->translate(*v)));
  }
  if (const Value* x = s.find("transform")) {
    const auto a = nums(*x, 16, 16);
    auto top = pop(st);
    if (!a || !top) {
      return bad("a transform is wrong");
    }
    std::array<double, 16> m{};
    std::ranges::copy(*a, m.begin());
    return push(st, lift(top->transform(m)));
  }
  if (const Value* o = s.find("op")) {
    auto b = pop(st);
    auto a = pop(st);
    if (!a || !b || !o->is_string()) {
      return bad("an op is wrong");
    }
    const std::string& op = o->as_string();
    if (op == "add") {
      return push(st, lift(a->add(*b)));
    }
    return push(st, lift(op == "subtract" ? a->subtract(*b) : a->intersect(*b)));
  }
  if (s.has("union") || s.has("intersection")) {
    const bool u = s.has("union");
    const auto n = count(s.get(u ? "union" : "intersection"), 100000);
    if (!n || *n < 0 || static_cast<std::size_t>(*n) > st.st.size()) {
      return bad("the stack is short");
    }
    const auto k = static_cast<std::ptrdiff_t>(*n);
    std::vector<Solid> xs(std::make_move_iterator(st.st.end() - k), std::make_move_iterator(st.st.end()));
    st.st.erase(st.st.end() - k, st.st.end());
    return push(st, lift(u ? union_of(xs) : intersection_of(xs)));
  }
  if (s.has("trim") || s.has("split")) {
    const bool trim = s.has("trim");
    const auto a = nums(s.get(trim ? "trim" : "split"), 4, 4);
    auto top = pop(st);
    if (!a || !top) {
      return bad("a plane is wrong");
    }
    const std::array<double, 3> n = {(*a)[0], (*a)[1], (*a)[2]};
    if (trim) {
      return push(st, lift(top->trim_by_plane(n, (*a)[3])));
    }
    auto parts = lift(top->split_by_plane(n, (*a)[3]));
    if (!parts) {
      return std::unexpected(parts.error());
    }
    st.push_back(std::move(parts->first));
    st.push_back(std::move(parts->second));
    return {};
  }
  return bad("an unknown step");
}

Value stopped(const Stop& e) {
  return base::make_object({{e.refused ? "error" : "bad", e.why}});
}

}  // namespace

Value run_case(const Value& steps) {
  if (!steps.is_array() || steps.as_array().size() > kMaxSteps) {
    return stopped(Stop{.refused = false, .why = "the steps are wrong"});
  }
  Stack st;
  for (const Value& s : steps.as_array()) {
    if (!s.is_object()) {
      return stopped(Stop{.refused = false, .why = "a step is not an object"});
    }
    if (auto r = step(s, st); !r) {
      return stopped(r.error());
    }
  }
  base::Array out;
  for (const Solid& s : st.st) {
    auto d = describe(s);
    if (!d) {
      return stopped(d.error());
    }
    out.push_back(std::move(*d));
  }
  return base::make_object({{"out", std::move(out)}});
}

Value run_cases(const Value& cases) {
  base::Array results;
  const Value& list = cases["cases"];
  if (list.is_array()) {
    for (const Value& c : list.as_array()) {
      results.push_back(run_case(c["steps"]));
    }
  }
  return base::make_object({{"results", std::move(results)}});
}

}  // namespace cc::solid
