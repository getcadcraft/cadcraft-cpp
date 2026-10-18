// SPDX-License-Identifier: Apache-2.0
// A CadCraft document as the renderer's draw lists: walk() per space (the model, then each sheet), its strokes
// joined into polylines (arcs in short steps), its fills as rings, each text line as one text at its baseline (as
// view.js draws textLayout's lines), batches by colour and lineweight (walk already left hidden layers out).
#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/cad/geom.hpp"
#include "cc/cad/save_parts.hpp"
#include "cc/docview/drawing.hpp"
#include "cc/docview/walk.hpp"

namespace cc::docview {

using base::Value;

namespace {

constexpr double kTau = std::numbers::pi * 2;

double num(const Value& v) {
  return base::js_to_number(v);
}

void grow(drawing::Bounds& b, double x, double y) {
  if (!std::isfinite(x) || !std::isfinite(y)) {
    return;
  }
  b.min_x = std::min(b.min_x, x);
  b.min_y = std::min(b.min_y, y);
  b.max_x = std::max(b.max_x, x);
  b.max_y = std::max(b.max_y, y);
}

class SpaceBuilder {
 public:
  SpaceBuilder(drawing::Space& sp, double arc_steps) : sp_(sp), arc_steps_(arc_steps) {}

  drawing::Batch& batch(const std::string& color, double lw) {
    const std::string key = color + "|" + std::to_string(lw);
    const auto it = index_.find(key);
    if (it != index_.end()) {
      return sp_.batches[it->second];
    }
    index_.emplace(key, sp_.batches.size());
    drawing::Batch b;
    b.color = color;
    b.lw = lw < 0 ? 0.25 : lw;
    sp_.batches.push_back(std::move(b));
    return sp_.batches.back();
  }

  void stroke(const std::string& color, const std::vector<cad::Prim>& ps, double lw) {
    drawing::Batch& b = batch(color, lw);
    std::vector<double>* line = nullptr;
    double lx = std::numeric_limits<double>::infinity();
    double ly = std::numeric_limits<double>::infinity();
    const auto point = [&](double x, double y) {
      line->push_back(x);
      line->push_back(y);
      grow(sp_.bounds, x, y);
    };
    for (const cad::Prim& q : ps) {
      if (!q.arc) {
        if (line == nullptr || std::abs(q.x1 - lx) > 1e-12 || std::abs(q.y1 - ly) > 1e-12) {
          b.lines.emplace_back();
          line = &b.lines.back();
          point(q.x1, q.y1);
        }
        point(q.x2, q.y2);
        lx = q.x2;
        ly = q.y2;
        continue;
      }
      const double a_start = q.rev ? q.a1 : q.a0;
      const double a_end = q.rev ? q.a0 : q.a1;
      const double sweep = q.a1 - q.a0;
      const double sx = q.cx + q.r * base::js_cos(a_start);
      const double sy = q.cy + q.r * base::js_sin(a_start);
      if (line == nullptr || std::abs(sx - lx) > 1e-9 || std::abs(sy - ly) > 1e-9) {
        b.lines.emplace_back();
        line = &b.lines.back();
        point(sx, sy);
      }
      const double n = std::clamp(std::ceil(std::abs(sweep) / kTau * arc_steps_), 1.0, arc_steps_);
      const auto steps = static_cast<int>(n);
      for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / n;
        const double a = q.rev ? a_start - sweep * t : a_start + sweep * t;
        point(q.cx + q.r * base::js_cos(a), q.cy + q.r * base::js_sin(a));
      }
      lx = q.cx + q.r * base::js_cos(a_end);
      ly = q.cy + q.r * base::js_sin(a_end);
    }
  }

  void fill(const std::string& color, const std::vector<std::vector<double>>& loops, bool solid) {
    drawing::Batch& b = batch(color, -3);
    drawing::Fill f;
    f.pattern = !solid;
    for (const auto& l : loops) {
      for (std::size_t i = 0; i + 1 < l.size(); i += 2) {
        grow(sp_.bounds, l[i], l[i + 1]);
      }
      f.rings.push_back(l);
    }
    b.fills.push_back(std::move(f));
    if (!solid) {  // patterned: tone plus outline (view.js)
      drawing::Batch& o = batch(color, -3);
      for (const auto& l : loops) {
        o.lines.push_back(l);
      }
    }
  }

  void text(const std::string& color, const Value& e) {
    const cad::TextLayout lay = cad::text_layout(e);
    const Value rv = e.get("rot");
    const double rot = rv.truthy() ? num(rv) : 0;
    const double x = num(e.get("x"));
    const double y = num(e.get("y"));
    const Value ha = e.get("ha");
    double h_align = 0;
    if (ha == Value("center")) {
      h_align = 1;
    } else if (ha == Value("right")) {
      h_align = 2;
    }
    drawing::Batch& b = batch(color, -3);
    for (std::size_t i = 0; i < lay.lines.size() && i < lay.baselines.size(); ++i) {
      // the line's baseline point: (0, baseline) in the text's own frame, turned by its rotation
      const double by = lay.baselines[i];
      drawing::Text t;
      t.x = x - base::js_sin(rot) * by;
      t.y = y + base::js_cos(rot) * by;
      t.h = num(e.get("h"));
      t.rot = rot;
      t.s = lay.lines[i];
      t.ha = h_align;
      t.va = 0;
      grow(sp_.bounds, t.x, t.y);
      b.texts.push_back(std::move(t));
    }
  }

  void point(const std::string& color, double x, double y) {
    drawing::Batch& b = batch(color, -3);
    const double d = 1e-6;  // a dot
    b.lines.push_back({x - d, y, x + d, y});
    grow(sp_.bounds, x, y);
  }

 private:
  drawing::Space& sp_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) — built for one space
  double arc_steps_;
  std::map<std::string, std::size_t> index_;
};

void walk_space(const doc::Doc& d, const doc::EntityMap& map, drawing::Space& sp, const DrawingOptions& o) {
  std::vector<const Value*> ents;
  map.each([&](const doc::Id&, const Value& e) { ents.push_back(&e); });
  SpaceBuilder b(sp, o.arc_steps);
  Out out;
  out.stroke = [&](const std::string& c, const std::vector<cad::Prim>& ps, double lw, const Opts&) {
    b.stroke(c, ps, lw);
  };
  out.fill = [&](const std::string& c, const std::vector<std::vector<double>>& loops, bool solid, const Opts&) {
    b.fill(c, loops, solid);
  };
  out.text = [&](const std::string& c, const Value& e, const Opts&) { b.text(c, e); };
  out.point = [&](const std::string& c, double x, double y) { b.point(c, x, y); };
  out.viewport = [&](const Value& e) {
    drawing::Viewport vp;
    vp.cx = num(e.get("x"));
    vp.cy = num(e.get("y"));
    vp.w = num(e.get("w"));
    vp.h = num(e.get("h"));
    vp.vx = num(e.get("cx"));
    vp.vy = num(e.get("cy"));
    const double s = num(e.get("s"));
    vp.view_height = s > 0 ? vp.h / s : vp.h;
    const Value tw = e.get("tw");
    vp.twist = tw.is_number() && std::isfinite(tw.as_number()) ? tw.as_number() : 0;
    if (e.get("on") == Value(false)) {
      return;  // a viewport switched off shows nothing
    }
    sp.viewports.push_back(vp);
  };
  walk(d, ents, out);
}

}  // namespace

drawing::Drawing to_drawing(const doc::Doc& d, const DrawingOptions& o) {
  drawing::Drawing out;
  out.units = base::js_string(d.meta.get("units"));
  d.layers().each([&](const std::string& name, const Value& l) {
    drawing::Layer L;
    L.name = name;
    const Value c = l.get("color");
    L.color = c.is_string() ? c.as_string() : cad::aci_hex(c.truthy() ? num(c) : 7);
    L.on = l.get("on").truthy() && !l.get("frozen").truthy();
    const Value lw = l.get("lineweight");
    L.lw = lw.is_number() && lw.as_number() >= 0 ? lw.as_number() : 0.25;
    L.plot = !(l.get("plot") == Value(false));
    out.layers.push_back(std::move(L));
  });
  drawing::Space model;
  model.key = "model";
  model.name = "Model";
  model.kind = "model";
  walk_space(d, *d.model(), model, o);
  out.spaces.push_back(std::move(model));
  for (std::size_t i = 0; i < d.layouts().size(); ++i) {
    const doc::Layout& L = d.layouts()[i];
    drawing::Space sp;
    sp.key = "layout:" + std::to_string(i);
    sp.name = L.name;
    sp.kind = "paper";
    const double x0 = num(L.paper.get("x0"));
    const double y0 = num(L.paper.get("y0"));
    const double w = num(L.paper.get("w"));
    const double h = num(L.paper.get("h"));
    if (w > 0 && h > 0) {
      sp.paper = drawing::Bounds{.min_x = x0, .min_y = y0, .max_x = x0 + w, .max_y = y0 + h};
      grow(sp.bounds, x0, y0);
      grow(sp.bounds, x0 + w, y0 + h);
    }
    sp.paper_unit = L.paper.get("unit") == Value("in") ? "in" : "mm";
    walk_space(d, *L.entities, sp, o);
    out.spaces.push_back(std::move(sp));
  }
  return out;
}

}  // namespace cc::docview
