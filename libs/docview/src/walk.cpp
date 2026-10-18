// SPDX-License-Identifier: Apache-2.0
// walk.js (CadCraft 0.5) in C++, with what it calls: doc.js resolveColor, linetypes.js resolveLt / resolveLw /
// dashPrims, hatch.js hatchLines, walk.js polyBands and maskLoop — the JS's order of operations.
#include "cc/docview/walk.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <utility>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/cad/dims.hpp"
#include "cc/cad/save_parts.hpp"
#include "cc/doc/linetypes.hpp"

namespace cc::docview {

using base::Value;
using cad::Mat2;
using cad::Prim;

namespace {

constexpr double kTau = std::numbers::pi * 2;
constexpr std::size_t kMaxDashes = 20000;  // linetypes.js MAX_DASHES
constexpr double kMaxLines = 6000;         // hatch.js MAX_LINES
constexpr std::size_t kMaxSegs = 60000;    // hatch.js MAX_SEGS

double num(const Value& v) {
  return base::js_to_number(v);
}
bool finite(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number());
}
bool nullish(const Value& v) {
  return v.is_undefined() || v.is_null();
}
std::string str(const Value& v) {
  return base::js_string(v);
}
/// o[k] in place for a plain object (undefined otherwise)
const Value& at(const Value& o, std::string_view k) {
  static const Value kUndefined;
  if (!o.is_object() || o.is_point()) {
    return kUndefined;
  }
  const Value* v = o.find(k);
  return v != nullptr ? *v : kUndefined;
}
const base::Array& arr(const Value& v) {
  static const base::Array kEmpty;
  return v.is_array() ? v.as_array() : kEmpty;
}
const base::Array& arr(const Value&& v) = delete;

Mat2 m_mul(const Mat2& m, const Mat2& n) {
  return {m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1],        m[0] * n[2] + m[2] * n[3],
          m[1] * n[2] + m[3] * n[3], m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]};
}
cad::P2 m_apply(const Mat2& m, double x, double y) {
  return {m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]};
}
std::vector<Prim> tx(const std::vector<Prim>& ps, const std::optional<Mat2>& m) {
  return m ? cad::tx_prims(ps, *m) : ps;
}
std::vector<std::vector<double>> tx_loops(const Value& loops, const std::optional<Mat2>& m) {
  std::vector<std::vector<double>> out;
  for (const Value& l : arr(loops)) {
    std::vector<double> o;
    const base::Array& f = arr(l);
    o.reserve(f.size());
    if (!m) {
      for (const Value& v : f) {
        o.push_back(num(v));
      }
    } else {
      for (std::size_t i = 0; i + 1 < f.size(); i += 2) {
        const cad::P2 p = m_apply(*m, num(f[i]), num(f[i + 1]));
        o.push_back(p.x);
        o.push_back(p.y);
      }
    }
    out.push_back(std::move(o));
  }
  return out;
}

double sweep(const Prim& q) {
  double d = q.a1 - q.a0;
  for (int guard = 0; d <= 0 && guard < 64; ++guard) {
    d += 2 * std::numbers::pi;
  }
  return d;
}
double prim_len(const Prim& q) {
  return !q.arc ? base::js_hypot(q.x2 - q.x1, q.y2 - q.y1) : q.r * std::abs(sweep(q));
}
/// The part [f0, f1] (fractions of its length) of one primitive.
Prim piece(const Prim& q, double f0, double f1) {
  if (!q.arc) {
    Prim p;
    p.x1 = q.x1 + (q.x2 - q.x1) * f0;
    p.y1 = q.y1 + (q.y2 - q.y1) * f0;
    p.x2 = q.x1 + (q.x2 - q.x1) * f1;
    p.y2 = q.y1 + (q.y2 - q.y1) * f1;
    return p;
  }
  const double sw = sweep(q);
  Prim p = q;
  if (q.rev) {
    p.a0 = q.a1 - sw * f1;
    p.a1 = q.a1 - sw * f0;
    return p;
  }
  p.a0 = q.a0 + sw * f0;
  p.a1 = q.a0 + sw * f1;
  return p;
}

/// walk.js maskLoop(e, scale): a text's box grown by (scale − 1) × its height, as a closed ring
std::vector<double> mask_loop(const Value& e, const Value& scale_v) {
  const cad::TextLayout lay = cad::text_layout(e);
  const double scale = finite(scale_v) ? scale_v.as_number() : 1.5;
  const double h = num(e.get("h"));
  const double m = std::max(0.0, scale - 1) * h;
  double x0 = lay.box_x;
  double x1 = lay.box_x + lay.box_w;
  const double ww = num(e.get("ww"));
  if (ww > 0 && ww > lay.box_w) {  // the paragraph's own width, placed as its alignment places it
    const Value ha = e.get("ha");
    if (ha == Value("center")) {
      const double c = (x0 + x1) / 2;
      x0 = c - ww / 2;
      x1 = c + ww / 2;
    } else if (ha == Value("right")) {
      x0 = x1 - ww;
    } else {
      x1 = x0 + ww;
    }
  }
  const Value rv = e.get("rot");
  const double rot = rv.truthy() ? num(rv) : 0;
  const double c = base::js_cos(rot);
  const double s = base::js_sin(rot);
  const double ex = num(e.get("x"));
  const double ey = num(e.get("y"));
  const auto P = [&](double x, double y) { return std::pair{ex + x * c - y * s, ey + x * s + y * c}; };
  const std::array<std::pair<double, double>, 4> pts = {P(x0 - m, lay.box_y - m), P(x1 + m, lay.box_y - m),
                                                        P(x1 + m, lay.box_y + lay.box_h + m),
                                                        P(x0 - m, lay.box_y + lay.box_h + m)};
  std::vector<double> out;
  for (const auto& [x, y] : pts) {
    out.push_back(x);
    out.push_back(y);
  }
  out.push_back(pts[0].first);
  out.push_back(pts[0].second);
  return out;
}

bool positive(double v) {
  return v > 0;
}

struct Bands {
  std::vector<std::vector<double>> loops;
  std::vector<Prim> plain;
};
/// walk.js polyBands(e): a polyline's widths as filled bands, or nothing when it has no width
std::optional<Bands> poly_bands(const Value& e) {
  const base::Array& pts = arr(at(e, "pts"));
  const Value wv = e.get("w");
  const double g = finite(wv) && wv.as_number() > 0 ? wv.as_number() : 0;
  bool any = g != 0;
  for (const Value& p : pts) {
    any = any || num(p.get("sw")) > 0 || num(p.get("ew")) > 0;
  }
  if (!any) {
    return std::nullopt;
  }
  const std::size_t n = pts.size();
  std::size_t segs = n;
  if (!e.get("closed").truthy()) {
    segs = n == 0 ? 0 : n - 1;
  }
  Bands out;
  for (std::size_t i = 0; i < segs; ++i) {
    const Value& a = pts[i];
    const Value& q = pts[(i + 1) % n];
    const Value sw = a.get("sw");
    const Value ew = a.get("ew");
    const double ws = finite(sw) ? sw.as_number() : g;
    const double we = finite(ew) ? ew.as_number() : g;
    const Value ab = a.get("b");
    const double b = ab.truthy() ? num(ab) : 0;
    const Value seg =
        base::make_object({{"type", "poly"},
                           {"closed", false},
                           {"pts", base::Array{base::make_object({{"x", a.get("x")}, {"y", a.get("y")}, {"b", b}}),
                                               base::make_object({{"x", q.get("x")}, {"y", q.get("y")}})}}});
    const std::vector<Prim> one = cad::entity_prims(seg, nullptr);
    if (!positive(ws) && !positive(we)) {
      out.plain.insert(out.plain.end(), one.begin(), one.end());
      continue;
    }
    if (one.empty()) {
      continue;
    }
    const Prim& p = one[0];
    struct Along {
      double x, y, nx, ny, t;
    };
    std::vector<Along> along;
    if (!p.arc) {
      double L = base::js_hypot(p.x2 - p.x1, p.y2 - p.y1);
      if (L == 0 || std::isnan(L)) {
        L = 1;
      }
      const double nx = -(p.y2 - p.y1) / L;
      const double ny = (p.x2 - p.x1) / L;
      along.push_back({p.x1, p.y1, nx, ny, 0});
      along.push_back({p.x2, p.y2, nx, ny, 1});
    } else {
      // an A primitive runs counter-clockwise from a0 to a1; the polyline may run either way
      double sw2 = std::fmod(p.a1 - p.a0 + kTau, kTau);
      if (sw2 == 0 || std::isnan(sw2)) {
        sw2 = kTau;
      }
      const bool ccw = b > 0;
      const double kk = std::max(2.0, std::ceil(sw2 / (std::numbers::pi / 18)));
      const auto steps = static_cast<std::int64_t>(std::min(kk, 1e5));
      for (std::int64_t j = 0; j <= steps; ++j) {
        const double t = static_cast<double>(j) / kk;
        const double ang = ccw ? p.a0 + sw2 * t : p.a1 - sw2 * t;
        const double cx = base::js_cos(ang);
        const double cy = base::js_sin(ang);
        along.push_back({p.cx + p.r * cx, p.cy + p.r * cy, ccw ? -cx : cx, ccw ? -cy : cy, t});
      }
    }
    std::vector<double> left;
    std::vector<double> right;
    for (const Along& v : along) {
      const double h = (ws + (we - ws) * v.t) / 2;
      left.push_back(v.x + v.nx * h);
      left.push_back(v.y + v.ny * h);
      right.insert(right.begin(), {v.x - v.nx * h, v.y - v.ny * h});
    }
    std::vector<double> ring = left;
    ring.insert(ring.end(), right.begin(), right.end());
    const double r0 = ring[0];
    const double r1 = ring[1];
    ring.push_back(r0);
    ring.push_back(r1);
    out.loops.push_back(std::move(ring));
  }
  return out;
}

class Walker {
 public:
  Walker(const doc::Doc& d, const Out& o, const WalkOptions& opts) : doc_(d), out_(o), opts_(opts) {
    const Value lts = d.meta.get("ltscale");
    ltscale_ = num(lts) > 0 ? num(lts) : 1;
  }

  void run(const std::vector<const Value*>& entities) {
    for (const Value* ep : entities) {
      const Value& e = *ep;
      if (e.get("type") == Value("viewport")) {
        // the view shows whatever its layer; the border follows the layer like anything else
        if (out_.viewport) {
          out_.viewport(e);
        }
        if (shown(e.get("layer"))) {
          stroke(resolve_color(doc_, e, Value(), Value()), tx(cad::entity_prims(e, nullptr), opts_.m),
                 resolve_lw(doc_, e, Value(), Value()), opts_of(tr_of(e, Value(), Value())));
        }
        continue;
      }
      if (shown(e.get("layer"))) {
        emit(e, Value(nullptr), Value(nullptr), opts_.m, 0, {});
      }
    }
  }

 private:
  // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) — a walker lives for one walk() call
  const doc::Doc& doc_;
  const Out& out_;
  const WalkOptions& opts_;
  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  double ltscale_ = 1;
  std::map<std::string, Value> patterns_;

  struct Blk {
    Value lt, lw, tr;
  };

  void stroke(const std::string& color, const std::vector<Prim>& ps, double lw, const Opts& o) const {
    if (out_.stroke) {
      out_.stroke(color, ps, lw, o);
    }
  }
  void fill(const std::string& color, const std::vector<std::vector<double>>& loops, bool solid, const Opts& o) const {
    if (out_.fill) {
      out_.fill(color, loops, solid, o);
    }
  }

  [[nodiscard]] bool shown(const Value& name) const {
    const std::string n = str(name);
    if (!opts_.frozen.empty() && opts_.frozen.contains(base::js_to_upper(n))) {
      return false;
    }
    const Value* l = doc_.layers().get(n);
    return l == nullptr ||
           (l->get("on").truthy() && !l->get("frozen").truthy() && (!opts_.plot || !(l->get("plot") == Value(false))));
  }
  const Value& pattern(const Value& name) {
    const std::string k = str(name);
    auto it = patterns_.find(k);
    if (it == patterns_.end()) {
      it = patterns_.emplace(k, doc::pattern_of(doc_, k)).first;
    }
    return it->second;
  }
  /// transparency (P.5): the object's, else its layer's (ByLayer), else the reference's (ByBlock)
  [[nodiscard]] Value tr_of(const Value& e, const Value& blk_tr, const Value& parent_layer) const {
    Value v = e.get("tr");
    if (v == Value("ByBlock")) {
      return blk_tr;
    }
    if (finite(v)) {
      return v;
    }
    const Value el = e.get("layer");
    const std::string ln = el == Value("0") && parent_layer.truthy() ? str(parent_layer) : str(el);
    const Value* l = doc_.layers().get(ln);
    const Value ltr = l != nullptr ? l->get("tr") : Value();
    return finite(ltr) ? ltr : Value();
  }
  static Opts opts_of(const Value& tr) {
    Opts o;
    if (finite(tr) && tr.as_number() > 0) {
      o.alpha = std::max(0.0, std::min(1.0, 1 - tr.as_number() / 100));
    }
    return o;
  }

  // lines in the object's own coordinates, dashed by its linetype there (so a scaled block scales its dashes), then
  // placed
  void stroke_of(const Value& e, const std::vector<Prim>& ps, const std::string& color, double lw,
                 const std::optional<Mat2>& mm, const Value& ctx_lt, const Value& ctx_layer, const Opts& o) {
    const Value& pat = pattern(resolve_lt(doc_, e, ctx_lt, ctx_layer));
    std::vector<Prim> d;
    if (pat.truthy()) {
      std::vector<double> p;
      for (const Value& v : arr(pat)) {
        p.push_back(num(v));
      }
      const Value lts = e.get("lts");
      d = dash_prims(ps, p, ltscale_ * (num(lts) > 0 ? num(lts) : 1));
    } else {
      d = ps;
    }
    stroke(color, tx(d, mm), lw, o);
  }

  // NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity) — depth-capped (17 levels), as walk.js
  void emit(const Value& e, const Value& by_block, const Value& parent_layer, const std::optional<Mat2>& mm, int depth,
            const Blk& blk) {
    if (depth > 16) {
      return;
    }
    const std::string color = resolve_color(doc_, e, by_block, parent_layer);
    const double lw = resolve_lw(doc_, e, blk.lw, parent_layer);
    const Value tr = tr_of(e, blk.tr, parent_layer);
    const Opts o = opts_of(tr);
    const std::string type = str(e.get("type"));
    if (type == "insert" || type == "dim" || type == "mleader") {
      base::Array parts_own;
      const base::Array* parts = &parts_own;
      std::optional<Mat2> m2 = mm;
      if (type == "insert") {
        const Value* b = doc_.blocks().get(str(e.get("block")));
        if (b == nullptr) {
          return;
        }
        // the block, then its attributes (visible ones with a value; same layer and colour rules)
        if (!arr(at(*b, "attdefs")).empty() || e.get("attrs").truthy()) {
          parts_own = arr(at(*b, "entities"));
          for (Value& t : cad::attr_texts(e, *b)) {
            parts_own.push_back(std::move(t));
          }
        } else {
          parts = &arr(at(*b, "entities"));
        }
        const Mat2 im = cad::insert_matrix(e, *b);
        m2 = mm ? m_mul(*mm, im) : im;
      } else if (type == "mleader") {
        parts_own = cad::leader_geometry(e);
      } else {
        // dimParts: the imported block while it has one, else the dimension's own geometry
        const Value bn = e.get("block");
        const Value* b = bn.truthy() ? doc_.blocks().get(str(bn)) : nullptr;
        if (b != nullptr) {
          parts = &arr(at(*b, "entities"));
        } else {
          parts_own = cad::dim_geometry(e);
        }
      }
      const Value el = e.get("layer");
      const Value layer = el == Value("0") && parent_layer.truthy() ? parent_layer : el;
      const Blk inner{
          .lt = resolve_lt(doc_, e, blk.lt, parent_layer), .lw = lw, .tr = tr};  // what ByBlock means inside
      const Blk solid{.lt = Value("Continuous"), .lw = lw, .tr = tr};  // dimension and leader lines stay solid
      const Value col(color);
      for (const Value& c : *parts) {
        const Value cl = c.get("layer");
        if (!shown(cl == Value("0") ? layer : cl)) {
          continue;
        }
        emit(c, col, layer, m2, depth + 1, type == "insert" ? inner : solid);
      }
      return;
    }
    if (type == "text") {
      // e.picName: a picture's name stands in for it until its file is loaded (pictures are not loaded here)
      const Value te = mm ? cad::tx_entity(e, *mm) : e;
      if (const Value bgm = e.get("bgm"); bgm.truthy()) {  // a background mask (0.5 WL.8): before the text
        const bool win = bgm.get("color") == Value("bg");
        const std::vector<double> loop = mask_loop(te, bgm.get("scale"));
        std::string mc = "ink";
        if (!win) {
          Value ce = e;
          ce.set("color", bgm.get("color"));
          mc = resolve_color(doc_, ce, by_block, parent_layer);
        }
        if (out_.mask) {
          out_.mask(mc, loop, win);
        } else {
          Opts wo;
          wo.wipe = win;
          fill(mc, {loop}, true, wo);
        }
      }
      if (out_.text) {
        out_.text(color, te, o);
      }
      return;
    }
    if (type == "point") {
      const double x = num(e.get("x"));
      const double y = num(e.get("y"));
      const cad::P2 p = mm ? m_apply(*mm, x, y) : cad::P2{x, y};
      if (out_.point) {
        out_.point(color, p.x, p.y);
      }
      return;
    }
    if (type == "hatch") {
      const bool solid = e.get("solid").truthy();
      if (const Value bg = e.get("bg"); !nullish(bg) && !solid) {  // background colour under the pattern
        Value ce = e;
        ce.set("color", bg);
        fill(resolve_color(doc_, ce, by_block, parent_layer), tx_loops(at(e, "loops"), mm), true, o);
      }
      if (const auto lines = hatch_lines(e, doc_)) {  // a known pattern draws its lines; others show as a tone
        stroke(color, tx(*lines, mm), lw, o);
        return;
      }
      Opts fo = o;
      if (e.get("wipe").truthy() && solid && e.get("color") == Value("bg")) {
        fo.wipe = true;  // DIMTFILL 1: the background; 2: its own colour
      }
      fill(color, tx_loops(at(e, "loops"), mm), solid, fo);
      return;
    }
    if (type == "ellipse") {
      const std::vector<cad::P2> pts = cad::ellipse_points(e, 96);
      std::vector<Prim> ps;
      for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        Prim q;
        q.x1 = pts[i].x;
        q.y1 = pts[i].y;
        q.x2 = pts[i + 1].x;
        q.y2 = pts[i + 1].y;
        ps.push_back(q);
      }
      stroke_of(e, ps, color, lw, mm, blk.lt, parent_layer, o);
      return;
    }
    if (type == "poly") {
      const auto b = poly_bands(e);
      if (!b) {
        stroke_of(e, cad::entity_prims(e, nullptr), color, lw, mm, blk.lt, parent_layer, o);
        return;
      }
      if (!b->loops.empty()) {
        std::vector<std::vector<double>> loops;
        for (const auto& l : b->loops) {
          if (!mm) {
            loops.push_back(l);
            continue;
          }
          std::vector<double> t;
          for (std::size_t i = 0; i + 1 < l.size(); i += 2) {
            const cad::P2 p = m_apply(*mm, l[i], l[i + 1]);
            t.push_back(p.x);
            t.push_back(p.y);
          }
          loops.push_back(std::move(t));
        }
        fill(color, loops, true, o);
      }
      if (!b->plain.empty()) {
        stroke_of(e, b->plain, color, lw, mm, blk.lt, parent_layer, o);
      }
      return;
    }
    if ((type == "xline" || type == "ray") && out_.infinite) {  // construction lines (0.5 DR.7): clipped by the output
      const double x = num(e.get("x"));
      const double y = num(e.get("y"));
      const double dx = num(e.get("dx"));
      const double dy = num(e.get("dy"));
      const cad::P2 p = mm ? m_apply(*mm, x, y) : cad::P2{x, y};
      const double ddx = mm ? (*mm)[0] * dx + (*mm)[2] * dy : dx;
      const double ddy = mm ? (*mm)[1] * dx + (*mm)[3] * dy : dy;
      out_.infinite(color, base::make_object({{"type", type}, {"x", p.x}, {"y", p.y}, {"dx", ddx}, {"dy", ddy}}), lw,
                    o);
      return;
    }
    stroke_of(e, cad::entity_prims(e, nullptr), color, lw, mm, blk.lt, parent_layer, o);
  }
};

}  // namespace

std::string resolve_color(const doc::Doc& doc, const Value& e, const Value& by_block, const Value& parent_layer) {
  Value c = e.get("color");
  if (c == Value(0)) {
    c = by_block;
  }
  if (nullish(c) || c == Value(256)) {
    const Value el = e.get("layer");
    const std::string ln = el == Value("0") && parent_layer.truthy() ? str(parent_layer) : str(el);
    const Value* l = doc.layers().get(ln);
    const Value lc = l != nullptr ? l->get("color") : Value();
    c = nullish(lc) ? Value(7) : lc;
  }
  return c.is_string() ? c.as_string() : cad::aci_hex(num(c));
}

namespace {
bool is_by_layer(const Value& n) {
  return nullish(n) || (n.is_string() && base::js_to_lower(n.as_string()) == "bylayer");
}
bool is_by_block(const Value& n) {
  return n.is_string() && base::js_to_lower(n.as_string()) == "byblock";
}
}  // namespace

Value resolve_lt(const doc::Doc& doc, const Value& e, const Value& by_block, const Value& parent_layer) {
  Value n = e.get("lt");
  if (is_by_block(n)) {
    n = nullish(by_block) ? Value("Continuous") : by_block;
  }
  if (is_by_layer(n)) {
    const Value el = e.get("layer");
    const std::string ln = el == Value("0") && parent_layer.truthy() ? str(parent_layer) : str(el);
    const Value* l = doc.layers().get(ln);
    const Value lt = l != nullptr ? l->get("linetype") : Value();
    n = lt.truthy() ? lt : Value("Continuous");
  }
  return n;
}

double resolve_lw(const doc::Doc& doc, const Value& e, const Value& by_block, const Value& parent_layer) {
  Value w = e.get("lw");
  if (w == Value(-2)) {
    w = nullish(by_block) ? Value(-3) : by_block;
  }
  if (nullish(w) || w == Value(-1)) {
    const Value el = e.get("layer");
    const std::string ln = el == Value("0") && parent_layer.truthy() ? str(parent_layer) : str(el);
    const Value* l = doc.layers().get(ln);
    const Value lw = l != nullptr ? l->get("lineweight") : Value();
    w = nullish(lw) ? Value(-3) : lw;
  }
  return finite(w) ? w.as_number() : -3;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the JS's loop
std::vector<Prim> dash_prims(const std::vector<Prim>& prims, const std::vector<double>& pattern, double scale) {
  if (pattern.empty() || !(scale > 0)) {
    return prims;
  }
  std::vector<double> p;
  p.reserve(pattern.size());
  double period = 0;
  for (const double v : pattern) {
    p.push_back(v * scale);
    period += std::abs(v * scale);
  }
  if (!(period > 1e-12)) {
    return prims;
  }
  double total = 0;
  for (const Prim& q : prims) {
    total += prim_len(q);
  }
  if (total / period * static_cast<double>(p.size()) > static_cast<double>(kMaxDashes)) {
    return prims;  // too fine to see: drawn continuous
  }
  const double dot = period * 0.01;
  std::vector<Prim> out;
  std::size_t i = 0;
  double left = std::abs(p[0]);  // the pattern runs on along a polyline's pieces
  for (const Prim& q : prims) {
    const double len = prim_len(q);
    double s = 0;
    for (std::size_t guard = 0; s < len - 1e-12 && guard < 4 * kMaxDashes + 16; ++guard) {
      const double v = p[i];
      const double step = std::min(left, len - s);
      if (v > 0) {
        out.push_back(piece(q, s / len, (s + step) / len));
      } else if (v == 0 && left == std::abs(v)) {
        out.push_back(piece(q, s / len, std::min(len, s + dot) / len));
      }
      s += step;
      left -= step;
      if (left <= 1e-12) {
        i = (i + 1) % p.size();
        left = std::abs(p[i]);
        if (left == 0) {
          if (p[i] == 0 && s < len - 1e-12) {
            out.push_back(piece(q, s / len, std::min(len, s + dot) / len));
          }
          i = (i + 1) % p.size();
          left = std::abs(p[i]);
        }
      }
    }
  }
  return out;
}

namespace {

struct Family {
  double angle = 0, x = 0, y = 0, dx = 0, dy = 0;
  std::vector<double> dashes;
};

Prim seg(double ox, double oy, double ux, double uy, double t0, double t1) {
  Prim q;
  q.x1 = ox + ux * t0;
  q.y1 = oy + uy * t0;
  q.x2 = ox + ux * t1;
  q.y2 = oy + uy * t1;
  return q;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — hatch.js clipFamilies, in its order
std::optional<std::vector<Prim>> clip_families(const std::vector<Family>& fams, const Value& loops) {
  struct Edge {
    double x1, y1, x2, y2;
  };
  std::vector<Edge> edges;
  for (const Value& l : arr(loops)) {
    const base::Array& f = arr(l);
    const std::size_t n = f.size() / 2;
    if (n < 3) {
      continue;
    }
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t j = (i + 1) % n;
      const double x1 = num(f[2 * i]);
      const double y1 = num(f[2 * i + 1]);
      const double x2 = num(f[2 * j]);
      const double y2 = num(f[2 * j + 1]);
      if (x1 != x2 || y1 != y2) {
        edges.push_back({x1, y1, x2, y2});
      }
    }
  }
  if (edges.empty()) {
    return std::nullopt;
  }
  std::vector<Prim> out;
  double line_count = 0;
  for (const Family& f : fams) {
    const double a = f.angle * std::numbers::pi / 180;
    const double ux = base::js_cos(a);
    const double uy = base::js_sin(a);
    const double nx = -uy;
    const double ny = ux;
    const double dy = f.dy;
    if (!(std::abs(dy) > 1e-12)) {
      continue;
    }
    // the loops' extent across the lines (along n) and along them (along u), from the family's origin
    double n_min = std::numeric_limits<double>::infinity();
    double n_max = -std::numeric_limits<double>::infinity();
    for (const Edge& ed : edges) {
      for (const auto& [x, y] : {std::pair{ed.x1, ed.y1}, std::pair{ed.x2, ed.y2}}) {
        const double v = (x - f.x) * nx + (y - f.y) * ny;
        n_min = std::min(n_min, v);
        n_max = std::max(n_max, v);
      }
    }
    const double k0 = std::ceil(std::min(n_min / dy, n_max / dy) - 1e-9);
    const double k1 = std::floor(std::max(n_min / dy, n_max / dy) + 1e-9);
    line_count += std::max(0.0, k1 - k0 + 1);
    if (line_count > kMaxLines) {
      return std::nullopt;
    }
    double period = 0;
    for (const double d : f.dashes) {
      period += std::abs(d);
    }
    const auto lines = static_cast<std::int64_t>(std::max(0.0, k1 - k0 + 1));
    for (std::int64_t kk = 0; kk < lines; ++kk) {
      const double k = k0 + static_cast<double>(kk);
      // the line through O + k (dx u + dy n), direction u
      const double ox = f.x + k * (f.dx * ux + dy * nx);
      const double oy = f.y + k * (f.dx * uy + dy * ny);
      std::vector<double> ts;
      for (const Edge& ed : edges) {
        const double d1 = (ed.x1 - ox) * nx + (ed.y1 - oy) * ny;
        const double d2 = (ed.x2 - ox) * nx + (ed.y2 - oy) * ny;
        if ((d1 > 0) == (d2 > 0)) {
          continue;  // half-open: a vertex on the line counts once
        }
        const double tt = d1 / (d1 - d2);
        const double px = ed.x1 + (ed.x2 - ed.x1) * tt;
        const double py = ed.y1 + (ed.y2 - ed.y1) * tt;
        ts.push_back((px - ox) * ux + (py - oy) * uy);
      }
      if (ts.size() < 2) {
        continue;
      }
      std::ranges::sort(ts);
      for (std::size_t i = 0; i + 1 < ts.size(); i += 2) {
        const double t0 = ts[i];
        const double t1 = ts[i + 1];
        if (t1 - t0 < 1e-12) {
          continue;
        }
        if (f.dashes.empty() || !(period > 1e-12)) {
          out.push_back(seg(ox, oy, ux, uy, t0, t1));
          continue;
        }
        // dashes repeat from the line's origin: walk the periods that overlap [t0, t1]
        double start = std::floor(t0 / period) * period;
        for (std::int64_t guard = 0; guard < 10000000; ++guard) {
          if (!(start < t1)) {
            break;
          }
          double p = start;
          for (const double d : f.dashes) {
            const double len = std::abs(d);
            if (d >= 0) {
              const double a0 = std::max(p, t0);
              const double a1 = std::min(p + (d == 0 ? 0 : len), t1);
              if (d == 0 ? (p >= t0 && p <= t1) : a1 > a0) {
                out.push_back(d == 0 ? seg(ox, oy, ux, uy, p - period * 0.02, p + period * 0.02)
                                     : seg(ox, oy, ux, uy, a0, a1));
              }
            }
            p += len;
          }
          if (out.size() > kMaxSegs) {
            return std::nullopt;
          }
          start += period;
        }
      }
      if (out.size() > kMaxSegs) {
        return std::nullopt;
      }
    }
  }
  return out;
}

}  // namespace

std::optional<std::vector<Prim>> hatch_lines(const Value& e, const doc::Doc& doc) {
  if (e.get("solid").truthy()) {
    return std::nullopt;
  }
  std::vector<Family> fams;
  bool have = false;
  if (const base::Array& pl = arr(at(e, "patlines")); !pl.empty()) {
    for (const Value& f : pl) {
      Family x{.angle = num(f.get("angle")),
               .x = num(f.get("x")),
               .y = num(f.get("y")),
               .dx = num(f.get("dx")),
               .dy = num(f.get("dy")),
               .dashes = {}};
      for (const Value& d : arr(at(f, "dashes"))) {
        x.dashes.push_back(num(d));
      }
      fams.push_back(std::move(x));
    }
    have = true;
  } else if (const Value* def = cad::hatch_pattern(base::js_to_upper(str(e.get("pattern"))))) {
    const Value ps = e.get("pscale");
    const double k = (num(ps) > 0 ? num(ps) : 1) / (cad::hatch_is_metric(e, doc.meta) ? 1 : 25.4);
    const Value pa = e.get("pangle");
    const double turn = pa.truthy() ? num(pa) : 0;
    const double c = base::js_cos(turn);
    const double s = base::js_sin(turn);
    for (const Value& f : arr(at(*def, "lines"))) {
      const double fx = num(f.get("x"));
      const double fy = num(f.get("y"));
      Family x{.angle = num(f.get("angle")) + turn * 180 / std::numbers::pi,
               .x = (fx * c - fy * s) * k,
               .y = (fx * s + fy * c) * k,
               .dx = num(f.get("dx")) * k,
               .dy = num(f.get("dy")) * k,
               .dashes = {}};
      for (const Value& d : arr(at(f, "dashes"))) {
        x.dashes.push_back(num(d) * k);
      }
      fams.push_back(std::move(x));
    }
    have = true;
  }
  if (!have) {
    return std::nullopt;
  }
  // the pattern origin (Properties › Origin X / Y, P.7): every family moves with it
  const Value oxv = e.get("ox");
  const Value oyv = e.get("oy");
  const double ox = finite(oxv) ? oxv.as_number() : 0;
  const double oy = finite(oyv) ? oyv.as_number() : 0;
  if (ox != 0 || oy != 0) {
    for (Family& f : fams) {
      f.x += ox;
      f.y += oy;
    }
  }
  return clip_families(fams, at(e, "loops"));
}

void walk(const doc::Doc& doc, const std::vector<const Value*>& entities, const Out& out, const WalkOptions& opts) {
  Walker w(doc, out, opts);
  w.run(entities);
}

}  // namespace cc::docview
