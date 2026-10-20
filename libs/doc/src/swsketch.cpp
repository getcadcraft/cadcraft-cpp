// SPDX-License-Identifier: Apache-2.0
// swsketch.js (CadCraft 0.5), step for step: see swsketch.hpp.
#include "cc/doc/swsketch.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/json.hpp"
#include "cc/cad/expr.hpp"
#include "cc/cad/gsolve.hpp"
#include "cc/cad/params.hpp"
#include "cc/doc/gcons.hpp"
#include "cc/doc/params_doc.hpp"

namespace cc::doc::swsketch {

namespace {

namespace gs = cad::gsolve;
namespace gc = doc::gcons;
namespace ex = cad::expr;
namespace pa = cad::params;
using base::Value;

constexpr double kTau = 2 * std::numbers::pi;

struct UnitRow {
  std::string_view unit;
  double mm;
};
constexpr std::array<UnitRow, 9> kUnitMm{{{"", 1},
                                          {"millimeters", 1},
                                          {"centimeters", 10},
                                          {"meters", 1000},
                                          {"inches", 25.4},
                                          {"feet", 304.8},
                                          {"yards", 914.4},
                                          {"mils", 0.0254},
                                          {"microinches", 0.0000254}}};
// swLengthUnit_e: swMM 0, swCM 1, swMETER 2, swINCHES 3, swFEET 4
struct SwUnit {
  std::string_view unit;
  int sw;
};
constexpr std::array<SwUnit, 6> kSwUnit{
    {{"", 0}, {"millimeters", 0}, {"centimeters", 1}, {"meters", 2}, {"inches", 3}, {"feet", 4}}};
// swUserPreferenceIntegerValue_e.swUnitsLinear = 41 · swUserPreferenceStringValue_e.swDefaultTemplatePart = 8 ·
// swUserPreferenceToggle_e.swInputDimValOnCreate = 10 · swUserPreferenceOption_e.swDetailingNoOptionSpecified = 0
constexpr int kSwUnitsLinear = 41;
constexpr int kSwDefaultTemplatePart = 8;
constexpr int kSwInputDimValOnCreate = 10;
constexpr int kSwDetailingNoOptionSpecified = 0;

struct Pair {
  std::string_view a, b;
};
constexpr std::array<Pair, 9> kRel{{{"coincident", "sgCOINCIDENT"},
                                    {"parallel", "sgPARALLEL"},
                                    {"perpendicular", "sgPERPENDICULAR"},
                                    {"tangent", "sgTANGENT"},
                                    {"concentric", "sgCONCENTRIC"},
                                    {"collinear", "sgCOLINEAR"},
                                    {"equal", "sgSAMELENGTH"},
                                    {"symmetric", "sgSYMMETRIC"},
                                    {"fix", "sgFIXED"}}};
constexpr std::array<Pair, 9> kSwFuncs{{{"sin", "sin"},
                                        {"cos", "cos"},
                                        {"tan", "tan"},
                                        {"atan", "atn"},
                                        {"sqrt", "sqr"},
                                        {"abs", "abs"},
                                        {"sign", "sgn"},
                                        {"exp", "exp"},
                                        {"int", "int"}}};
template <std::size_t N>
std::optional<std::string_view> lookup(const std::array<Pair, N>& t, std::string_view k) {
  // NOLINTNEXTLINE(readability-qualified-auto)
  const auto it = std::ranges::find_if(t, [k](const Pair& p) { return p.a == k; });
  return it == t.end() ? std::nullopt : std::optional<std::string_view>(it->b);
}

double num(const Value& v) {
  return base::js_to_number(v);
}
std::string str(const Value& v) {
  return base::js_string(v);
}
std::string js_num(double v) {
  std::string s;
  base::append_number(s, v);
  return s;
}

/// `num`: Number(v.toPrecision(15)) as JS writes it; an exponent form as v.toExponential(14) with "E"
std::string num15(double v) {
  std::string e = base::js_to_exponential(v, 14);
  std::string s = js_num(base::js_number(e));
  if (s.find_first_of("eE") == std::string::npos) {
    return s;
  }
  std::ranges::replace(e, 'e', 'E');
  return e;
}

/// s.slice(0, n) in UTF-16 units (a pair that does not fit is left out)
std::string slice16(std::string_view s, std::size_t n) {
  std::size_t units = 0;
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t len = 1;
    if (c >= 0xF0) {
      len = 4;
    } else if (c >= 0xE0) {
      len = 3;
    } else if (c >= 0xC0) {
      len = 2;
    }
    const std::size_t u = len == 4 ? 2 : 1;
    if (units + u > n) {
      break;
    }
    units += u;
    i = std::min(s.size(), i + len);
  }
  return std::string(s.substr(0, i));
}

std::string join(const std::vector<std::string>& v, std::string_view sep) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i > 0) {
      out += sep;
    }
    out += v[i];
  }
  return out;
}

const Value* entity(const Doc& doc, const Value& id) {
  const double d = num(id);
  if (!std::isfinite(d) || d < 0) {
    return nullptr;
  }
  return doc.model()->get(static_cast<Id>(d));
}

/// wholeKey(e)
std::string whole_key(const Value* e) {
  const std::string t = e != nullptr ? str(e->get("type")) : std::string();
  if (t == "line") {
    return "L";
  }
  if (t == "circle" || t == "arc") {
    return "R";
  }
  if (t == "point") {
    return "p";
  }
  return "null";
}

struct XY {
  double x = 0, y = 0;
};
XY bulge_centre(XY p, XY q, double b) {
  const double th = 4 * std::atan(b);
  const double c = std::hypot(q.x - p.x, q.y - p.y);
  const double r = std::abs(c / (2 * std::sin(th / 2)));
  const double h = r * std::cos(th / 2) * (b > 0 ? 1 : -1);
  const double ux = (q.x - p.x) / c;
  const double uy = (q.y - p.y) / c;
  return {(p.x + q.x) / 2 - uy * h, (p.y + q.y) / 2 + ux * h};
}

double ang(double y, double x) {
  double a = std::atan2(y, x);
  if (a < 0) {
    a += kTau;
  }
  return a;
}

bool finite(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number()) && std::abs(v.as_number()) < 1e9;
}
/// /^[\w.@-]{1,60}$/
bool id_ok(const Value& v) {
  if (!v.is_string()) {
    return false;
  }
  const std::string& s = v.as_string();
  return !s.empty() && s.size() <= 60 && std::ranges::all_of(s, [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' ||
           ch == '@' || ch == '-';
  });
}
bool word_char(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
}

struct Limits {
  std::size_t features = 5000, points = 50000, segments = 50000, relations = 50000, dimensions = 20000, globals = 2000,
              equations = 5000;
};
constexpr Limits kLim{};

/// a list that may be absent (`x ?? []`)
Value list_or_empty(const Value& v) {
  return v.is_undefined() || v.is_null() ? Value::array() : v;
}

struct Checked {
  bool ok = false;
  std::string error;
  std::vector<std::size_t> sketches;  // the features that are sketches
};

}  // namespace

std::string safe_chars(std::string_view s) {
  std::string out;
  bool run = false;
  for (const char ch : s) {
    if (word_char(ch) || ch == '.' || ch == '-') {
      out += ch;
      run = false;
    } else if (!run) {
      out += '_';
      run = true;
    }
  }
  return out;
}

namespace {

std::string safe(std::string_view s) {
  std::string r = safe_chars(s).substr(0, 80);
  return r.empty() ? std::string("Drawing1") : r;
}

// i18n-ignore-start: the macro's VBA text and its own log text are English, as in the JS
// NOLINTNEXTLINE(readability-function-cognitive-complexity,misc-no-recursion) — over the parsed AST (depth ≤ 100)
std::string sw_node(const ex::Node& n, int ctx, const std::function<std::string(const std::string&)>& quote,
                    std::optional<std::string>& bad, const i18n::Catalog& words) {
  const auto prec = [](std::string_view op) {
    if (op == "+" || op == "-") {
      return 1;
    }
    if (op == "^") {
      return 3;
    }
    return 2;
  };
  switch (n.kind) {
    case ex::Node::Kind::num: return n.name == "pi" ? std::string("pi") : num15(n.v);
    case ex::Node::Kind::var: return quote(n.name);
    case ex::Node::Kind::neg:
      return "-" + (n.args.empty() ? std::string("0") : sw_node(*n.args.front(), 4, quote, bad, words));
    case ex::Node::Kind::cmp:
      if (!bad) {
        bad = words.t("swsketch.comparisons_have_no_solidworks");
      }
      return "0";
    case ex::Node::Kind::bin: {
      if (n.op == "%") {
        if (!bad) {
          bad = words.t("swsketch.remainder_operator_has_no");
        }
        return "0";
      }
      if (n.args.size() < 2) {
        return "0";
      }
      const int p = prec(n.op);
      std::string s = sw_node(*n.args[0], p, quote, bad, words);
      s += " " + n.op + " ";
      s += sw_node(*n.args[1], p + (n.op == "^" ? 0 : 1), quote, bad, words);
      return p < ctx ? "(" + s + ")" : s;
    }
    case ex::Node::Kind::call: {
      const auto f = lookup(kSwFuncs, n.name);
      if (!f) {
        if (!bad) {
          bad = words.t("swsketch.fn_noform", {{"fn", n.name}});
        }
        return "0";
      }
      std::vector<std::string> args;
      args.reserve(n.args.size());
      for (const auto& a : n.args) {
        args.push_back(sw_node(*a, 0, quote, bad, words));
      }
      return std::string(*f) + "(" + join(args, ", ") + ")";
    }
  }
  if (!bad) {
    bad = words.t("swsketch.unknown_expression");
  }
  return "0";
}

}  // namespace

SwExpr sw_expr(std::string_view src, const std::function<std::string(const std::string&)>& quote,
               const i18n::Catalog& words) {
  const ex::Parsed r = ex::parse(src, std::nullopt, words);
  if (!r.ok || !r.ast) {
    return {.ok = false, .text = {}, .error = r.error};
  }
  std::optional<std::string> bad;
  std::string text = sw_node(*r.ast, 0, quote, bad, words);
  if (bad) {
    return {.ok = false, .text = {}, .error = *bad};
  }
  return {.ok = true, .text = std::move(text), .error = {}};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — docToSwb, step for step
Swb to_swb(const Doc& doc, std::string_view name, std::string_view date, const i18n::Catalog& words) {
  Swb out;
  const Value u = doc.meta.get("units");
  const std::string unit = u.truthy() ? str(u) : std::string();
  // NOLINTNEXTLINE(readability-qualified-auto)
  const auto um = std::ranges::find_if(kUnitMm, [&unit](const UnitRow& r) { return r.unit == unit; });
  const double scale = um != kUnitMm.end() ? um->mm : 1;
  if (um == kUnitMm.end()) {
    out.notes.push_back(words.t("swsketch.units_nomatch", {{"unit", unit}}));
  }
  // NOLINTNEXTLINE(readability-qualified-auto)
  const auto su = std::ranges::find_if(kSwUnit, [&unit](const SwUnit& r) { return r.unit == unit; });
  const int sw_unit = su != kSwUnit.end() ? su->sw : 0;
  if (su == kSwUnit.end()) {
    out.notes.push_back(words.t("swsketch.part_mm"));
  }
  // drawing units → metres (the API's unit); solver noise below 1e-9 dropped
  const auto M = [scale](double v) {
    const double a = base::js_number(base::js_to_fixed(v, 9));
    return num15(base::js_number(base::js_to_fixed(a * scale / 1000, 12)));
  };
  Counts& n = out.counts;
  int skipped = 0;

  // ---- geometry: one statement list; every sketch entity gets an index in segs() or pts()
  std::vector<std::string> geo;
  std::map<std::string, std::size_t> seg_of;  // "<id>|L" / "<id>|R" / "<id>|s<i>" → segs index
  std::map<std::string, std::string> pt_of;   // "<id>|<k>" → VBA expression for the point object
  std::vector<std::pair<std::string, std::string>> joins;
  std::size_t ns = 0;
  std::size_t np = 0;
  const auto segs = [](std::size_t i) { return "segs(" + std::to_string(i) + ")"; };
  const auto seg = [&](const std::string& stmt, const std::string& what) {
    const std::size_t i = ns++;
    geo.push_back("Set " + segs(i) + " = " + stmt);
    geo.push_back("If " + segs(i) + R"( Is Nothing Then LogFail ")" + what + R"(")");
    return i;
  };
  const auto args = [](std::initializer_list<std::string> a) { return join(std::vector<std::string>(a), ", "); };
  doc.model()->each([&](const Id& eid, const Value& e) {
    const std::string id = std::to_string(eid);
    const std::string type = str(e.get("type"));
    if (type == "line") {
      const std::size_t i =
          seg("swSkMgr.CreateLine(" +
                  args({M(num(e.get("x1"))), M(num(e.get("y1"))), "0", M(num(e.get("x2"))), M(num(e.get("y2"))), "0"}) +
                  ")",
              "line " + id);
      seg_of[id + "|L"] = i;
      pt_of[id + "|p1"] = segs(i) + ".GetStartPoint2";
      pt_of[id + "|p2"] = segs(i) + ".GetEndPoint2";
      ++n.lines;
    } else if (type == "circle") {
      const std::size_t i = seg("swSkMgr.CreateCircleByRadius(" +
                                    args({M(num(e.get("cx"))), M(num(e.get("cy"))), "0", M(num(e.get("r")))}) + ")",
                                "circle " + id);
      seg_of[id + "|R"] = i;
      pt_of[id + "|c"] = segs(i) + ".GetCenterPoint2";
      ++n.circles;
    } else if (type == "arc") {
      const double cx = num(e.get("cx"));
      const double cy = num(e.get("cy"));
      const double r = num(e.get("r"));
      const double a0 = num(e.get("a0"));
      const double a1 = num(e.get("a1"));
      const std::size_t i = seg("swSkMgr.CreateArc(" +
                                    args({M(cx), M(cy), "0", M(cx + r * std::cos(a0)), M(cy + r * std::sin(a0)), "0",
                                          M(cx + r * std::cos(a1)), M(cy + r * std::sin(a1)), "0", "1"}) +
                                    ")",
                                "arc " + id);
      seg_of[id + "|R"] = i;
      pt_of[id + "|c"] = segs(i) + ".GetCenterPoint2";
      pt_of[id + "|s"] = segs(i) + ".GetStartPoint2";
      pt_of[id + "|e"] = segs(i) + ".GetEndPoint2";
      ++n.arcs;
    } else if (type == "point") {
      const std::size_t i = np++;
      const std::string p = "pts(" + std::to_string(i) + ")";
      geo.push_back("Set " + p + " = swSkMgr.CreatePoint(" + args({M(num(e.get("x"))), M(num(e.get("y"))), "0"}) + ")");
      geo.push_back("If " + p + R"( Is Nothing Then LogFail "point )" + id + R"(")");
      pt_of[id + "|p"] = p;
      ++n.points;
    } else if (type == "poly") {
      const base::Array pts = e.get("pts").is_array() ? e.get("pts").as_array() : base::Array{};
      const std::size_t cnt = pts.size();
      std::vector<std::size_t> first;
      const double sc = gs::seg_count(e);
      for (std::size_t j = 0; static_cast<double>(j) < sc && cnt > 0; ++j) {
        const Value& p = pts[j];
        const Value& q = pts[(j + 1) % cnt];
        double b = num(p.get("b"));
        if (std::isnan(b)) {
          b = 0;
        }
        const XY pp{num(p.get("x")), num(p.get("y"))};
        const XY qq{num(q.get("x")), num(q.get("y"))};
        std::size_t i = 0;
        if (std::abs(b) < 1e-12) {
          i = seg("swSkMgr.CreateLine(" + args({M(pp.x), M(pp.y), "0", M(qq.x), M(qq.y), "0"}) + ")",
                  "polyline " + id + " segment " + std::to_string(j + 1));
          ++n.lines;
        } else {
          const XY a = bulge_centre(pp, qq, b);
          // CreateArc runs from its start to its end, counter-clockwise for +1, clockwise for −1: a bulge's own sense
          i = seg("swSkMgr.CreateArc(" +
                      args({M(a.x), M(a.y), "0", M(pp.x), M(pp.y), "0", M(qq.x), M(qq.y), "0", b > 0 ? "1" : "-1"}) +
                      ")",
                  "polyline " + id + " arc " + std::to_string(j + 1));
          ++n.arcs;
        }
        seg_of[id + "|s" + std::to_string(j)] = i;
        first.push_back(i);
      }
      for (std::size_t j = 0; j < cnt; ++j) {
        std::string at;
        if (j < first.size()) {
          at = segs(first[j]) + ".GetStartPoint2";
        } else if (j >= 1 && j - 1 < first.size()) {
          at = segs(first[j - 1]) + ".GetEndPoint2";
        } else {
          at = "segs(undefined).GetEndPoint2";
        }
        pt_of[id + "|v" + std::to_string(j)] = at;
      }
      for (std::size_t j = 1; j < first.size(); ++j) {
        joins.emplace_back(segs(first[j - 1]) + ".GetEndPoint2", segs(first[j]) + ".GetStartPoint2");
      }
      if (e.get("closed").truthy() && first.size() > 1) {
        joins.emplace_back(segs(first.back()) + ".GetEndPoint2", segs(first.front()) + ".GetStartPoint2");
      }
    } else {
      ++skipped;
    }
  });
  if (skipped > 0) {
    out.notes.push_back(words.tn("swsketch.skipped", skipped));
  }
  if (ns == 0 && np == 0) {
    out.error = words.t("swsketch.there_nothing_model_put");
    return out;
  }

  // ---- relations
  std::vector<std::string> rel;
  const auto ref_expr = [&](const std::string& key) -> std::optional<std::string> {
    if (const auto it = pt_of.find(key); it != pt_of.end()) {
      return it->second;
    }
    if (const auto it = seg_of.find(key); it != seg_of.end()) {
      return segs(it->second);
    }
    return std::nullopt;
  };
  const auto key_of = [](const Value& r) { return str(r.get("e")) + "|" + str(r.get("k")); };
  std::map<std::string, std::string> mid_pts;
  const auto mid_of = [&](const Value& r) -> std::optional<std::string> {
    // a line's middle: a construction point held at the middle (SolidWorks has no middle point of its own)
    const std::string key = str(r.get("e")) + "|mid";
    if (const auto it = mid_pts.find(key); it != mid_pts.end()) {
      return it->second;
    }
    const Value* e = entity(doc, r.get("e"));
    if (e == nullptr || str(e->get("type")) != "line") {
      return std::nullopt;
    }
    const std::size_t i = np++;
    std::string p = "pts(" + std::to_string(i) + ")";
    rel.push_back(
        "Set " + p + " = swSkMgr.CreatePoint(" +
        args({M((num(e->get("x1")) + num(e->get("x2"))) / 2), M((num(e->get("y1")) + num(e->get("y2"))) / 2), "0"}) +
        ")");
    const auto line = seg_of.find(str(r.get("e")) + "|L");
    rel.push_back("If Not " + p + " Is Nothing Then " + p + ".Select4 False, Nothing: " +
                  (line != seg_of.end() ? segs(line->second) : std::string("segs(undefined)")) +
                  R"(.Select4 True, Nothing: Relate "sgATMIDDLE", "middle of line )" + str(r.get("e")) + R"(")");
    mid_pts[key] = p;
    return p;
  };
  const auto sel = [&](const std::vector<std::string>& exprs, std::string_view rname, const std::string& what) {
    rel.emplace_back("swModel.ClearSelection2 True");
    for (std::size_t j = 0; j < exprs.size(); ++j) {
      rel.push_back("Pick " + exprs[j] + ", " + (j > 0 ? "True" : "False"));
    }
    rel.push_back(R"(Relate ")" + std::string(rname) + R"(", ")" + what + R"(")");
  };
  for (const auto& [a, b] : joins) {
    sel({a, b}, "sgCOINCIDENT", "polyline vertex");
    ++n.relations;
  }
  int lost_rel = 0;
  const base::Array cs = doc.constraints.is_array() ? doc.constraints.as_array() : base::Array{};
  for (const Value& c : cs) {
    if (gc::is_dim(c)) {
      continue;
    }
    const base::Array refs = c.get("refs").is_array() ? c.get("refs").as_array() : base::Array{};
    std::vector<std::optional<std::string>> exprs;
    for (const Value& r : refs) {
      const std::string k = str(r.get("k"));
      if (k == "mid") {
        exprs.push_back(mid_of(r));
      } else if (k == "*") {
        exprs.push_back(ref_expr(str(r.get("e")) + "|" + whole_key(entity(doc, r.get("e")))));
      } else {
        exprs.push_back(ref_expr(key_of(r)));
      }
    }
    if (std::ranges::any_of(exprs, [](const auto& x) { return !x; })) {
      ++lost_rel;
      continue;
    }
    const std::string type = str(c.get("type"));
    std::optional<std::string> rname;
    if (const auto r = lookup(kRel, type)) {
      rname = std::string(*r);
    }
    std::string sig;
    for (const Value& r : refs) {
      const Value* e = entity(doc, r.get("e"));
      if (const auto kk = gs::ref_kind(e, r.get("k"))) {
        sig += *kk;
      }
    }
    if (type == "horizontal") {
      rname = sig == "PP" ? "sgHORIZONTALPOINTS2D" : "sgHORIZONTAL2D";
    }
    if (type == "vertical") {
      rname = sig == "PP" ? "sgVERTICALPOINTS2D" : "sgVERTICAL2D";
    }
    if (!rname) {
      ++lost_rel;
      continue;
    }
    std::vector<std::string> ex_list;
    ex_list.reserve(exprs.size());
    for (const auto& x : exprs) {
      ex_list.push_back(*x);
    }
    sel(ex_list, *rname, type + " " + str(c.get("id")));
    ++n.relations;
  }
  if (lost_rel > 0) {
    out.notes.push_back(words.tn("swsketch.lost_rel", lost_rel));
  }
  if (std::ranges::any_of(cs, [](const Value& c) { return str(c.get("type")) == "symmetric"; })) {
    out.notes.push_back(words.t("swsketch.symmetric"));
  }

  // ---- dimensions (named; their values set; expressions become equations below)
  std::vector<std::string> dims;
  std::set<std::string> sketch_dims;
  for (const Value& c : cs) {
    if (!gc::is_dim(c)) {
      continue;
    }
    const base::Array refs = c.get("refs").is_array() ? c.get("refs").as_array() : base::Array{};
    std::vector<std::string> pick;
    bool lost = false;
    for (const Value& r : refs) {
      const auto x = ref_expr(key_of(r));
      if (!x) {
        lost = true;
        break;
      }
      pick.push_back(*x);
    }
    if (lost) {
      continue;
    }
    XY at;
    if (const auto d = gc::dim_entity_for(doc, c, nullptr, words)) {
      const Value loc = d->get("loc");
      at = {num(loc.get("x")), num(loc.get("y"))};
    }
    const std::string type = str(c.get("type"));
    std::string call = "swModel.AddDimension2(";
    if (type == "hdist") {
      call = "swModel.AddHorizontalDimension2(";
    } else if (type == "vdist") {
      call = "swModel.AddVerticalDimension2(";
    } else if (type == "radius") {
      call = "swModel.AddRadialDimension2(";
    } else if (type == "diameter") {
      call = "swModel.AddDiameterDimension2(";
    } else if (type == "angle" && refs.size() == 1) {
      const std::string e = str(refs.front().get("e"));
      pick.clear();
      for (const char* k : {"|c", "|s", "|e"}) {
        const auto it = pt_of.find(e + k);
        pick.push_back(it != pt_of.end() ? it->second : std::string("undefined"));
      }
    }
    const double cv = num(c.get("value"));
    const std::string value = type == "angle" ? num15(cv * std::numbers::pi / 180) : M(cv);
    const std::string cname = str(c.get("name"));
    dims.emplace_back("swModel.ClearSelection2 True");
    for (std::size_t j = 0; j < pick.size(); ++j) {
      dims.push_back("Pick " + pick[j] + ", " + (j > 0 ? "True" : "False"));
    }
    std::string stmt = "Dimension " + call;
    stmt += M(at.x);
    stmt += ", ";
    stmt += M(at.y);
    stmt += R"(, 0), ")";
    stmt += cname;
    stmt += R"(", )";
    stmt += value;
    stmt += R"(, ")";
    stmt += cname;
    stmt += " (";
    stmt += type;
    stmt += ')';
    stmt += '"';
    dims.push_back(std::move(stmt));
    sketch_dims.insert(cname);
    ++n.dimensions;
  }

  // ---- global variables and equations (EquationMgr.Add2, index -1 = at the end)
  std::vector<std::string> eqs;
  const pa::Params params = pa::from_value(doc.params);
  const pa::Evaluated ev = pa::evaluate_vars(params, nullptr, words);
  const auto quote = [&sketch_dims](const std::string& v) {
    return sketch_dims.contains(v) ? R"("")" + v + R"(@" & sketchName & """)" : R"("")" + v + R"("")";
  };
  for (const std::string& vn : ev.order) {
    const pa::Var* v = pa::var_by_name(params, vn);
    if (v == nullptr) {
      continue;
    }
    const bool dim = sketch_dims.contains(vn);
    const ex::Parsed lp = ex::parse(v->expr, std::optional<std::set<std::string>>(std::set<std::string>{}), words);
    const bool lit = lp.ok && lp.ast && lp.ast->kind == ex::Node::Kind::num;
    if (dim && lit) {
      continue;  // a plain number: the dimension's own value
    }
    const SwExpr rhs = sw_expr(v->expr, quote, words);
    const std::string value = ex::format_number(ev.value(vn).value_or(0));
    if (!rhs.ok) {
      out.notes.push_back(words.t(dim ? "swsketch.eq_value.dim" : "swsketch.eq_value.param",
                                  {{"name", vn}, {"why", rhs.error}, {"value", value}}));
    }
    eqs.push_back(R"(AddEquation ")" + quote(vn) + " = " + (rhs.ok ? rhs.text : value) + R"(", ")" + vn + R"(")");
    if (dim) {
      ++n.equations;
    } else {
      ++n.globals;
    }
  }
  if (!doc.layouts().empty()) {
    out.notes.push_back(words.t("swsketch.sheets_not_part_sketch"));
  }

  // ---- the macro
  const std::string nm = safe(name);
  const auto ns_ = [](int v) { return std::to_string(v); };
  std::vector<std::string> lines{
      "' " + nm + ".swb - written by the CadCraft drafting app (getcadcraft.com) on " + std::string(date) + ".",
      R"(' Run it in SolidWorks: Tools, Macro, Run, then choose this file. It makes a new part with one sketch on the Front Plane:)",
      "' " + ns_(n.lines) + " lines, " + ns_(n.arcs) + " arcs, " + ns_(n.circles) + " circles, " + ns_(n.points) +
          " points, " + ns_(n.relations) + " relations, " + ns_(n.dimensions) + " dimensions, " + ns_(n.globals) +
          " global variables, " + ns_(n.equations) + " equations.",
      "' Not yet tested in SolidWorks. Failures are written to " + nm +
          "-cadcraft.log next to this macro; the macro carries on.",
      "' Units: the part uses the drawing's length unit; the API takes metres, so every coordinate below is in metres.",
      "Option Explicit",
      "Dim swApp As Object",
      "Dim swModel As Object",
      "Dim swSkMgr As Object",
      "Dim swEqnMgr As Object",
      "Dim segs(" + std::to_string(ns > 0 ? ns - 1 : 0) + ") As Object",
      "Dim pts(" + std::to_string(np > 0 ? np - 1 : 0) + ") As Object",
      "Dim sketchName As String",
      "Dim logPath As String",
      "Dim fails As Long",
      "Dim done As Long",
      "",
      "Sub LogFail(what As String)",
      "    Dim f As Integer",
      "    fails = fails + 1",
      "    On Error Resume Next",
      "    f = FreeFile",
      "    Open logPath For Append As #f",
      R"(    Print #f, Now & "  could not make: " & what)",
      "    Close #f",
      "    Err.Clear",
      "End Sub",
      "",
      "Sub Pick(obj As Object, append As Boolean)",
      "    On Error Resume Next",
      "    If obj Is Nothing Then Exit Sub",
      "    obj.Select4 append, Nothing",
      "    Err.Clear",
      "End Sub",
      "",
      "Sub Relate(kind As String, what As String)",
      "    On Error Resume Next",
      "    swModel.SketchAddConstraints kind",
      "    If Err.Number <> 0 Then LogFail what & \" (\" & kind & \")\" Else done = done + 1",
      "    Err.Clear",
      "    swModel.ClearSelection2 True",
      "End Sub",
      "",
      "Sub Dimension(dd As Object, dimName As String, v As Double, what As String)",
      "    Dim d As Object",
      "    On Error Resume Next",
      "    If dd Is Nothing Then LogFail what: Exit Sub",
      "    Set d = dd.GetDimension2(0)",
      "    If d Is Nothing Then LogFail what: Exit Sub",
      "    d.Name = dimName",
      "    d.SystemValue = v",
      "    If Err.Number <> 0 Then LogFail what & \" (name or value)\" Else done = done + 1",
      "    Err.Clear",
      "    swModel.ClearSelection2 True",
      "End Sub",
      "",
      "Sub AddEquation(text As String, what As String)",
      "    Dim r As Long",
      "    On Error Resume Next",
      "    r = swEqnMgr.Add2(-1, text, True)",
      R"(    If Err.Number <> 0 Or r < 0 Then LogFail "equation for " & what & ": " & text Else done = done + 1)",
      "    Err.Clear",
      "End Sub",
      "",
  };
  struct Sub {
    std::string nm;
    std::vector<std::string> body;
  };
  std::vector<Sub> subs;
  const auto chunked = [&subs](const std::string& prefix, const std::vector<std::string>& stmts) {
    for (std::size_t i = 0; i < stmts.size(); i += 150) {
      Sub s{.nm = prefix + std::to_string(i / 150 + 1), .body = {"    On Error Resume Next"}};
      for (std::size_t j = i; j < std::min(stmts.size(), i + 150); ++j) {
        s.body.push_back("    " + stmts[j]);
      }
      s.body.emplace_back("    Err.Clear");
      subs.push_back(std::move(s));
    }
  };
  chunked("Geometry", geo);
  chunked("Relations", rel);
  chunked("Dimensions", dims);
  chunked("Equations", eqs);
  for (const Sub& s : subs) {
    lines.push_back("Sub " + s.nm + "()");
    lines.insert(lines.end(), s.body.begin(), s.body.end());
    lines.emplace_back("End Sub");
    lines.emplace_back("");
  }
  const std::vector<std::string> main_head{
      "Sub main()",
      "    Dim tpl As String",
      "    Dim ok As Boolean",
      "    Dim feat As Object",
      "    Set swApp = Application.SldWorks",
      R"(    logPath = swApp.GetCurrentMacroPathFolder() & "\)" + nm + R"(-cadcraft.log")",
      "    tpl = swApp.GetUserPreferenceStringValue(" + std::to_string(kSwDefaultTemplatePart) + ")",
      "    Set swModel = swApp.NewDocument(tpl, 0, 0, 0)",
      "    If swModel Is Nothing Then",
      R"(        MsgBox "Could not make a new part from the default part template (" & tpl & "). Set one in Tools, Options, Default Templates.")",
      "        Exit Sub",
      "    End If",
      "    ok = swModel.Extension.SetUserPreferenceInteger(" + std::to_string(kSwUnitsLinear) + ", " +
          std::to_string(kSwDetailingNoOptionSpecified) + ", " + std::to_string(sw_unit) + ")",
      "    swApp.SetUserPreferenceToggle " + std::to_string(kSwInputDimValOnCreate) + ", False",
      "    Set swSkMgr = swModel.SketchManager",
      R"(    ok = swModel.Extension.SelectByID2("Front Plane", "PLANE", 0, 0, 0, False, 0, Nothing, 0))",
      R"(    If Not ok Then LogFail "selecting the Front Plane")",
      "    swSkMgr.InsertSketch True",
      "    swSkMgr.AddToDB = True",
      "    swSkMgr.DisplayWhenAdded = False",
      "    Set feat = swSkMgr.ActiveSketch",
      R"(    sketchName = "Sketch1")",
      "    If Not feat Is Nothing Then sketchName = feat.Name",
  };
  lines.insert(lines.end(), main_head.begin(), main_head.end());
  for (const Sub& s : subs) {
    if (s.nm.starts_with("Geometry") || s.nm.starts_with("Relations") || s.nm.starts_with("Dimensions")) {
      lines.push_back("    " + s.nm);
    }
  }
  lines.insert(lines.end(), {"    swSkMgr.AddToDB = False", "    swSkMgr.DisplayWhenAdded = True",
                             "    swSkMgr.InsertSketch True", "    Set swEqnMgr = swModel.GetEquationMgr()"});
  for (const Sub& s : subs) {
    if (s.nm.starts_with("Equations")) {
      lines.push_back("    " + s.nm);
    }
  }
  lines.insert(
      lines.end(),
      {"    ok = swModel.EditRebuild3()", "    swModel.ViewZoomtofit2",
       R"(    MsgBox "CadCraft sketch: " & done & " relations, dimensions and equations made, " & fails & " could not be made" & IIf(fails > 0, " (see " & logPath & ").", "."))",
       "End Sub", ""});
  for (const std::string& l : lines) {
    out.text += l;
    out.text += "\r\n";
  }
  for (const std::uint16_t cu : base::to_utf16(out.text)) {
    out.bytes.push_back(static_cast<std::byte>(cu < 256 ? cu : 63));
  }
  out.summary = words.t("swsketch.summary", {{"lines", ns_(n.lines)},
                                             {"arcs", ns_(n.arcs)},
                                             {"circles", ns_(n.circles)},
                                             {"points", ns_(n.points)},
                                             {"relations", ns_(n.relations)},
                                             {"dimensions", ns_(n.dimensions)},
                                             {"globals", ns_(n.globals)},
                                             {"equations", ns_(n.equations)}});
  out.ok = true;
  return out;
}
// i18n-ignore-end

// ---------------------------------------------------------------- backward

namespace {

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — readSwExport's checks, in its order
Checked check_export(const Value& j, const i18n::Catalog& words) {
  const auto fail = [](std::string e) { return Checked{.ok = false, .error = std::move(e), .sketches = {}}; };
  if (!j.is_object() || str(j.get("format")) != "cadcraft-sw-export" || !j.get("format").is_string()) {
    return fail(words.t("swsketch.json_not_export_cadcraft"));
  }
  const Value ver = j.get("version");
  if (!ver.is_number() || ver.as_number() != 1) {
    return fail(words.t("swsketch.version", {{"version", slice16(str(ver), 10)}}));
  }
  const Value units = j.get("units");
  if (!units.is_undefined() && !(units.is_string() && units.as_string() == "mm")) {
    return fail(words.t("swsketch.export_not_millimetres"));
  }
  const Value features = j.get("features");
  if (!features.is_array() || features.as_array().size() > kLim.features) {
    return fail(words.t("swsketch.export_has_no_feature"));
  }
  Checked out;
  const base::Array& fs = features.as_array();
  for (std::size_t fi = 0; fi < fs.size(); ++fi) {
    const Value& f = fs[fi];
    if (!f.truthy() || !(f.get("type").is_string() && f.get("type").as_string() == "sketch")) {
      continue;
    }
    const Value fname = f.get("name");
    if (!fname.is_string() || base::js_blank(fname.as_string())) {
      return fail(words.t("swsketch.sketch_export_has_no"));
    }
    const auto bad = [&](const std::string& what) {
      return fail(words.t("swsketch.bad", {{"name", slice16(fname.as_string(), 60)}, {"what", what}}));
    };
    const Value pts = list_or_empty(f.get("points"));
    const Value segs = list_or_empty(f.get("segments"));
    const Value rels = list_or_empty(f.get("relations"));
    const Value dims = list_or_empty(f.get("dimensions"));
    if (!pts.is_array() || !segs.is_array() || !rels.is_array() || !dims.is_array() ||
        pts.as_array().size() > kLim.points || segs.as_array().size() > kLim.segments ||
        rels.as_array().size() > kLim.relations || dims.as_array().size() > kLim.dimensions) {
      return bad(words.t("swsketch.lists_bad_too_long"));
    }
    std::set<std::string> pids;
    std::set<std::string> sids;
    const auto has = [](const std::set<std::string>& s, const Value& v) {
      return v.is_string() && s.contains(v.as_string());
    };
    for (const Value& p : pts.as_array()) {
      const Value id = p.get("id");
      if (!p.truthy() || !id_ok(id) || has(pids, id) || !finite(p.get("x")) || !finite(p.get("y"))) {
        return bad(words.t("swsketch.point_bad"));
      }
      pids.insert(id.as_string());
    }
    for (const Value& s : segs.as_array()) {
      const Value id = s.get("id");
      if (!s.truthy() || !id_ok(id) || has(sids, id) || has(pids, id)) {
        return bad(words.t("swsketch.segment_bad"));
      }
      const std::string type = s.get("type").is_string() ? s.get("type").as_string() : std::string();
      const std::string& sid = id.as_string();
      if (type == "line") {
        if (!has(pids, s.get("a")) || !has(pids, s.get("b"))) {
          return bad(words.t("swsketch.bad.missing", {{"what", "line " + sid}}));  // i18n-ignore: an id from the file
        }
      } else if (type == "arc") {
        if (!has(pids, s.get("c")) || !has(pids, s.get("s")) || !has(pids, s.get("e"))) {
          return bad(words.t("swsketch.bad.missing", {{"what", "arc " + sid}}));  // i18n-ignore: an id from the file
        }
      } else if (type == "circle") {
        const Value r = s.get("r");
        if (!has(pids, s.get("c")) || !finite(r) || !(r.as_number() > 0)) {
          return bad(words.t("swsketch.bad.bad", {{"what", "circle " + sid}}));  // i18n-ignore: an id from the file
        }
      } else {
        return bad(words.t("swsketch.bad.type", {{"what", "segment " + sid}}));  // i18n-ignore: an id from the file
      }
      sids.insert(sid);
    }
    const auto refs_ok = [&](const Value& refs) {
      return refs.is_array() && refs.as_array().size() <= 3 &&
             std::ranges::all_of(refs.as_array(), [&](const Value& x) {
               return (x.is_string() && x.as_string() == "O") || has(pids, x) || has(sids, x);
             });
    };
    for (const Value& r : rels.as_array()) {
      if (!r.truthy() || !r.get("type").is_string() || !refs_ok(r.get("refs"))) {
        return bad(words.t("swsketch.relation_bad"));
      }
    }
    for (const Value& d : dims.as_array()) {
      const Value nm = d.get("name");
      if (!d.truthy() || !d.get("type").is_string() || !finite(d.get("value")) || !refs_ok(d.get("refs")) ||
          !(nm.is_undefined() || nm.is_string())) {
        return bad(words.t("swsketch.dimension_bad"));
      }
    }
    out.sketches.push_back(fi);
  }
  if (out.sketches.empty()) {
    return fail(words.t("swsketch.export_has_no_sketches"));
  }
  for (const auto& [k, lim] : {std::pair<std::string_view, std::size_t>{"globals", kLim.globals},
                               std::pair<std::string_view, std::size_t>{"equations", kLim.equations}}) {
    const Value v = j.get(k);
    if (!v.is_undefined() && (!v.is_array() || v.as_array().size() > lim)) {
      return fail(words.t(k == "globals" ? "swsketch.bad.globals" : "swsketch.bad.equations"));
    }
  }
  out.ok = true;
  return out;
}

std::string sketch_name(const Value& f) {
  return slice16(str(f.get("name")), 120);
}

}  // namespace

Export read_export(const Value& j, const i18n::Catalog& words) {
  const Checked c = check_export(j, words);
  Export out{.ok = c.ok, .error = c.error, .sketches = {}};
  if (c.ok) {
    const base::Array fs = j.get("features").as_array();
    for (const std::size_t i : c.sketches) {
      out.sketches.push_back(sketch_name(fs[i]));
    }
  }
  return out;
}

Export read_export_text(std::string_view raw, const i18n::Catalog& words) {
  if (base::utf16_length(raw) > 50'000'000) {
    return {.ok = false, .error = words.t("swsketch.export_too_large"), .sketches = {}};
  }
  if (raw.starts_with("\xEF\xBB\xBF")) {
    raw.remove_prefix(3);
  }
  const auto j = base::parse_json(raw);
  if (!j) {
    return {.ok = false, .error = words.t("swsketch.file_not_readable_json"), .sketches = {}};
  }
  return read_export(*j, words);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — loadSwSketch, step for step
Loaded load_sketch(Doc& doc, const Value& exp, std::string_view sketch_name_in, const i18n::Catalog& words) {
  const Checked chk = check_export(exp, words);
  Loaded out;
  if (!chk.ok) {
    out.error = chk.error;
    return out;
  }
  const base::Array fs = exp.get("features").as_array();
  std::size_t pick = chk.sketches.front();
  for (const std::size_t i : chk.sketches) {
    if (sketch_name(fs[i]) == sketch_name_in) {
      pick = i;
      break;
    }
  }
  const Value& f = fs[pick];
  const std::string sk_name = sketch_name(f);
  std::vector<std::string>& notes = out.notes;
  if (chk.sketches.size() > 1) {
    notes.push_back(words.t("swsketch.load.several", {{"n", std::to_string(chk.sketches.size())}, {"name", sk_name}}));
  }
  if (f.get("plane").truthy()) {
    notes.push_back(words.t("swsketch.load.plane"));
  }
  if (f.get("suppressed").truthy()) {
    notes.push_back(words.t("swsketch.load.suppressed", {{"name", sk_name}}));
  }
  const base::Array pts = list_or_empty(f.get("points")).as_array();
  const base::Array segs = list_or_empty(f.get("segments")).as_array();
  const base::Array rels = list_or_empty(f.get("relations")).as_array();
  const base::Array dims = list_or_empty(f.get("dimensions")).as_array();
  std::map<std::string, const Value*> P;
  for (const Value& p : pts) {
    P.emplace(p.get("id").as_string(), &p);  // the first of each id (ids are unique: checked)
  }
  const auto pxy = [&P](const Value& id) {
    const Value* p = P.at(id.as_string());
    return XY{num(p->get("x")), num(p->get("y"))};
  };
  struct Ref {
    std::size_t i = 0;
    std::string k;
  };
  // entities: segments, then points no segment uses
  base::Array ents;
  std::map<std::string, Ref> ref_of_point;
  std::map<std::string, Ref> ref_of_seg;
  std::vector<std::pair<Ref, Ref>> extra;  // shared sketch points → coincident pairs
  const auto use_point = [&](const Value& pid, Ref ref) {
    const std::string& id = pid.as_string();
    if (const auto it = ref_of_point.find(id); it != ref_of_point.end()) {
      extra.emplace_back(it->second, std::move(ref));
    } else {
      ref_of_point.emplace(id, std::move(ref));
    }
  };
  const auto layer_of = [](const Value& x) { return x.get("construction").truthy() ? "CONSTRUCTION" : "0"; };
  for (const Value& s : segs) {
    const std::size_t i = ents.size();
    const std::string type = s.get("type").as_string();
    const std::string sid = s.get("id").as_string();
    if (type == "line") {
      const XY a = pxy(s.get("a"));
      const XY b = pxy(s.get("b"));
      ents.emplace_back(base::make_object(
          {{"type", "line"}, {"layer", layer_of(s)}, {"x1", a.x}, {"y1", a.y}, {"x2", b.x}, {"y2", b.y}}));
      ref_of_seg[sid] = {i, "L"};
      use_point(s.get("a"), {i, "p1"});
      use_point(s.get("b"), {i, "p2"});
    } else if (type == "arc") {
      const XY c = pxy(s.get("c"));
      const XY st = pxy(s.get("s"));
      const XY en = pxy(s.get("e"));
      ents.emplace_back(base::make_object({{"type", "arc"},
                                           {"layer", layer_of(s)},
                                           {"cx", c.x},
                                           {"cy", c.y},
                                           {"r", std::hypot(st.x - c.x, st.y - c.y)},
                                           {"a0", ang(st.y - c.y, st.x - c.x)},
                                           {"a1", ang(en.y - c.y, en.x - c.x)}}));
      ref_of_seg[sid] = {i, "R"};
      use_point(s.get("c"), {i, "c"});
      use_point(s.get("s"), {i, "s"});
      use_point(s.get("e"), {i, "e"});
    } else {
      const XY c = pxy(s.get("c"));
      ents.emplace_back(base::make_object(
          {{"type", "circle"}, {"layer", layer_of(s)}, {"cx", c.x}, {"cy", c.y}, {"r", num(s.get("r"))}}));
      ref_of_seg[sid] = {i, "R"};
      use_point(s.get("c"), {i, "c"});
    }
  }
  for (const Value& p : pts) {
    const std::string id = p.get("id").as_string();
    if (!ref_of_point.contains(id)) {
      ref_of_point[id] = {ents.size(), "p"};
      ents.emplace_back(base::make_object(
          {{"type", "point"}, {"layer", layer_of(p)}, {"x", num(p.get("x"))}, {"y", num(p.get("y"))}}));
    }
  }
  // "O" = the sketch origin (SolidWorks' is not a sketch point): a construction point at 0,0, fixed
  const auto has_origin = [](const Value& x) {
    const Value refs = x.get("refs");
    return std::ranges::any_of(refs.as_array(), [](const Value& r) { return r.is_string() && r.as_string() == "O"; });
  };
  const bool uses_origin = std::ranges::any_of(rels, has_origin) || std::ranges::any_of(dims, has_origin);
  if (uses_origin) {
    ref_of_point["O"] = {ents.size(), "p"};
    ents.emplace_back(base::make_object({{"type", "point"}, {"layer", "CONSTRUCTION"}, {"x", 0}, {"y", 0}}));
  }
  base::Array layers;
  if (std::ranges::any_of(ents, [](const Value& e) { return str(e.get("layer")) == "CONSTRUCTION"; })) {
    layers.emplace_back(base::make_object({{"name", "CONSTRUCTION"}, {"color", 8}}));
  }
  const std::size_t n_ents = ents.size();
  const Value en = exp.get("name");
  doc.load(base::make_object(
               {{"entities", std::move(ents)},
                {"layers", std::move(layers)},
                {"meta", base::make_object({{"name", sk_name},
                                            {"units", "millimeters"},
                                            {"source", (en.truthy() ? str(en) : std::string("export")) + ".json"},
                                            {"format", words.t("swsketch.load.format", {{"name", sk_name}})}})}}),
           words);
  std::vector<double> ids;
  doc.model()->each([&ids](const Id& id, const Value& /*e*/) { ids.push_back(static_cast<double>(id)); });
  using OptRef = std::optional<Value>;
  const auto R = [&ids](const std::optional<Ref>& x) -> OptRef {
    if (!x || x->i >= ids.size()) {
      return std::nullopt;
    }
    return base::make_object({{"e", ids[x->i]}, {"k", x->k}});
  };
  const auto ref_for = [&](const Value& id) -> OptRef {
    const std::string s = str(id);
    if (const auto it = ref_of_point.find(s); it != ref_of_point.end()) {
      return R(it->second);
    }
    if (const auto it = ref_of_seg.find(s); it != ref_of_seg.end()) {
      return R(it->second);
    }
    return std::nullopt;
  };

  // relations
  struct Plan {
    std::string type;
    std::vector<OptRef> refs;
    std::optional<double> value;
    std::optional<std::string> name;
  };
  std::vector<Plan> plan;
  plan.reserve(extra.size() + rels.size() + dims.size() + 1);
  for (const auto& [a, b] : extra) {
    plan.push_back({.type = "coincident", .refs = {R(b), R(a)}, .value = {}, .name = {}});
  }
  if (uses_origin) {
    plan.push_back({.type = "fix", .refs = {ref_for(Value("O"))}, .value = {}, .name = {}});
  }
  int lost = 0;
  std::vector<std::string> lost_kinds;
  const auto lost_kind = [&lost_kinds](const std::string& k) {
    if (std::ranges::find(lost_kinds, k) == lost_kinds.end()) {
      lost_kinds.push_back(k);
    }
  };
  const auto k_of = [](const OptRef& r) { return r ? str(r->get("k")) : std::string(); };
  static constexpr std::array<std::string_view, 11> kKinds{"horizontal",    "vertical",  "coincident", "parallel",
                                                           "perpendicular", "tangent",   "equal",      "concentric",
                                                           "fix",           "collinear", "symmetric"};
  for (const Value& rl : rels) {
    std::vector<OptRef> refs;
    for (const Value& x : rl.get("refs").as_array()) {
      refs.push_back(ref_for(x));
    }
    const std::string t = rl.get("type").as_string();
    if (t == "midpoint") {
      if (refs.size() >= 2 && refs[0] && refs[1] && k_of(refs[1]) == "L") {
        plan.push_back({.type = "coincident",
                        .refs = {refs[0], base::make_object({{"e", refs[1]->get("e")}, {"k", "mid"}})},
                        .value = {},
                        .name = {}});
        continue;
      }
      ++lost;
      lost_kind(t);
      continue;
    }
    if (std::ranges::find(kKinds, t) == kKinds.end()) {
      ++lost;
      lost_kind(slice16(t, 30));
      continue;
    }
    if (t == "fix" && refs.size() == 1 && refs[0]) {
      const std::string k = k_of(refs[0]);
      if (k != "p" && k != "p1" && k != "p2" && k != "c" && k != "s" && k != "e") {
        refs[0] = base::make_object({{"e", refs[0]->get("e")}, {"k", "*"}});
      }
    }
    if (t == "tangent" && refs.size() == 2 && k_of(refs[0]) == "R" && k_of(refs[1]) == "L") {
      std::ranges::reverse(refs);
    }
    if (t == "coincident" && refs.size() == 2 && (k_of(refs[0]) == "L" || k_of(refs[0]) == "R")) {
      std::ranges::reverse(refs);
    }
    plan.push_back({.type = t, .refs = std::move(refs), .value = {}, .name = {}});
  }
  // dimensions
  std::map<std::string, std::string> dim_names;  // SolidWorks name (D1) → ours
  std::set<std::string> taken;
  static constexpr std::array<Pair, 6> kDimTypes{{{"distance", "dist"},
                                                  {"hdistance", "hdist"},
                                                  {"vdistance", "vdist"},
                                                  {"radius", "radius"},
                                                  {"diameter", "diameter"},
                                                  {"angle", "angle"}}};
  for (const Value& d : dims) {
    std::vector<OptRef> refs;
    for (const Value& x : d.get("refs").as_array()) {
      refs.push_back(ref_for(x));
    }
    const auto type = lookup(kDimTypes, d.get("type").as_string());
    if (!type || std::ranges::any_of(refs, [](const OptRef& x) { return !x; })) {
      ++lost;
      lost_kind(words.t("swsketch.load.lost_dim", {{"type", slice16(d.get("type").as_string(), 20)}}));
      continue;
    }
    std::vector<OptRef> rr = refs;
    if ((*type == "dist" || *type == "hdist" || *type == "vdist") && refs.size() == 1 && k_of(refs[0]) == "L") {
      rr = {base::make_object({{"e", refs[0]->get("e")}, {"k", "p1"}}),
            base::make_object({{"e", refs[0]->get("e")}, {"k", "p2"}})};
    }
    if (*type == "dist" && rr.size() == 2 && k_of(rr[0]) == "L" && k_of(rr[1]) != "L") {
      std::swap(rr[0], rr[1]);
    }
    const Value dn = d.get("name");
    std::string name;
    if (dn.is_string() && ex::valid_name(dn.as_string()) && !ex::reserved(base::js_to_lower(dn.as_string())) &&
        !taken.contains(dn.as_string())) {
      name = dn.as_string();
    } else {
      const gc::TypeInfo* ti = gc::type_info(*type);
      name = gc::next_name(pa::Params{}, ti != nullptr ? ti->prefix : std::string_view("d"), taken);
    }
    taken.insert(name);
    if (dn.is_string()) {
      dim_names[dn.as_string()] = name;
    }
    if (const Value full = d.get("full"); full.is_string()) {
      dim_names[full.as_string()] = name;
    }
    plan.push_back({.type = std::string(*type), .refs = std::move(rr), .value = num(d.get("value")), .name = name});
  }
  if (lost > 0) {
    notes.push_back(words.tn("swsketch.load.lost", lost, {{"kinds", join(lost_kinds, ", ")}}));
  }

  // build the constraints against the geometry (not undoable: this is opening a file)
  // the list grows in the document while it is made (makeConstraint numbers against it), as the JS's doc.constraints =
  // cs
  pa::Params params;
  int refused = 0;
  Value before = std::exchange(doc.constraints, Value::array());
  doc.solving = true;
  for (const Plan& p : plan) {
    if (std::ranges::any_of(p.refs, [](const OptRef& x) { return !x; })) {
      ++refused;
      continue;
    }
    base::Array refs;
    for (const OptRef& x : p.refs) {
      refs.push_back(*x);
    }
    gc::MakeOpts o;
    o.name = p.name;
    o.model_space = true;
    auto [m, err] = gc::make_constraint(doc, p.type, Value(std::move(refs)), o, words);
    if (!m) {
      ++refused;
      continue;
    }
    Value c = std::move(*m);
    if (gc::is_dim(c)) {
      if (p.value && std::isfinite(*p.value) && *p.value > 0) {
        c.set("value", *p.value);  // millimetres; angles in degrees
      }
      const std::string cname = str(c.get("name"));
      params.vars.push_back({.name = cname,
                             .expr = ex::format_number(base::js_round(num(c.get("value")) * 1e9) / 1e9),
                             .note = std::nullopt});
      params.links.push_back({.target = gc::target_key(c), .expr = cname});
    }
    doc.constraints.as_array().push_back(std::move(c));
  }
  doc.solving = false;
  base::Array cs = std::exchange(doc.constraints, std::move(before)).as_array();
  if (refused > 0) {
    notes.push_back(words.tn("swsketch.load.refused", refused));
  }

  // global variables, then equations on this sketch's dimensions
  struct Tr {
    std::string s;
    std::optional<std::string> unknown;
  };
  const auto sw_to_ours = [&](const std::string& txt) {
    Tr tr;
    // /"([^"]{1,120})"/g → our names
    std::size_t i = 0;
    while (i < txt.size()) {
      const std::size_t q = txt.find('"', i);
      if (q == std::string::npos) {
        tr.s += txt.substr(i);
        break;
      }
      const std::size_t q2 = txt.find('"', q + 1);
      const std::size_t len16 =
          q2 == std::string::npos ? 0 : base::utf16_length(std::string_view(txt).substr(q + 1, q2 - q - 1));
      if (q2 == std::string::npos || len16 < 1 || len16 > 120) {
        // no match here: JS moves on one character
        tr.s += txt.substr(i, q - i + 1);
        i = q + 1;
        continue;
      }
      tr.s += txt.substr(i, q - i);
      const std::string n = txt.substr(q + 1, q2 - q - 1);
      std::string rep;
      if (const auto it = dim_names.find(n); it != dim_names.end()) {
        rep = it->second;
      } else {
        const std::size_t at = n.find('@');
        const std::string short_n = n.substr(0, at);
        if (at != std::string::npos) {
          if (n.ends_with("@" + sk_name) && dim_names.contains(short_n)) {
            rep = dim_names.at(short_n);
          } else {
            if (!tr.unknown) {
              tr.unknown = n;
            }
            rep = "0";
          }
        } else if (ex::valid_name(n)) {
          rep = n;
        } else {
          if (!tr.unknown) {
            tr.unknown = n;
          }
          rep = "0";
        }
      }
      tr.s += rep;
      i = q2 + 1;
    }
    // \bsqr\( → sqrt( · \bsgn\( → sign( · \batn\( → atan(  (any case)
    for (const auto& [from, to] :
         {std::pair<std::string_view, std::string_view>{"sqr(", "sqrt("}, {"sgn(", "sign("}, {"atn(", "atan("}}) {
      std::string r;
      std::size_t k = 0;
      while (k < tr.s.size()) {
        const bool boundary = k == 0 || !word_char(tr.s[k - 1]);
        if (boundary && k + from.size() <= tr.s.size() && base::js_to_lower(tr.s.substr(k, from.size())) == from &&
            word_char(tr.s[k])) {
          r += to;
          k += from.size();
        } else {
          r += tr.s[k];
          ++k;
        }
      }
      tr.s = std::move(r);
    }
    return tr;
  };
  int lost_eq = 0;
  const Value globals = exp.get("globals");
  const base::Array gl = globals.is_array() ? globals.as_array() : base::Array{};
  for (const Value& g : gl) {
    if (!g.truthy() || !g.get("name").is_string() || !g.get("expr").is_string()) {
      ++lost_eq;
      continue;
    }
    const std::string gname = g.get("name").as_string();
    if (const auto why = pa::name_problem(Value(gname), &params, nullptr, words)) {
      ++lost_eq;
      notes.push_back(words.t("swsketch.load.global_name", {{"name", slice16(gname, 40)}, {"why", *why}}));
      continue;
    }
    const Tr tr = sw_to_ours(g.get("expr").as_string());
    if (tr.unknown || !ex::parse(tr.s, std::nullopt, words).ok) {
      ++lost_eq;
      notes.push_back(tr.unknown ? words.t("swsketch.load.global_unknown", {{"name", gname}, {"ref", *tr.unknown}})
                                 : words.t("swsketch.load.global_unread", {{"name", gname}}));
      continue;
    }
    params.vars.push_back(
        {.name = gname, .expr = tr.s, .note = "From SolidWorks"});  // i18n-ignore: stored in the drawing
  }
  const Value equations = exp.get("equations");
  const base::Array eq = equations.is_array() ? equations.as_array() : base::Array{};
  for (const Value& q : eq) {
    if (!q.truthy() || !q.get("lhs").is_string() || !q.get("rhs").is_string()) {
      ++lost_eq;
      continue;
    }
    std::string lhs = q.get("lhs").as_string();
    if (lhs.starts_with('"')) {
      lhs.erase(0, 1);
    }
    if (lhs.ends_with('"')) {
      lhs.pop_back();
    }
    std::optional<std::string> ours;
    if (const auto it = dim_names.find(lhs); it != dim_names.end()) {
      ours = it->second;
    } else if (lhs.ends_with("@" + sk_name)) {
      if (const auto it2 = dim_names.find(lhs.substr(0, lhs.find('@'))); it2 != dim_names.end()) {
        ours = it2->second;
      }
    }
    if (!ours) {
      if (std::ranges::none_of(gl, [&lhs](const Value& g) { return g.get("name") == Value(lhs); })) {
        ++lost_eq;
      }
      continue;
    }
    const Tr tr = sw_to_ours(q.get("rhs").as_string());
    pa::Var* v = pa::var_by_name(params, *ours);
    if (tr.unknown || !ex::parse(tr.s, std::nullopt, words).ok || v == nullptr) {
      ++lost_eq;
      notes.push_back(words.t("swsketch.load.eq_unread", {{"lhs", lhs}, {"name", *ours}}));
      continue;
    }
    v->expr = tr.s;
  }
  if (lost_eq > 0) {
    notes.push_back(words.tn("swsketch.load.lost_eq", lost_eq));
  }
  const std::size_t n_cs = cs.size();
  doc.constraints = Value(std::move(cs));
  doc.params = pa::to_value(params);
  // resolve: equations drive the dimensions, the sketch solves (it should already agree)
  const params_doc::Result s = params_doc::solve_all(doc, "OPEN", words);
  if (!s.ok) {
    notes.push_back(words.t("swsketch.load.unsolved", {{"why", s.error}}));
  }
  doc.clear_history();
  doc.mark_saved();
  out.ok = true;
  out.sketch = sk_name;
  out.read = n_ents;
  out.constraints = n_cs;
  return out;
}

}  // namespace cc::doc::swsketch
