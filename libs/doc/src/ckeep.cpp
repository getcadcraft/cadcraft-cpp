// SPDX-License-Identifier: Apache-2.0
// ckeep.js (CadCraft 0.5) in C++.
#include "cc/doc/ckeep.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/cad/gsolve.hpp"
#include "cc/cad/params.hpp"
#include "cc/doc/gcons.hpp"

namespace cc::doc::ckeep {

using base::Value;
namespace gs = cad::gsolve;
namespace gc = gcons;
namespace pr = cad::params;

namespace {

double num(const Value& v) {
  return base::js_to_number(v);
}
Value at(const Value& a, std::size_t i) {
  return a.is_array() && i < a.as_array().size() ? a.as_array()[i] : Value();
}
const Value* entity(const Doc& doc, double id) {
  if (!std::isfinite(id) || id < 0 || id > 9e15 || id != std::floor(id)) {
    return nullptr;
  }
  return doc.model()->get(static_cast<Id>(id));
}

/// reSign(doc, c): the constraint with its signs, sides and fixed values taken from the geometry as it is now
std::optional<Value> re_sign(const Doc& doc, const Value& c, const i18n::Catalog& words) {
  gc::MakeOpts mo;
  mo.id = base::js_string(c.get("id"));
  if (const Value n = c.get("name"); n.is_string()) {
    mo.name = n.as_string();
  }
  if (const Value v = c.get("value"); v.is_number()) {
    mo.value = v.as_number();
  }
  auto [made, why] = gc::make_constraint(doc, base::js_string(c.get("type")), c.get("refs"), mo, words);
  if (!made) {
    return std::nullopt;
  }
  const Value& n = *made;
  Value out = c;
  for (const char* k : {"sign", "kind", "pair", "value", "fixed"}) {
    const std::string key(k);
    if (key == "value" && gc::is_dim(c)) {
      continue;  // a dimension keeps its value (a mirror changes no length or angle)
    }
    if (const Value* v = n.find(key)) {
      out.set(key, *v);
    } else if (key != "value") {
      out.erase(key);
    }
  }
  const Value dirs = c.get("dirs");
  if (base::js_string(c.get("type")) == "angle" && dirs.truthy()) {
    // the same sector as before (each leg's side), so only the turn's sense is worked out again
    const Value refs = c.get("refs");
    const auto line_of = [&](std::size_t i) -> const Value* {
      const Value* e = entity(doc, gs::ref_id(at(refs, i)));
      return e != nullptr && e->get("type") == Value("line") ? e : nullptr;
    };
    const Value* l1 = line_of(0);
    const Value* l2 = line_of(1);
    if (l1 != nullptr && l2 != nullptr) {
      const double d0 = num(at(dirs, 0));
      const double d1 = num(at(dirs, 1));
      const double ux = (num(l1->get("x2")) - num(l1->get("x1"))) * d0;
      const double uy = (num(l1->get("y2")) - num(l1->get("y1"))) * d0;
      const double vx = (num(l2->get("x2")) - num(l2->get("x1"))) * d1;
      const double vy = (num(l2->get("y2")) - num(l2->get("y1"))) * d1;
      out.set("sign", base::js_atan2(ux * vy - uy * vx, ux * vx + uy * vy) >= 0 ? 1 : -1);
      out.set("dirs", dirs);
    }
  }
  return out;
}

/// refWhat(type, k): what a reference holds on a line or an arc
std::string ref_what(const std::string& type, const std::string& k) {
  if (type == "line") {
    if (k == "p1") {
      return "start";
    }
    if (k == "p2") {
      return "end";
    }
    if (k == "mid") {
      return "mid";
    }
    return k == "L" ? "curve" : "";
  }
  if (type == "arc") {
    if (k == "s") {
      return "start";
    }
    if (k == "e") {
      return "end";
    }
    if (k == "mid") {
      return "mid";
    }
    return k == "c" || k == "R" ? "circle" : "";
  }
  if (type == "circle") {
    return k == "c" || k == "R" ? "circle" : "";
  }
  return "";
}

}  // namespace

double resign_among(Doc& doc, const std::set<double>& ids, const i18n::Catalog& words) {
  if (!doc.in_model() || !doc.constraints.is_array() || doc.constraints.as_array().empty()) {
    return 0;
  }
  double n = 0;
  base::Array cs;
  for (const Value& c : doc.constraints.as_array()) {
    const Value refs = c.get("refs");
    if (!std::ranges::all_of(refs.as_array(), [&ids](const Value& r) { return ids.contains(gs::ref_id(r)); })) {
      cs.push_back(c);
      continue;
    }
    auto k = re_sign(doc, c, words);
    if (k) {
      ++n;
      cs.push_back(std::move(*k));
    } else {
      cs.push_back(c);
    }
  }
  if (n > 0) {
    doc.set_parametric(Value(cs), doc.params);
  }
  return n;
}

double mirror_constraints(Doc& doc, const std::vector<std::pair<double, double>>& pairs, const i18n::Catalog& words) {
  if (!doc.in_model() || !doc.constraints.is_array() || doc.constraints.as_array().empty() || pairs.empty()) {
    return 0;
  }
  std::vector<std::pair<double, double>> index;
  std::vector<std::optional<double>> copies;
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    if (std::ranges::none_of(index, [&](const auto& x) { return x.first == pairs[i].first; })) {
      index.emplace_back(pairs[i].first, static_cast<double>(i));
    } else {
      for (auto& x : index) {
        if (x.first == pairs[i].first) {
          x.second = static_cast<double>(i);  // a Map keeps the last
        }
      }
    }
    copies.emplace_back(pairs[i].second);
  }
  const base::Array list = gc::clip_constraints(doc, index);
  if (list.empty()) {
    return 0;
  }
  std::set<std::string> before;
  for (const Value& c : doc.constraints.as_array()) {
    before.insert(base::js_string(c.get("id")));
  }
  const double n = gc::paste_constraints(doc, list, copies, 0, 0, words);
  if (n == 0) {
    return 0;
  }
  base::Array cs;
  for (const Value& c : doc.constraints.as_array()) {
    if (before.contains(base::js_string(c.get("id")))) {
      cs.push_back(c);
    } else {
      if (auto k = re_sign(doc, c, words)) {
        cs.push_back(std::move(*k));
      } else {
        cs.push_back(c);
      }
    }
  }
  doc.set_parametric(Value(cs), doc.params);
  return n;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — trimConstraints, as the JS
Kept trim_constraints(Doc& doc, const Value& old_e, const std::vector<Piece>& pieces, const i18n::Catalog& words) {
  Kept out;
  if (!doc.constraints.is_array() || doc.constraints.as_array().empty()) {
    return out;
  }
  const double id = num(old_e.get("id"));
  const auto mentions = [id](const Value& c) {
    return std::ranges::any_of(c.get("refs").as_array(), [id](const Value& r) { return gs::ref_id(r) == id; });
  };
  base::Array mine;
  base::Array others;
  for (const Value& c : doc.constraints.as_array()) {
    (mentions(c) ? mine : others).push_back(c);
  }
  if (mine.empty()) {
    return out;
  }
  const auto with_start = std::ranges::find_if(pieces, [](const Piece& p) { return p.t0 <= 1e-9; });
  const auto with_end = std::ranges::find_if(pieces, [](const Piece& p) { return p.t1 >= 1 - 1e-9; });
  const std::string type = base::js_string(old_e.get("type"));
  base::Array keep;
  base::Array dropped;
  for (const Value& c : mine) {
    if (type == "poly" || pieces.empty()) {  // a polyline's vertices are numbered again
      dropped.push_back(c);
      continue;
    }
    bool ok = true;
    bool on_all = false;
    const std::string ctype = base::js_string(c.get("type"));
    const Value crefs = c.get("refs");
    base::Array refs;
    for (const Value& r : crefs.as_array()) {
      if (gs::ref_id(r) != id) {
        refs.push_back(r);
        continue;
      }
      const std::string what = ref_what(type, base::js_string(r.get("k")));
      if (what == "start" && with_start != pieces.end()) {
        refs.push_back(base::make_object({{"e", with_start->id}, {"k", r.get("k")}}));
        continue;
      }
      if (what == "end" && with_end != pieces.end()) {
        refs.push_back(base::make_object({{"e", with_end->id}, {"k", r.get("k")}}));
        continue;
      }
      if (what == "curve" || what == "circle") {
        if (ctype == "equal" || (what == "circle" && ctype == "angle") || ctype == "fix") {
          ok = false;  // the length or the sweep changed
          refs.push_back(r);
          continue;
        }
        const bool direction =
            ctype == "horizontal" || ctype == "vertical" || ctype == "parallel" || ctype == "perpendicular";
        const auto on_me =
            std::ranges::count_if(crefs.as_array(), [id](const Value& q) { return gs::ref_id(q) == id; });
        on_all = direction && on_me == 1;  // a direction rule: every piece keeps it
        refs.push_back(base::make_object({{"e", pieces.front().id}, {"k", r.get("k")}}));
        continue;
      }
      ok = false;  // the middle moved, or the end it held is gone
      refs.push_back(r);
    }
    if (!ok) {
      dropped.push_back(c);
      continue;
    }
    if (ctype == "angle" && crefs.as_array().size() == 1) {  // an arc's sweep
      dropped.push_back(c);
      continue;
    }
    Value k = c;
    k.set("refs", Value(refs));
    keep.push_back(k);
    // the same rule on the other pieces too: copies without a name
    if (on_all) {
      for (std::size_t pi = 1; pi < pieces.size(); ++pi) {
        Value copy = c;
        copy.set("id", Value());  // id: undefined
        base::Array rs;
        for (std::size_t i = 0; i < refs.size(); ++i) {
          rs.push_back(gs::ref_id(at(crefs, i)) == id
                           ? base::make_object({{"e", pieces[pi].id}, {"k", refs[i].get("k")}})
                           : refs[i]);
        }
        copy.set("refs", Value(std::move(rs)));
        keep.push_back(std::move(copy));
      }
    }
  }
  // fresh ids for the copies, then signs from the cut geometry
  std::set<std::string> ids;
  for (const base::Array* list : {&others, &keep}) {
    for (const Value& c : *list) {
      if (const Value v = c.get("id"); v.is_string() && !v.as_string().empty()) {
        ids.insert(v.as_string());
      }
    }
  }
  int next = 1;
  const auto fresh = [&ids, &next] {
    while (ids.contains("k" + std::to_string(next))) {
      ++next;
    }
    ids.insert("k" + std::to_string(next));
    return "k" + std::to_string(next);
  };
  base::Array outs;
  for (Value& c : keep) {
    const Value cid = c.get("id");
    if (!cid.is_string() || cid.as_string().empty()) {
      c.set("id", fresh());
    }
    if (auto k = re_sign(doc, c, words)) {
      outs.push_back(std::move(*k));
    }
  }
  const auto lost = static_cast<double>(keep.size() - outs.size());
  base::Array all = others;
  all.insert(all.end(), outs.begin(), outs.end());
  doc.set_parametric(Value(all), pr::to_value(gc::drop_dim_params(pr::from_value(doc.params), dropped, words)));
  out.kept = static_cast<double>(outs.size());
  out.dropped = static_cast<double>(dropped.size()) + lost;
  return out;
}

}  // namespace cc::doc::ckeep
