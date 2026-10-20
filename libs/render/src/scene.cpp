// SPDX-License-Identifier: Apache-2.0
#include "cc/render/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "cc/render/view.hpp"

namespace cc::render {

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

/// The batch's origin: its first point (dwg-view.js pathsFor), so its floats stay small near the data.
std::array<double, 2> origin_of(const drawing::Batch& b) {
  for (const auto& l : b.lines) {
    if (l.size() >= 2 && std::isfinite(l[0]) && std::isfinite(l[1])) {
      return {l[0], l[1]};
    }
  }
  for (const auto& f : b.fills) {
    for (const auto& r : f.rings) {
      if (r.size() >= 2 && std::isfinite(r[0]) && std::isfinite(r[1])) {
        return {r[0], r[1]};
      }
    }
  }
  for (const auto& t : b.texts) {
    if (std::isfinite(t.x) && std::isfinite(t.y)) {
      return {t.x, t.y};
    }
  }
  return {0, 0};
}

bool finite2(double x, double y) {
  return std::isfinite(x) && std::isfinite(y);
}

void add_polyline(SceneBatch& sb, const std::vector<double>& pts) {
  for (std::size_t i = 0; i + 3 < pts.size(); i += 2) {
    if (!finite2(pts[i], pts[i + 1]) || !finite2(pts[i + 2], pts[i + 3])) {
      continue;  // a Path2D lineTo(NaN) draws nothing: neither does this
    }
    sb.segments.push_back(static_cast<float>(pts[i] - sb.ox));
    sb.segments.push_back(static_cast<float>(pts[i + 1] - sb.oy));
    sb.segments.push_back(static_cast<float>(pts[i + 2] - sb.ox));
    sb.segments.push_back(static_cast<float>(pts[i + 3] - sb.oy));
  }
}

void add_fill(SceneBatch& sb, const drawing::Fill& f) {
  FillGeometry g;
  g.first = static_cast<std::uint32_t>(sb.fan.size() / 2);
  g.pattern = f.pattern;
  g.box = {kInf, kInf, -kInf, -kInf};
  for (const auto& r : f.rings) {
    std::vector<std::array<float, 2>> p;
    for (std::size_t i = 0; i + 1 < r.size(); i += 2) {
      if (finite2(r[i], r[i + 1])) {
        p.push_back({static_cast<float>(r[i] - sb.ox), static_cast<float>(r[i + 1] - sb.oy)});
      }
    }
    if (p.size() < 3) {
      continue;
    }
    for (std::size_t k = 1; k + 1 < p.size(); ++k) {  // a fan from the first point; the closing edge is implied
      for (const auto& q : {p[0], p[k], p[k + 1]}) {
        sb.fan.push_back(q[0]);
        sb.fan.push_back(q[1]);
      }
    }
    for (const auto& q : p) {
      g.box[0] = std::min(g.box[0], q[0]);
      g.box[1] = std::min(g.box[1], q[1]);
      g.box[2] = std::max(g.box[2], q[0]);
      g.box[3] = std::max(g.box[3], q[1]);
    }
  }
  g.count = static_cast<std::uint32_t>(sb.fan.size() / 2) - g.first;
  if (g.count > 0) {
    sb.fills.push_back(g);
  }
}

class TextCache {
 public:
  explicit TextCache(text::Font& font) : font_(&font) {}
  const text::Line& line(const std::string& s) {
    const auto it = lines_.find(s);
    if (it != lines_.end()) {
      return it->second;
    }
    return lines_.emplace(s, font_->layout(s)).first->second;
  }
  [[nodiscard]] float ascender() const { return font_->ascender(); }
  [[nodiscard]] float descender() const { return font_->descender(); }

 private:
  text::Font* font_;
  std::unordered_map<std::string, text::Line> lines_;
};

void add_text(SceneBatch& sb, const drawing::Text& t, TextCache& cache) {
  if (!finite2(t.x, t.y) || !std::isfinite(t.h)) {
    return;
  }
  const text::Line& line = cache.line(t.s);
  // textAlign: left / center / right of [ha] (anything else: left); textBaseline: alphabetic / bottom / middle / top
  float dx = 0;
  if (t.ha && *t.ha == 1) {
    dx = -line.width / 2;
  } else if (t.ha && *t.ha == 2) {
    dx = -line.width;
  }
  float dy = 0;
  if (t.va == 1) {
    dy = -cache.descender();
  } else if (t.va == 2) {
    dy = -(cache.ascender() + cache.descender()) / 2;
  } else if (t.va == 3) {
    dy = -cache.ascender();
  }
  for (const auto& g : line.glyphs) {
    GlyphInstance gi;
    gi.ax = static_cast<float>(t.x - sb.ox);
    gi.ay = static_cast<float>(t.y - sb.oy);
    gi.h = static_cast<float>(t.h);
    gi.rot = static_cast<float>(t.rot);
    gi.x0 = g.x + g.quad.x0 + dx;
    gi.x1 = g.x + g.quad.x1 + dx;
    gi.y0 = g.quad.y0 + dy;
    gi.y1 = g.quad.y1 + dy;
    gi.u0 = g.quad.u0;
    gi.v0 = g.quad.v0;
    gi.u1 = g.quad.u1;
    gi.v1 = g.quad.v1;
    gi.mirror = t.mirror ? 1.0F : 0.0F;
    sb.glyphs.push_back(gi);
  }
}

}  // namespace

Scene build_scene(const drawing::Drawing& d, text::Font& font) {
  std::unordered_map<std::string, std::size_t> layer_index;
  for (std::size_t i = 0; i < d.layers.size(); ++i) {
    layer_index.emplace(d.layers[i].name, i);
  }
  auto layer_of = [&](const std::string& name) {
    const auto it = layer_index.find(name);
    return it == layer_index.end() ? kNoLayer : it->second;
  };
  TextCache cache(font);
  Scene scene;
  scene.spaces.reserve(d.spaces.size());
  for (const drawing::Space& s : d.spaces) {
    SceneSpace ss;
    if (s.kind == "paper" && s.paper) {
      const drawing::Bounds& p = *s.paper;
      SceneBatch fill;
      fill.kind = BatchKind::sheet_fill;
      fill.ox = p.min_x;
      fill.oy = p.min_y;
      add_fill(fill, drawing::Fill{{{p.min_x, p.min_y, p.max_x, p.min_y, p.max_x, p.max_y, p.min_x, p.max_y}}, false});
      SceneBatch edge;
      edge.kind = BatchKind::sheet_edge;
      edge.ox = p.min_x;
      edge.oy = p.min_y;
      add_polyline(edge, {p.min_x, p.min_y, p.max_x, p.min_y, p.max_x, p.max_y, p.min_x, p.max_y, p.min_x, p.min_y});
      ss.batches.push_back(std::move(fill));
      ss.batches.push_back(std::move(edge));
    }
    for (const drawing::Bitmap& bm : s.bitmaps) {  // a document's picture (view.js draws them first)
      if (bm.clip.size() < 6 || !(bm.w > 0) || !(bm.h > 0)) {
        continue;
      }
      SceneBatch pb;
      pb.kind = BatchKind::bitmap;
      pb.ox = bm.x;
      pb.oy = bm.y;
      pb.bitmap = bm.file;
      pb.alpha = static_cast<float>(std::isfinite(bm.alpha) ? std::clamp(bm.alpha, 0.0, 1.0) : 1.0);
      add_fill(pb, drawing::Fill{{bm.clip}, false});
      if (pb.fills.empty()) {
        continue;
      }
      // the bitmap's unit square, x along the width and y down from the top row, onto the picture's corners
      const double wx = bm.ux * bm.w;
      const double wy = bm.uy * bm.w;
      const double hx = bm.vx * bm.h;
      const double hy = bm.vy * bm.h;
      const std::array<std::array<double, 4>, 4> c{
          {{hx, hy, 0, 0}, {hx + wx, hy + wy, 1, 0}, {0, 0, 0, 1}, {wx, wy, 1, 1}}};
      for (const std::size_t k : {0U, 1U, 2U, 2U, 1U, 3U}) {
        pb.quad.insert(pb.quad.end(), {static_cast<float>(c[k][0]), static_cast<float>(c[k][1]),
                                       static_cast<float>(c[k][2]), static_cast<float>(c[k][3])});
      }
      ss.batches.push_back(std::move(pb));
    }
    for (const drawing::Picture& im : s.images) {  // a picture's frame (pixel space → world)
      SceneBatch pb;
      pb.kind = BatchKind::picture;
      pb.layer = layer_of(im.layer);
      pb.ox = im.m[4];
      pb.oy = im.m[5];
      std::vector<double> frame;
      for (const auto& [x, y] :
           std::array<std::array<double, 2>, 5>{{{0, 0}, {im.w, 0}, {im.w, im.h}, {0, im.h}, {0, 0}}}) {
        frame.push_back(im.m[0] * x + im.m[2] * y + im.m[4]);
        frame.push_back(im.m[1] * x + im.m[3] * y + im.m[5]);
      }
      add_polyline(pb, frame);
      ss.batches.push_back(std::move(pb));
    }
    for (std::size_t bi = 0; bi < s.batches.size(); ++bi) {
      const drawing::Batch& b = s.batches[bi];
      const auto o = origin_of(b);
      SceneBatch sb;
      sb.batch = bi;
      sb.layer = layer_of(b.layer);
      sb.ox = o[0];
      sb.oy = o[1];
      for (const auto& l : b.lines) {
        add_polyline(sb, l);
      }
      for (const auto& f : b.fills) {
        add_fill(sb, f);
      }
      for (const auto& t : b.texts) {
        add_text(sb, t, cache);
      }
      ss.segments += sb.segments.size() / 4;
      ss.triangles += sb.fan.size() / 6;
      ss.glyphs += sb.glyphs.size();
      ss.batches.push_back(std::move(sb));
    }
    scene.spaces.push_back(std::move(ss));
  }
  return scene;
}

std::vector<DrawBatch> frame_batches(const Scene& scene, const drawing::Drawing& d, std::size_t s, const View& v,
                                     const FrameOptions& o) {
  std::vector<DrawBatch> out;
  if (s >= d.spaces.size() || s >= scene.spaces.size()) {
    return out;
  }
  const drawing::Space& space = d.spaces[s];
  const Affine view = world_to_device(v);
  const bool paper = space.kind == "paper";
  auto shown = [&](std::size_t layer) {
    if (layer == kNoLayer || layer >= d.layers.size()) {
      return true;
    }
    if (o.hidden != nullptr && layer < o.hidden->size() && (*o.hidden)[layer]) {
      return false;
    }
    return !o.print || d.layers[layer].plot;
  };
  // one group: the images' frames, every batch's fills and lines, then their texts (dwg-view.js drawBatches)
  auto group = [&](std::size_t si, const Affine& m, double px_per_unit, double px_per_mm, bool clip,
                   std::array<float, 4> scissor) {
    const drawing::Space& sp = d.spaces[si];
    std::vector<DrawBatch> texts;
    for (const SceneBatch& sb : scene.spaces[si].batches) {
      if (sb.kind == BatchKind::sheet_fill || sb.kind == BatchKind::sheet_edge || !shown(sb.layer)) {
        continue;
      }
      DrawBatch db;
      db.geometry = &sb;
      db.to_device = mul(m, {1, 0, 0, 1, sb.ox, sb.oy});
      db.css_px_per_unit = px_per_unit;
      db.dpr = v.dpr;
      db.clip = clip;
      db.scissor = scissor;
      if (sb.kind == BatchKind::bitmap) {
        db.color = Rgba{.r = 1, .g = 1, .b = 1, .a = sb.alpha};
        db.line_px = v.dpr;
      } else if (sb.kind == BatchKind::picture) {
        db.color = o.theme.line;
        db.line_px = v.dpr;
      } else {
        const drawing::Batch& b = sp.batches[sb.batch];
        db.color = draw_color(b.color, o.theme, o.print);
        // a hairline at any zoom; in print view the line weight (never thinner than its screen weight)
        const double css = o.print ? std::max({1.0, std::round(b.lw / 0.25), b.lw * px_per_mm}) : 1.0;
        db.line_px = css * v.dpr;
      }
      out.push_back(db);
      if (!sb.glyphs.empty()) {
        db.text = true;
        texts.push_back(db);
      }
    }
    out.insert(out.end(), texts.begin(), texts.end());
  };
  if (paper) {
    for (const SceneBatch& sb : scene.spaces[s].batches) {  // the sheet itself
      if (sb.kind == BatchKind::sheet_fill || sb.kind == BatchKind::sheet_edge) {
        DrawBatch db;
        db.geometry = &sb;
        db.color = sb.kind == BatchKind::sheet_fill ? o.theme.paper : o.theme.line;
        db.to_device = mul(view, {1, 0, 0, 1, sb.ox, sb.oy});
        db.css_px_per_unit = v.zoom;
        db.dpr = v.dpr;
        db.line_px = v.dpr;
        out.push_back(db);
      }
    }
    std::size_t model = d.spaces.size();
    for (std::size_t k = 0; k < d.spaces.size(); ++k) {
      if (d.spaces[k].kind == "model") {
        model = k;
        break;
      }
    }
    if (model < d.spaces.size()) {
      for (const drawing::Viewport& vp : space.viewports) {
        const double k = vp.h / vp.view_height;
        // the viewport's rectangle on the sheet, in device px: the clip
        const Affine& m = view;
        const double x0 = m[0] * (vp.cx - vp.w / 2) + m[4];
        const double x1 = m[0] * (vp.cx + vp.w / 2) + m[4];
        const double y0 = m[3] * (vp.cy + vp.h / 2) + m[5];
        const double y1 = m[3] * (vp.cy - vp.h / 2) + m[5];
        const std::array<float, 4> clip = {static_cast<float>(std::min(x0, x1)), static_cast<float>(std::min(y0, y1)),
                                           static_cast<float>(std::abs(x1 - x0)),
                                           static_cast<float>(std::abs(y1 - y0))};
        group(model, mul(view, viewport_matrix(vp)), v.zoom * k, v.zoom / mm_per_unit(space), true, clip);
      }
    }
  }
  group(s, view, v.zoom, paper ? v.zoom / mm_per_unit(space) : 0, false, {});
  return out;
}

}  // namespace cc::render
