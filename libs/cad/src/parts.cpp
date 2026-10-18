// SPDX-License-Identifier: Apache-2.0
#include "parts.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/base/json.hpp"
#include "props.hpp"

namespace cc::cad::detail {

// ---------------------------------------------------------------- attrs.js

Value attdef(const Value& t, const AttdefFlags& f) {
  const auto or0 = [](double v) { return v != 0 && !std::isnan(v) ? v : 0.0; };  // +t.x || 0
  const Value h = g(t, "h");
  Value d = base::make_object({
      {"tag", norm_tag(f.tag)},
      {"prompt", f.prompt},
      {"def", one_line(f.def)},
      {"x", or0(num(g(t, "x")))},
      {"y", or0(num(g(t, "y")))},
      {"h", num(h) > 0 ? Value(num(h)) : Value(2.5)},
      {"rot", or_(g(t, "rot"), 0)},
      {"ha", or_(g(t, "ha"), "left")},
      {"va", or_(g(t, "va"), "baseline")},
      {"wf", or_(g(t, "wf"), 1)},
      {"layer", nn(g(t, "layer"), "0")},
      {"color", nn(g(t, "color"), Value(nullptr))},
  });
  if (f.invisible) {
    d.set("invisible", true);
  }
  if (f.constant) {
    d.set("constant", true);
  }
  if (f.preset) {
    d.set("preset", true);
  }
  return d;
}

namespace {

bool same_spot(const Value& t, const Value& d, bool invisible) {
  const Value dh = g(d, "h");
  const double tol = 1e-6 * js_max(js_max(js_max(1, std::abs(dh.truthy() ? num(dh) : 1)), std::abs(num(g(d, "x")))),
                                   std::abs(num(g(d, "y"))));
  const Value tr = g(t, "rot");
  const Value dr = g(d, "rot");
  const double da = std::abs(norm_angle((tr.truthy() ? num(tr) : 0) - (dr.truthy() ? num(dr) : 0)));
  const auto s_or = [](const Value& v, const char* def) { return v.truthy() ? v : Value(def); };
  return std::abs(num(g(t, "x")) - num(g(d, "x"))) <= tol && std::abs(num(g(t, "y")) - num(g(d, "y"))) <= tol &&
         std::abs(num(g(t, "h")) - num(dh)) <= tol * 10 && js_min(da, 2 * std::numbers::pi - da) < 1e-6 &&
         s_or(g(t, "ha"), "left") == s_or(g(d, "ha"), "left") &&
         s_or(g(t, "va"), "baseline") == s_or(g(d, "va"), "baseline") && invisible == g(d, "invisible").truthy() &&
         nn(g(t, "layer"), "0") == nn(g(d, "layer"), "0") &&
         nn(g(t, "color"), Value(nullptr)) == nn(g(d, "color"), Value(nullptr));
}

}  // namespace

void resolve_attribs(const std::vector<Array*>& lists, const Array& blocks) {
  std::unordered_map<std::string, const Value*> by_name;  // new Map(blocks.map(b => [b.name, b])): the last one wins
  for (const Value& b : blocks) {
    by_name[str(g(b, "name"))] = &b;
  }
  for (Array* list : lists) {
    for (Value& e : *list) {
      if (!e.is_object() || g(e, "type") != Value("insert")) {
        continue;
      }
      const Value* rawp = e.find("_attribs");
      if (rawp == nullptr || !rawp->truthy()) {
        continue;
      }
      const Value raw = *rawp;
      e.erase("_attribs");
      if (len(raw) == 0) {
        continue;
      }
      const auto it = by_name.find(str(g(e, "block")));
      const Value* blk = it == by_name.end() ? nullptr : it->second;
      Mat inv{};
      const bool has_inv = blk != nullptr && m_inverse(insert_matrix(e, *blk), inv);
      Value attrs = Value::object();
      Value attloc = Value::object();
      for (const Value& a : raw.as_array()) {
        const std::string tag = norm_tag(g(a, "tag"));
        if (tag.empty() || attrs.has(tag)) {
          continue;
        }
        attrs.append(tag, one_line(str(g(a, "s"))));
        if (!has_inv) {
          continue;
        }
        Value as_text = a;
        as_text.set("type", "text");
        const Value local = tx_text(as_text, inv);
        const Value* d = nullptr;
        if (const Value ads = g(*blk, "attdefs"); ads.is_array()) {
          for (const Value& x : at(*blk, "attdefs").as_array()) {
            if (g(x, "tag") == Value(tag)) {
              d = &x;
              break;
            }
          }
        }
        if (d != nullptr && same_spot(local, *d, g(a, "invisible").truthy())) {
          continue;
        }
        Value loc = base::make_object({
            {"x", g(local, "x")},
            {"y", g(local, "y")},
            {"h", g(local, "h")},
            {"rot", or_(g(local, "rot"), 0)},
            {"ha", or_(g(a, "ha"), "left")},
            {"va", or_(g(a, "va"), "baseline")},
            {"wf", or_(g(a, "wf"), 1)},
            {"layer", nn(g(a, "layer"), "0")},
            {"color", nn(g(a, "color"), Value(nullptr))},
        });
        if (g(a, "invisible").truthy()) {
          loc.set("invisible", true);
        }
        attloc.set(tag, std::move(loc));
      }
      e.set("attrs", std::move(attrs));
      if (!attloc.as_object().empty()) {
        e.set("attloc", std::move(attloc));
      }
    }
  }
}

// ---------------------------------------------------------------- sheets.js

std::string match_paper(double w, double h, std::string_view unit) {
  const auto unit_mm = [](std::string_view u) -> double {
    if (u == "millimeters" || u == "mm") {
      return 1;
    }
    if (u == "centimeters") {
      return 10;
    }
    if (u == "meters") {
      return 1000;
    }
    if (u == "kilometers") {
      return 1e6;
    }
    if (u == "inches" || u == "in") {
      return 25.4;
    }
    if (u == "feet") {
      return 304.8;
    }
    if (u == "yards") {
      return 914.4;
    }
    if (u == "miles") {
      return 1609344;
    }
    if (u == "microinches") {
      return 25.4e-6;
    }
    if (u == "mils") {
      return 0.0254;
    }
    return kNaN;
  };
  struct Paper {
    std::string_view key;
    double w, h;
    std::string_view unit;
  };
  static constexpr std::array<Paper, 13> kPapers = {{{"iso-a4", 297, 210, "mm"},
                                                     {"iso-a3", 420, 297, "mm"},
                                                     {"iso-a2", 594, 420, "mm"},
                                                     {"iso-a1", 841, 594, "mm"},
                                                     {"iso-a0", 1189, 841, "mm"},
                                                     {"letter", 11, 8.5, "in"},
                                                     {"legal", 14, 8.5, "in"},
                                                     {"ansi-b", 17, 11, "in"},
                                                     {"ansi-c", 22, 17, "in"},
                                                     {"ansi-d", 34, 22, "in"},
                                                     {"arch-c", 24, 18, "in"},
                                                     {"arch-d", 36, 24, "in"},
                                                     {"arch-e", 48, 36, "in"}}};
  double k = unit_mm(unit);
  if (!(k != 0) || std::isnan(k)) {
    k = 1;  // UNIT_MM[unit] || 1
  }
  const double a = js_max(w, h) * k;
  const double b = js_min(w, h) * k;
  for (const Paper& p : kPapers) {
    const double pk = unit_mm(p.unit);
    if (std::abs(p.w * pk - a) <= a * 0.005 && std::abs(p.h * pk - b) <= b * 0.005) {
      return std::string(p.key);
    }
  }
  return {};
}

// ---------------------------------------------------------------- panes.js / dwg-vports.js

namespace {

constexpr std::array<std::string_view, 6> kLayouts = {"single", "2v", "2h", "3r", "3l", "4"};

std::size_t pane_count(std::string_view layout) {
  if (layout == "2v" || layout == "2h") {
    return 2;
  }
  if (layout == "3r" || layout == "3l") {
    return 3;
  }
  if (layout == "4") {
    return 4;
  }
  return 1;
}

double clamp_split(const Value& f) {
  return finite(f) ? js_max(0.1, js_min(0.9, f.as_number())) : 0.5;
}

struct Corner {
  double x0, y0, x1, y1;
};

std::vector<Corner> corners_of(std::string_view layout, double X, double Y) {
  const double yu = 1 - Y;
  if (layout == "2v") {
    return {{0, 0, X, 1}, {X, 0, 1, 1}};
  }
  if (layout == "2h") {
    return {{0, yu, 1, 1}, {0, 0, 1, yu}};
  }
  if (layout == "3r") {
    return {{0, yu, X, 1}, {0, 0, X, yu}, {X, 0, 1, 1}};
  }
  if (layout == "3l") {
    return {{0, 0, X, 1}, {X, yu, 1, 1}, {X, 0, 1, yu}};
  }
  if (layout == "4") {
    return {{0, yu, X, 1}, {X, yu, 1, 1}, {0, 0, X, yu}, {X, 0, 1, yu}};
  }
  return {{0, 0, 1, 1}};
}

struct Fit {
  std::string layout;
  double fx = 0, fy = 0;
  std::vector<std::size_t> order;
};

bool layout_from_corners(const std::vector<const Value*>& list, Fit& fit) {
  std::vector<const Value*> rs;
  for (const Value* r : list) {
    const Value x0 = g(*r, "x0");
    const Value y0 = g(*r, "y0");
    const Value x1 = g(*r, "x1");
    const Value y1 = g(*r, "y1");
    if (finite(x0) && finite(y0) && finite(x1) && finite(y1) && x1.as_number() > x0.as_number() &&
        y1.as_number() > y0.as_number()) {
      rs.push_back(r);
    }
  }
  if (rs.size() < 2 || rs.size() > 4) {
    return false;
  }
  const auto uniq = [&](const char* a, const char* b) {
    std::vector<double> out;
    for (const Value* r : rs) {
      for (const char* k : {a, b}) {
        const double v = base::js_round(num(g(*r, k)) * 1e4) / 1e4;
        if (std::ranges::find(out, v) == out.end()) {
          out.push_back(v);
        }
      }
    }
    std::erase_if(out, [](double v) { return v <= 0.001 || v >= 0.999; });
    return out;
  };
  const std::vector<double> xs = uniq("x0", "x1");
  const std::vector<double> ys = uniq("y0", "y1");
  const double X = xs.empty() ? 0.5 : xs[0];
  const double Y = ys.empty() ? 0.5 : 1 - ys[0];
  for (const std::string_view layout : kLayouts) {
    if (pane_count(layout) != rs.size()) {
      continue;
    }
    std::vector<std::size_t> order;
    bool ok = true;
    for (const Corner& w : corners_of(layout, X, Y)) {
      std::size_t found = rs.size();
      for (std::size_t i = 0; i < rs.size(); ++i) {
        const Value& r = *rs[i];
        if (std::abs(num(g(r, "x0")) - w.x0) < 0.01 && std::abs(num(g(r, "y0")) - w.y0) < 0.01 &&
            std::abs(num(g(r, "x1")) - w.x1) < 0.01 && std::abs(num(g(r, "y1")) - w.y1) < 0.01) {
          found = i;
          break;
        }
      }
      if (found == rs.size() || std::ranges::find(order, found) != order.end()) {
        ok = false;
        break;
      }
      order.push_back(found);
    }
    if (ok) {
      fit = {std::string(layout), clamp_split(X), clamp_split(Y), std::move(order)};
      return true;
    }
  }
  return false;
}

Value clean_panes(const std::string& layout, double fx, double fy, double active, const Array& in) {
  if (std::ranges::find(kLayouts, std::string_view(layout)) == kLayouts.end() || layout == "single") {
    return {nullptr};
  }
  const std::size_t n = pane_count(layout);
  Array list;
  for (std::size_t i = 0; i < in.size() && i < n; ++i) {
    const Value& p = in[i];
    const Value cx = g(p, "cx");
    const Value cy = g(p, "cy");
    const Value h = g(p, "h");
    const bool ok = p.truthy() && finite(cx) && finite(cy) && finite(h) && h.as_number() > 0 &&
                    std::abs(cx.as_number()) < 1e15 && std::abs(cy.as_number()) < 1e15 && h.as_number() < 1e15;
    const Value twv = g(p, "tw");
    const double tw = finite(twv) && std::abs(twv.as_number()) <= 100 ? twv.as_number() : 0;
    if (!ok) {
      list.emplace_back(nullptr);
      continue;
    }
    Value o = base::make_object({{"cx", cx}, {"cy", cy}, {"h", h}});
    if (tw != 0) {
      o.set("tw", tw);
    }
    if (const Value sh = g(p, "sheet"); sh.is_string() && !sh.as_string().empty()) {
      o.set("sheet", js_slice(sh.as_string(), 255));
    }
    list.push_back(std::move(o));
  }
  while (list.size() < n) {
    list.emplace_back(nullptr);
  }
  const bool int_ok =
      std::isfinite(active) && std::trunc(active) == active && active >= 0 && active < static_cast<double>(n);
  return base::make_object({{"layout", layout},
                            {"fx", clamp_split(fx)},
                            {"fy", clamp_split(fy)},
                            {"active", int_ok ? active : 0},
                            {"list", std::move(list)}});
}

}  // namespace

Value clean_panes_value(const Value& v) {
  if (!v.is_object() || v.is_point()) {
    return {nullptr};
  }
  const Value layout = v.get("layout");
  const Value list = v.get("list");
  const Value fx = v.get("fx");
  const Value fy = v.get("fy");
  const Value active = v.get("active");
  return clean_panes(layout.is_string() ? layout.as_string() : std::string("?"), finite(fx) ? fx.as_number() : kNaN,
                     finite(fy) ? fy.as_number() : kNaN, active.is_number() ? active.as_number() : kNaN,
                     list.is_array() ? list.as_array() : Array{});
}

Value vports_from(const Array& records) {
  std::vector<const Value*> rs;
  for (const Value& r : records) {
    if (r.truthy() && finite(g(r, "x0")) && finite(g(r, "y0")) && finite(g(r, "x1")) && finite(g(r, "y1")) &&
        finite(g(r, "cx")) && finite(g(r, "cy")) && finite(g(r, "h"))) {
      rs.push_back(&r);
    }
  }
  if (rs.size() < 2) {
    return {nullptr};
  }
  Fit fit;
  if (!layout_from_corners(rs, fit)) {
    return {nullptr};
  }
  Array list;
  for (const std::size_t k : fit.order) {
    const Value& r = *rs.at(k);  // the order indexes layoutFromCorners's own filtered list, applied to this one (as JS)
    Value o = base::make_object({{"cx", g(r, "cx")}, {"cy", g(r, "cy")}, {"h", g(r, "h")}});
    if (const Value tw = g(r, "tw");
        finite(tw) && std::abs(tw.as_number()) > 1e-12 && std::abs(tw.as_number()) <= 100) {
      o.set("tw", tw);
    }
    if (const Value sh = g(r, "sheet"); sh.is_string() && !sh.as_string().empty()) {
      o.set("sheet", sh);
    }
    list.push_back(std::move(o));
  }
  const auto zero = std::ranges::find(fit.order, std::size_t{0});
  const double active = zero == fit.order.end() ? 0 : static_cast<double>(zero - fit.order.begin());
  return clean_panes(fit.layout, fit.fx, fit.fy, active, list);
}

// ---------------------------------------------------------------- pictures.js / xclip.js

namespace {

std::array<std::array<double, 2>, 4> pic_corners(const Value& pic) {
  const double w = num(g(pic, "w"));
  const double h = num(g(pic, "h"));
  const double Wx = num(g(pic, "ux")) * w;
  const double Wy = num(g(pic, "uy")) * w;
  const double Hx = num(g(pic, "vx")) * h;
  const double Hy = num(g(pic, "vy")) * h;
  const double x = num(g(pic, "x"));
  const double y = num(g(pic, "y"));
  return {{{x, y}, {x + Wx, y + Wy}, {x + Wx + Hx, y + Wy + Hy}, {x + Hx, y + Hy}}};
}

}  // namespace

Value picture_block(const std::string& name, const Value& pic, const Value* clip) {
  const auto c = pic_corners(pic);
  Array pts;
  const bool clipped = clip != nullptr && len(*clip) >= 3;
  if (clipped) {
    for (const Value& p : clip->as_array()) {
      pts.push_back(base::make_object({{"x", g(p, "x")}, {"y", g(p, "y")}, {"b", 0}}));
    }
  } else {
    for (const auto& p : c) {
      pts.push_back(base::make_object({{"x", p[0]}, {"y", p[1]}, {"b", 0}}));
    }
  }
  const double wlen = base::js_hypot(c[1][0] - c[0][0], c[1][1] - c[0][1]);
  const double hlen = base::js_hypot(c[3][0] - c[0][0], c[3][1] - c[0][1]);
  const double rot = base::js_atan2(c[1][1] - c[0][1], c[1][0] - c[0][0]);
  // String(pic.file || '').split(/[\\/]/).pop() || 'image'
  std::string file = g(pic, "file").truthy() ? str(g(pic, "file")) : std::string();
  if (const std::size_t sl = file.find_last_of("\\/"); sl != std::string::npos) {
    file = file.substr(sl + 1);
  }
  if (file.empty()) {
    file = "image";
  }
  const double th = js_max(1e-9, js_min(hlen / 8, wlen / js_max(4, static_cast<double>(base::utf16_length(file)))));
  const double k = th * 0.5;
  Value picv = pic;
  if (clipped) {
    picv.set("clipped", true);
  }
  Array ents;
  ents.push_back(base::make_object({{"type", "poly"},
                                    {"layer", "0"},
                                    {"color", 0},
                                    {"closed", true},
                                    {"pts", std::move(pts)},
                                    {"pic", std::move(picv)}}));
  const double tau = 2 * std::numbers::pi;
  ents.push_back(base::make_object({{"type", "text"},
                                    {"layer", "0"},
                                    {"color", 0},
                                    {"x", c[0][0] + (cc::base::js_cos(rot) - cc::base::js_sin(rot)) * k},
                                    {"y", c[0][1] + (cc::base::js_sin(rot) + cc::base::js_cos(rot)) * k},
                                    {"h", th},
                                    {"rot", std::fmod(std::fmod(rot, tau) + tau, tau)},
                                    {"s", file},
                                    {"ha", "left"},
                                    {"va", "baseline"},
                                    {"picName", true}}));
  return base::make_object(
      {{"name", name}, {"base", base::make_object({{"x", 0}, {"y", 0}})}, {"entities", std::move(ents)}});
}

Value clip_from_filter(const Value& o) {
  std::vector<std::array<double, 2>> ps;
  if (const Value& pts = at(o, "points"); pts.is_array()) {
    for (const Value& p : pts.as_array()) {
      if (finite(g(p, "x")) && finite(g(p, "y"))) {
        ps.push_back({g(p, "x").as_number(), g(p, "y").as_number()});
      }
    }
  }
  if (ps.size() < 2) {
    return {nullptr};
  }
  const auto mat = [&](std::string_view key) {
    std::array<double, 12> m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const Value& a = at(o, key);
    if (a.is_array() && a.as_array().size() == 12 &&
        std::ranges::all_of(a.as_array(), [](const Value& x) { return finite(x); })) {
      for (std::size_t i = 0; i < 12; ++i) {
        m[i] = a.as_array()[i].as_number();
      }
    }
    return m;
  };
  const auto T = mat("clipTransform");
  const auto I = mat("inverseBlockTransform");
  const auto ap = [](const std::array<double, 12>& m, std::array<double, 2> p) {
    return std::array<double, 2>{m[0] * p[0] + m[1] * p[1] + m[3], m[4] * p[0] + m[5] * p[1] + m[7]};
  };
  if (ps.size() == 2) {
    ps = {ps[0], {ps[1][0], ps[0][1]}, ps[1], {ps[0][0], ps[1][1]}};
  }
  Array out;
  for (const auto& p : ps) {
    const auto q = ap(I, ap(T, p));
    out.push_back(base::pt2(q[0], q[1]));
  }
  return base::make_object({{"pts", std::move(out)}});
}

// ---------------------------------------------------------------- model3d.js

Value face3d_from_dwg(const Value& e, const Value& base) {
  const Value& p = at(e, "points");
  const std::array<Value, 4> raw = {item(p, 0), item(p, 1), item(p, 2), item(p, 3).truthy() ? item(p, 3) : item(p, 2)};
  Array pts;
  for (const Value& q : raw) {
    if (q.truthy() && finite(g(q, "x")) && finite(g(q, "y"))) {
      const Value z = g(q, "z");
      pts.emplace_back(Array{g(q, "x"), g(q, "y"), finite(z) ? z : Value(0)});
    }
  }
  if (pts.size() < 3) {
    return {nullptr};
  }
  Value out = base::make_object({{"type", "face3d"}});
  for (const base::Member& m : base.as_object()) {
    out.set(m.key, m.value);
  }
  out.set("pts", std::move(pts));
  return out;
}

namespace {

constexpr std::size_t kMaxPlaced = 200000;

std::string cadcraft_string(const Value& xdata, bool& found) {
  found = false;
  if (!xdata.is_object() || xdata.is_point()) {
    return {};
  }
  for (const base::Member* mp : base::js_members(xdata.as_object())) {
    const base::Member& m = *mp;
    if (base::js_to_upper(m.key) != "CADCRAFT") {
      continue;
    }
    if (!m.value.is_array() || m.value.as_array().empty()) {
      return {};
    }
    const Value& first = m.value.as_array()[0];
    if (first.is_array() && item(first, 0) == Value(1000)) {
      found = true;
      const Value& v = item(first, 1);
      return nullish(v) ? std::string() : str(v);
    }
    return {};
  }
  return {};
}

/// /^PREFIX(\d{1,7}):(.*)$/s → (n, rest)
bool numbered(std::string_view s, std::string_view prefix, double& n, std::string& rest) {
  if (!s.starts_with(prefix)) {
    return false;
  }
  std::size_t i = prefix.size();
  const std::size_t d0 = i;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    ++i;
  }
  if (i == d0 || i - d0 > 7 || i >= s.size() || s[i] != ':') {
    return false;
  }
  n = base::js_parse_int(s.substr(d0, i - d0));
  rest = std::string(s.substr(i + 1));
  return true;
}

std::string joined_numbered(const std::vector<std::string>& ccs, std::string_view prefix, bool& any) {
  std::vector<std::pair<double, std::string>> parts;
  for (const std::string& s : ccs) {
    double n = 0;
    std::string rest;
    if (numbered(s, prefix, n, rest)) {
      parts.emplace_back(n, std::move(rest));
    }
  }
  std::ranges::stable_sort(parts, {}, &std::pair<double, std::string>::first);
  std::string out;
  for (auto& [n, s] : parts) {
    out += s;
  }
  any = !parts.empty();
  return out;
}

/// x.toPrecision(12) read back as a number
double precision12(double x) {
  std::array<char, 40> buf{};
  const int n = std::snprintf(buf.data(), buf.size(), "%.11e", x);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  return n > 0 ? base::js_parse_float(std::string_view(buf.data(), static_cast<std::size_t>(n))) : x;
}

Value clean_view(const Value& v) {
  if (!v.is_object()) {
    return {nullptr};
  }
  const auto P = [](const Value& p) -> Value {
    if (!p.is_array() || p.as_array().size() != 3) {
      return {nullptr};
    }
    Array out;
    for (const Value& x : p.as_array()) {
      if (!finite(x) || std::abs(x.as_number()) >= 1e9) {
        return {nullptr};
      }
      out.emplace_back(precision12(x.as_number()));
    }
    return out;
  };
  const Value eye = P(g(v, "eye"));
  const Value target = P(g(v, "target"));
  if (eye.is_null() || target.is_null()) {
    return {nullptr};
  }
  const auto& e = eye.as_array();
  const auto& t = target.as_array();
  const double dx = e[0].as_number() - t[0].as_number();
  const double dy = e[1].as_number() - t[1].as_number();
  const double dz = e[2].as_number() - t[2].as_number();
  if (std::sqrt(dx * dx + dy * dy + dz * dz) < 1e-9) {
    return {nullptr};
  }
  const Value fovv = g(v, "fov");
  const double fov = finite(fovv) && fovv.as_number() >= 5 && fovv.as_number() <= 120 ? fovv.as_number() : 35;
  Value out =
      base::make_object({{"eye", eye}, {"target", target}, {"persp", g(v, "persp") != Value(false)}, {"fov", fov}});
  const Value st = g(v, "style");
  if (st.is_string() && (st.as_string() == "shaded" || st.as_string() == "edges" || st.as_string() == "hidden" ||
                         st.as_string() == "wire" || st.as_string() == "xray")) {
    out.set("style", st);
  }
  return out;
}

Mat insert_matrix2(const Value& e, const Value& b) {
  const Value rot = g(e, "rot");
  const double r = rot.truthy() ? num(rot) : 0;
  const double c = cc::base::js_cos(r);
  const double s = cc::base::js_sin(r);
  const Value sxv = g(e, "sx");
  const Value syv = g(e, "sy");
  const double sx = nullish(sxv) ? 1 : num(sxv);
  const double sy = nullish(syv) ? 1 : num(syv);
  const Value base = g(b, "base");
  const Value bxv = base.truthy() ? g(base, "x") : Value();
  const Value byv = base.truthy() ? g(base, "y") : Value();
  const double bx = bxv.truthy() ? num(bxv) : 0;
  const double by = byv.truthy() ? num(byv) : 0;
  const double a = c * sx;
  const double bb = s * sx;
  const double cc = -s * sy;
  const double d = c * sy;
  const Value ex = g(e, "x");
  const Value ey = g(e, "y");
  return {
      a, bb, cc, d, (ex.truthy() ? num(ex) : 0) - (a * bx + cc * by), (ey.truthy() ? num(ey) : 0) - (bb * bx + d * by)};
}

bool is_face(const Value& e) {
  return e.is_object() && g(e, "type") == Value("face3d");
}

}  // namespace

Value split3d(Value& input, const i18n::Catalog& words) {
  Array faces = g(input, "faces3d").is_array() ? at(input, "faces3d").as_array() : Array{};
  // strip(input.entities, kept): face3d entities out, in their order
  if (Value* ents = input.find("entities"); ents != nullptr && ents->is_array()) {
    Array keep;
    Array rest;
    for (Value& e : ents->as_array()) {
      (is_face(e) ? keep : rest).push_back(std::move(e));
    }
    ents->as_array() = std::move(rest);
    for (Value& f : keep) {
      faces.push_back(std::move(f));
    }
  }
  std::size_t flat = 0;
  std::size_t placed = 0;
  Value* blocks = input.find("blocks");
  if (blocks != nullptr && blocks->is_array()) {
    std::erase_if(blocks->as_array(), [](const Value& b) { return is_m3d_block(g(b, "name")); });
    std::unordered_map<std::string, Array> own;
    for (const Value& b : blocks->as_array()) {
      Array list;
      if (const Value& es = at(b, "entities"); es.is_array()) {
        for (const Value& e : es.as_array()) {
          if (is_face(e)) {
            list.push_back(e);
          }
        }
      }
      if (!list.empty()) {
        own[str(g(b, "name"))] = std::move(list);
      }
    }
    if (!own.empty()) {
      std::unordered_map<std::string, const Value*> by_name;
      for (const Value& b : blocks->as_array()) {
        by_name[str(g(b, "name"))] = &b;
      }
      Array out;
      // NOLINTNEXTLINE(misc-no-recursion) — depth ≤ 16
      std::function<void(const Value&, const Mat&, int)> walk = [&](const Value& list, const Mat& m, int depth) {
        if (depth > 16 || out.size() > kMaxPlaced || !list.is_array()) {
          return;
        }
        for (const Value& e : list.as_array()) {
          if (!e.is_object() || g(e, "type") != Value("insert")) {
            continue;
          }
          const auto bi = by_name.find(str(g(e, "block")));
          if (bi == by_name.end()) {
            continue;
          }
          const Value& b = *bi->second;
          const Mat mm = m_mul(m, insert_matrix2(e, b));
          if (const auto oi = own.find(str(g(b, "name"))); oi != own.end()) {
            for (const Value& f : oi->second) {
              Value c = f;
              const Value fl = g(f, "layer");
              c.set("layer", fl == Value("0") ? or_(g(e, "layer"), "0") : fl);
              Array pts;
              for (const Value& q : at(f, "pts").as_array()) {
                const double q0 = num(item(q, 0));
                const double q1 = num(item(q, 1));
                const Value q2 = item(q, 2);
                pts.emplace_back(Array{mm[0] * q0 + mm[2] * q1 + mm[4], mm[1] * q0 + mm[3] * q1 + mm[5],
                                       q2.truthy() ? q2 : Value(0)});
              }
              c.set("pts", std::move(pts));
              out.push_back(std::move(c));
            }
          }
          walk(at(b, "entities"), mm, depth + 1);
        }
      };
      walk(at(input, "entities"), Mat{1, 0, 0, 1, 0, 0}, 0);
      placed = std::min(out.size(), kMaxPlaced);
      for (std::size_t i = 0; i < placed; ++i) {
        faces.push_back(std::move(out[i]));
      }
    }
  }
  const auto to_outlines = [&](Value* list) {
    if (list == nullptr || !list->is_array()) {
      return;
    }
    Array kept;
    for (Value& e : list->as_array()) {
      if (!is_face(e)) {
        kept.push_back(std::move(e));
        continue;
      }
      Array pts;
      for (const Value& q : at(e, "pts").as_array()) {
        const double x = num(item(q, 0));
        const double y = num(item(q, 1));
        if (pts.empty() || base::js_hypot(x - num(g(pts.back(), "x")), y - num(g(pts.back(), "y"))) > 1e-12) {
          pts.push_back(base::make_object({{"x", x}, {"y", y}, {"b", 0}}));
        }
      }
      if (pts.size() > 1 && base::js_hypot(num(g(pts.front(), "x")) - num(g(pts.back(), "x")),
                                           num(g(pts.front(), "y")) - num(g(pts.back(), "y"))) <= 1e-12) {
        pts.pop_back();
      }
      ++flat;
      if (pts.size() >= 2) {
        const bool closed = pts.size() > 2;
        kept.push_back(base::make_object({{"type", "poly"},
                                          {"layer", or_(g(e, "layer"), "0")},
                                          {"color", nn(g(e, "color"), Value(nullptr))},
                                          {"closed", closed},
                                          {"pts", std::move(pts)}}));
      }
    }
    list->as_array() = std::move(kept);
  };
  if (blocks != nullptr && blocks->is_array()) {
    for (Value& b : blocks->as_array()) {
      to_outlines(b.find("entities"));
    }
  }
  if (Value* layouts = input.find("layouts"); layouts != nullptr && layouts->is_array()) {
    for (Value& l : layouts->as_array()) {
      to_outlines(l.find("entities"));
    }
  }
  if (Value* report = input.find("report"); report != nullptr && report->truthy()) {
    if (Value* notes = report->find("notes"); notes != nullptr && notes->is_array()) {
      if (flat != 0) {
        notes->push(words.tn("model3d.read.flat", static_cast<double>(flat)));
      }
      if (placed != 0) {
        notes->push(words.tn("model3d.read.placed", static_cast<double>(placed)));
      }
    }
  }
  Value faces_out = faces;
  if (!faces.empty()) {
    input.set("faces3d", std::move(faces));
  }
  const Value m3 = input.get("model3d");
  return base::make_object({{"text", m3.is_string() ? m3 : Value(nullptr)},
                            {"faces", std::move(faces_out)},
                            {"view", clean_view(input.get("view3d"))}});
}

void read3d_from_dwg(const Value& db, Value& drawing, const i18n::Catalog& words) {
  if (const Value& blocks = at(db, "blocks"); blocks.is_array()) {
    for (const Value& b : blocks.as_array()) {
      if (!is_m3d_block(g(b, "name"))) {
        continue;
      }
      std::vector<std::string> ccs;
      if (const Value& es = at(b, "entities"); es.is_array()) {
        for (const Value& e : es.as_array()) {
          bool found = false;
          std::string s = cadcraft_string(g(e, "xdata"), found);
          ccs.push_back(found ? s : std::string());  // cadcraftString(e.xdata) ?? ''
        }
      }
      bool any = false;
      const std::string text = joined_numbered(ccs, "CC3D:", any);
      if (any && !text.empty()) {
        drawing.set("model3d", text);
      }
      for (const std::string& s : ccs) {
        if (s.starts_with("CCCAM:") && base::utf16_length(s) < 2000) {
          auto parsed = base::parse_json(std::string_view(s).substr(6));
          const Value view = parsed ? clean_view(*parsed) : Value(nullptr);
          if (!view.is_null()) {
            drawing.set("view3d", view);
          }
          break;
        }
      }
      break;
    }
  }
  split3d(drawing, words);
}

// ---------------------------------------------------------------- gcons.js

bool is_param_block(const Value& name) {
  return name.truthy() && base::js_to_upper(str(name)) == "CADCRAFT_PARAMETRIC";
}
bool is_m3d_block(const Value& name) {
  return name.truthy() && base::js_to_upper(str(name)) == "CADCRAFT_3D";
}

void tag_from_xdata(Value& conv, const Value& xdata) {
  if (!conv.truthy() || conv.is_array()) {
    return;
  }
  bool found = false;
  const std::string first = cadcraft_string(xdata, found);
  if (!found) {
    return;
  }
  // refKeyIn([first]): /^CCREF:(\d{1,9})$/
  if (first.starts_with("CCREF:")) {
    const std::string_view d = std::string_view(first).substr(6);
    if (!d.empty() && d.size() <= 9 && std::ranges::all_of(d, [](char c) { return c >= '0' && c <= '9'; })) {
      conv.set("ccref", std::string(d));
    }
  }
  if (first.starts_with("CCPARAM:")) {
    conv.set("cc", Array{Value(first)});
  }
}

std::string parametric_text(const Array& ents) {
  std::vector<std::string> ccs;
  for (const Value& e : ents) {
    const Value& cc = at(e, "cc");
    if (!cc.is_array()) {
      continue;
    }
    const Value& first = item(cc, 0);
    ccs.push_back(nullish(first) ? std::string() : str(first));
  }
  bool any = false;
  return joined_numbered(ccs, "CCPARAM:", any);
}

// ---------------------------------------------------------------- dyn-blocks.js

std::string hex_key(const Value& h) {
  if (nullish(h)) {
    return {};
  }
  std::string s = base::js_to_upper(str(h));
  std::size_t z = 0;
  while (z + 1 < s.size() && s[z] == '0') {
    ++z;
  }
  return s.substr(z);
}

namespace {

std::string clean_name(const Value& v) {
  std::string s = nullish(v) ? std::string() : str(v);
  std::erase_if(s, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
  return js_slice(s, 255);
}

bool dyn_class(
    const std::string& n) {  // /^(BLOCK\w*|ACAD_EVALUATION_GRAPH|ACDB_BLOCKREPRESENTATION_DATA|ACDB_DYNAMICBLOCK\w*)$/
  const auto word = [](std::string_view s) {
    return std::ranges::all_of(s, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; });
  };
  if (n.starts_with("BLOCK") && word(std::string_view(n).substr(5))) {
    return true;
  }
  if (n == "ACAD_EVALUATION_GRAPH" || n == "ACDB_BLOCKREPRESENTATION_DATA") {
    return true;
  }
  return n.starts_with("ACDB_DYNAMICBLOCK") && word(std::string_view(n).substr(17));
}

Array dyn_objects(const Value& db) {
  const Value& objs = at(db, "objects");
  std::unordered_map<std::string, const Value*> by_h;
  std::unordered_map<std::string, std::vector<const Value*>> kids;
  if (objs.is_array()) {
    for (const Value& o : objs.as_array()) {
      by_h[hex_key(g(o, "handle"))] = &o;
    }
    for (const Value& o : objs.as_array()) {
      const std::string k = hex_key(g(o, "owner"));
      if (!k.empty()) {
        kids[k].push_back(&o);
      }
    }
  }
  std::unordered_map<std::string, const Value*> recs;
  if (const Value& br = at(at(db, "tables"), "blockRecords"); br.is_array()) {
    for (const Value& r : br.as_array()) {
      recs[hex_key(g(r, "handle"))] = &r;
    }
  }
  std::deque<std::string> queue;
  if (const Value& blocks = at(db, "blocks"); blocks.is_array()) {
    for (const Value& b : blocks.as_array()) {
      if (g(b, "dynamic").truthy() || g(b, "dynamicOf").truthy()) {
        const auto r = recs.find(hex_key(g(b, "record")));
        if (r != recs.end()) {
          if (const Value x = g(*r->second, "extDict"); x.truthy()) {
            queue.push_back(hex_key(x));
          }
        }
      }
      if (const Value& es = at(b, "entities"); es.is_array()) {
        for (const Value& e : es.as_array()) {
          if (g(e, "dyn").truthy() && g(e, "extDict").truthy()) {
            queue.push_back(hex_key(g(e, "extDict")));
          }
        }
      }
    }
  }
  std::vector<std::string> keep;
  std::set<std::string> seen;
  while (!queue.empty()) {
    const std::string k = std::move(queue.front());
    queue.pop_front();
    if (k.empty() || seen.contains(k) || !by_h.contains(k)) {
      continue;
    }
    seen.insert(k);
    keep.push_back(k);
    const Value& o = *by_h[k];
    if (const Value x = g(o, "extDict"); x.truthy()) {
      queue.push_back(hex_key(x));
    }
    if (const Value& en = at(o, "entries"); en.truthy() && en.is_object()) {
      for (const base::Member* m : base::js_members(en.as_object())) {  // Object.values: JS order
        queue.push_back(hex_key(m->value));
      }
    }
    if (const auto ki = kids.find(k); ki != kids.end()) {
      for (const Value* c : ki->second) {
        queue.push_back(hex_key(g(*c, "handle")));
      }
    }
  }
  Array out;
  for (const std::string& k : keep) {
    out.push_back(*by_h[k]);
  }
  return out;
}

}  // namespace

Value dyn_block_from_dwg(const Value& b, const DynHooks& hooks) {
  if (!b.truthy() || (!g(b, "dynamic").truthy() && !g(b, "dynamicOf").truthy())) {
    return {};
  }
  Array all;
  Array h;
  Array shown;
  Array attdefs;
  Array ad;
  if (const Value& es = at(b, "entities"); es.is_array()) {
    std::size_t n = 0;
    for (const Value& e : es.as_array()) {
      if (n++ >= 100000) {
        break;
      }
      h.emplace_back(hex_key(g(e, "handle")));
      shown.emplace_back(!g(e, "invisible").truthy());
      if (g(e, "type") == Value("ATTDEF")) {
        const Value d = hooks.attdef_of(e);
        if (d.truthy()) {
          attdefs.push_back(d);
          ad.emplace_back(static_cast<double>(all.size()));
        }
        all.emplace_back(Array{});
        continue;
      }
      Value copy = e;
      copy.set("invisible", false);
      const Value c = hooks.convert(copy);
      Array parts;
      if (c.is_array()) {
        for (const Value& x : c.as_array()) {
          if (x.truthy()) {
            parts.push_back(x);
          }
        }
      } else if (c.truthy()) {
        parts.push_back(c);
      }
      all.emplace_back(std::move(parts));
    }
  }
  Value dyn = base::make_object(
      {{"src", hex_key(g(b, "record"))}, {"h", h}, {"shown", shown}, {"all", all}, {"ad", std::move(ad)}});
  if (hooks.record != nullptr) {
    if (const Value x = g(*hooks.record, "extDict"); x.truthy()) {
      dyn.set("x", hex_key(x));
    }
    if (const Value xd = g(*hooks.record, "xdata"); xd.truthy() && (xd.is_object() || xd.is_array())) {
      dyn.set("xd", xd);
    }
  }
  if (const Value& dynamic = at(b, "dynamic"); dynamic.truthy()) {
    dyn.set("def", true);
    const Value& ps = at(dynamic, "params");
    Array params;
    if (ps.is_array()) {
      for (const Value& p : ps.as_array()) {
        params.push_back(
            base::make_object({{"node", g(p, "node")}, {"kind", g(p, "kind")}, {"name", clean_name(g(p, "name"))}}));
      }
    }
    dyn.set("params", std::move(params));
    const Value* v = nullptr;
    if (ps.is_array()) {
      for (const Value& p : ps.as_array()) {
        if (g(p, "kind") == Value("visibility") && g(p, "states").is_array()) {
          v = &p;
          break;
        }
      }
    }
    if (v != nullptr) {
      std::unordered_map<std::string, double> atmap;  // new Map(h.map((x, i) => [x, i])): the last index wins
      for (std::size_t i = 0; i < h.size(); ++i) {
        atmap[h[i].as_string()] = static_cast<double>(i);
      }
      Value vis = base::make_object(
          {{"node", g(*v, "node")}, {"name", clean_name(g(*v, "name"))}, {"label", clean_name(g(*v, "label"))}});
      if (const Value hd = g(*v, "handle"); hd.truthy()) {
        vis.set("param", hex_key(hd));
      }
      const Value pt = g(*v, "point");
      vis.set("point", pt.truthy() && finite(g(pt, "x")) && finite(g(pt, "y"))
                           ? base::pt2(g(pt, "x").as_number(), g(pt, "y").as_number())
                           : Value(nullptr));
      Array states;
      std::size_t si = 0;
      for (const Value& st : at(*v, "states").as_array()) {
        if (si++ >= 1000) {
          break;
        }
        Array show;
        if (const Value& ents = at(st, "entities"); ents.is_array()) {
          for (const Value& x : ents.as_array()) {
            if (const auto f = atmap.find(hex_key(x)); f != atmap.end()) {
              show.emplace_back(f->second);
            }
          }
        }
        states.push_back(base::make_object({{"name", clean_name(g(st, "name"))}, {"show", std::move(show)}}));
      }
      vis.set("states", std::move(states));
      dyn.set("vis", std::move(vis));
    }
  }
  if (g(b, "dynamicOf").truthy() && hooks.def_name.truthy()) {
    dyn.set("of", hooks.def_name);
  }
  Array shown_entities;
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (shown[i].as_bool()) {
      for (const Value& x : all[i].as_array()) {
        shown_entities.push_back(x);
      }
    }
  }
  return base::make_object(
      {{"entities", std::move(shown_entities)}, {"attdefs", std::move(attdefs)}, {"dyn", std::move(dyn)}});
}

Value dyn_ref_from_dwg(const Value& e) {
  const Value& d = at(e, "dyn");
  if (!d.truthy()) {
    return {nullptr};
  }
  Array values;
  if (const Value& vs = at(d, "values"); vs.is_array()) {
    std::size_t n = 0;
    for (const Value& v : vs.as_array()) {
      if (n++ >= 1000) {
        break;
      }
      Value c = v;
      if (c.is_object()) {
        c.erase("data");
      }
      values.push_back(std::move(c));
    }
  }
  Value out = Value::object();
  if (const Value st = g(d, "state"); st.is_string()) {
    out.set("state", clean_name(st));
  }
  out.set("values", std::move(values));
  out.set("src", hex_key(g(e, "handle")));
  if (const Value x = g(e, "extDict"); x.truthy()) {
    out.set("x", hex_key(x));
  }
  return out;
}

Value dyn_source(const Value& db) {
  Array objects = dyn_objects(db);
  std::vector<double> nums;
  for (const Value& o : objects) {
    const Value n = g(o, "typeNum");
    if (finite(n) && std::trunc(n.as_number()) == n.as_number() && n.as_number() >= 500) {
      nums.push_back(n.as_number());
    }
  }
  Array classes;
  if (const Value& cs = at(db, "classes"); cs.is_array()) {
    for (const Value& c : cs.as_array()) {
      const Value n = g(c, "num");
      const bool has = n.is_number() && std::ranges::find(nums, n.as_number()) != nums.end();
      const Value dx = g(c, "dxfName");
      if (has || dyn_class(dx.truthy() ? str(dx) : std::string())) {
        classes.push_back(c);
      }
    }
  }
  Array records;
  if (const Value& blocks = at(db, "blocks"); blocks.is_array()) {
    for (const Value& b : blocks.as_array()) {
      if (g(b, "dynamic").truthy() || g(b, "dynamicOf").truthy()) {
        records.emplace_back(hex_key(g(b, "record")));
      }
    }
  }
  const Value ver = g(db, "version");
  Value out = base::make_object({{"version", ver.truthy() ? str(ver) : std::string()}});
  if (const Value m = g(db, "maintenance"); finite(m)) {
    out.set("maintenance", m);
  }
  out.set("objects", std::move(objects));
  out.set("classes", std::move(classes));
  out.set("records", std::move(records));
  return out;
}

}  // namespace cc::cad::detail
