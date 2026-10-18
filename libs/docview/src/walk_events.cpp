// SPDX-License-Identifier: Apache-2.0
// walk() as plain events (tools/parity/walk.mjs, the golden test, `cadcraft-cli walk`).
#include <string>
#include <utility>
#include <vector>

#include "cc/docview/walk.hpp"

namespace cc::docview {

/// walk.js events of a space, as tools/parity/walk.mjs records them
base::Value walk_events(const doc::Doc& d, const doc::EntityMap& space) {
  using base::Array;
  using base::Value;
  std::vector<const Value*> ents;
  space.each([&](const doc::Id&, const Value& e) { ents.push_back(&e); });
  Array ev;
  const auto alpha = [](const Opts& o) { return o.alpha ? Value(*o.alpha) : Value(nullptr); };
  Out out;
  out.stroke = [&](const std::string& color, const std::vector<cad::Prim>& ps, double lw, const Opts& o) {
    Array prims;
    prims.reserve(ps.size());
    for (const cad::Prim& q : ps) {
      prims.emplace_back(q.arc ? Array{"A", q.cx, q.cy, q.r, q.a0, q.a1, q.full ? 1 : 0, q.rev ? 1 : 0}
                               : Array{"L", q.x1, q.y1, q.x2, q.y2});
    }
    ev.emplace_back(Array{"s", color, lw, alpha(o), std::move(prims)});
  };
  out.fill = [&](const std::string& color, const std::vector<std::vector<double>>& loops, bool solid, const Opts& o) {
    Array ls;
    for (const auto& l : loops) {
      Array a;
      for (const double v : l) {
        a.emplace_back(v);
      }
      ls.emplace_back(std::move(a));
    }
    ev.emplace_back(Array{"f", color, solid, alpha(o), o.wipe, std::move(ls)});
  };
  out.text = [&](const std::string& color, const Value& e, const Opts& o) {
    ev.emplace_back(Array{"t", color, alpha(o), e});
  };
  out.point = [&](const std::string& color, double x, double y) { ev.emplace_back(Array{"p", color, x, y}); };
  out.viewport = [&](const Value& e) { ev.emplace_back(Array{"v", e.get("id")}); };
  out.mask = [&](const std::string& color, const std::vector<double>& loop, bool wipe) {
    Array a;
    for (const double v : loop) {
      a.emplace_back(v);
    }
    ev.emplace_back(Array{"m", color, wipe, std::move(a)});
  };
  out.infinite = [&](const std::string& color, const Value& e, double lw, const Opts& o) {
    ev.emplace_back(Array{"i", color, lw, alpha(o), e.get("type"), e.get("x"), e.get("y"), e.get("dx"), e.get("dy")});
  };
  walk(d, ents, out);
  return ev;
}

}  // namespace cc::docview
