// SPDX-License-Identifier: Apache-2.0
// gsolve.js and params.js: a few solves and a parameter chain driven backwards (the command scripts compare the rest
// with CadCraft 0.5 bit for bit).
#include "cc/cad/gsolve.hpp"

#include <cmath>
#include <set>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "cc/cad/params.hpp"
#include "cc/i18n/i18n.hpp"

namespace gs = cc::cad::gsolve;
namespace pr = cc::cad::params;
using cc::base::Value;

namespace {
Value line(double x1, double y1, double x2, double y2) {
  return cc::base::make_object({{"type", "line"}, {"x1", x1}, {"y1", y1}, {"x2", x2}, {"y2", y2}});
}
Value constraint(const char* id, const char* type, cc::base::Array refs) {
  return cc::base::make_object({{"id", id}, {"type", type}, {"refs", Value(std::move(refs))}});
}
Value ref(double e, const char* k) {
  return cc::base::make_object({{"e", e}, {"k", k}});
}
}  // namespace

TEST_CASE("gsolve: a tilted line made horizontal moves as little as it can; a pinned end stays") {
  const Value a = line(0, 0, 100, 10);
  const gs::Getter get = [&a](double id) { return id == 1 ? &a : nullptr; };
  const cc::base::Array cs{constraint("k1", "horizontal", {ref(1, "L")})};
  const gs::SolveResult r = gs::solve_system(cs, get);
  REQUIRE(r.ok);
  REQUIRE(r.updates.size() == 1);
  const Value& e = r.updates.front().second;
  CHECK(std::abs(e.get("y1").as_number() - e.get("y2").as_number()) < 1e-6);  // met to 1e-9 × the drawing's size
  CHECK(std::abs(e.get("y1").as_number() - 5) < 1e-6);  // the least-norm step: both ends move half way
  const std::set<std::string> pinned{"1|x1", "1|y1"};
  const gs::SolveResult held = gs::solve_system(cs, get, &pinned);
  REQUIRE(held.ok);
  CHECK(std::abs(held.updates.front().second.get("y2").as_number()) < 1e-6);
  CHECK(gs::residual_ok(cs, [&](double id) { return id == 1 ? &e : nullptr; }));
}

TEST_CASE("gsolve: analysis counts degrees of freedom and finds a redundant constraint") {
  const Value a = line(0, 0, 10, 0);
  const gs::Getter get = [&a](double id) { return id == 1 ? &a : nullptr; };
  const cc::base::Array cs{constraint("k1", "horizontal", {ref(1, "L")}),
                           constraint("k2", "horizontal", {ref(1, "p1"), ref(1, "p2")})};
  const gs::Analysis an = gs::analyze(cs, get);
  CHECK(an.dof == 3);
  CHECK(an.redundant == std::vector<std::string>{"k2"});
  CHECK(gs::components(cs).size() == 1);
}

// (a cycle is found by its message, as params.js does: tests/commands/constraints.json covers it with the English
// words)
TEST_CASE("params: a driven value solved back through a chain; removing a used one refused unless written in") {
  const cc::i18n::Catalog words;
  pr::Params p;
  p.vars = {{.name = "a", .expr = "10", .note = std::nullopt}, {.name = "b", .expr = "a*2", .note = std::nullopt}};
  const pr::Drive d = pr::drive_var(p, "b", 50, std::nullopt, words);
  REQUIRE(d.ok);
  CHECK(d.var == "a");
  CHECK(p.vars[0].expr == "25");
  const pr::Edit e = pr::remove_var(p, "a", false, words);
  CHECK_FALSE(e.ok);
  CHECK(e.used);
  CHECK(pr::remove_var(p, "a", true, words).ok);
  CHECK(p.vars.size() == 1);
  CHECK(p.vars[0].expr == "(25)*2");
}
