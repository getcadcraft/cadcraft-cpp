// SPDX-License-Identifier: Apache-2.0
// The point cloud in the viewer (cloud-cad.js draw and snap; cadcraft.js zoomCloud): every point shown is put in one
// picture of the canvas, the highest point winning a pixel, and drawn under the linework of the Model tab. Node snaps
// to the points drawn, after the drawing's own ends, middles and centres. Not drawn through a sheet's viewports yet
// (docs/gaps.md).
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/m3d/drawing3d.hpp"
#include "cc/render/view.hpp"
#include "cc/ui/viewer.hpp"

namespace cc::ui {

cloud::DrawingUnits Viewer::cloud_units() const {
  std::string units;
  if (cad_) {
    const base::Value u = cad_->meta.get("units");
    units = u.truthy() ? base::js_string(u) : std::string();
  }
  return {.units = units, .m_per_unit = m3d::m_per_unit(units)};
}

/// cadcraft.js zoomCloud: fitPoints of the cloud's corners, 0.9 of the view
void Viewer::zoom_to_cloud() {
  const auto b = cloud_.bbox(cloud_units());
  if (!b || space_ >= views_.size() || drawing_.spaces.empty() || drawing_.spaces[space_].kind == "paper") {
    return;
  }
  render::View& v = views_[space_];
  v.cx = (b->min_x + b->max_x) / 2;
  v.cy = (b->min_y + b->max_y) / 2;
  v.zoom = std::clamp(
      std::min(v.width / std::max(b->max_x - b->min_x, 1e-6), v.height / std::max(b->max_y - b->min_y, 1e-6)) * 0.9,
      1e-9, 1e9);
  redraw_ = std::max(redraw_, 2);
}

std::optional<cad::Snap> Viewer::cloud_snap(cad::P2 p, double r) {
  if (space_ >= drawing_.spaces.size() || drawing_.spaces[space_].kind == "paper") {
    return std::nullopt;
  }
  const auto q = cloud_.snap(p.x, p.y, r, cloud_units());
  if (!q) {
    return std::nullopt;
  }
  return cad::Snap{.kind = "node", .x = (*q)[0], .y = (*q)[1], .defer = std::nullopt, .pr = 3};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — cloud-cad.js draw(), step for step
void Viewer::cloud_underlay(const render::View& v, double ox, double oy) {
  const bool paper = space_ < drawing_.spaces.size() && drawing_.spaces[space_].kind == "paper";
  if (!cloud_.cloud || !cloud_.visible || paper) {
    renderer_.set_underlay(nullptr, {0, 0});
    cloud_key_.clear();
    return;
  }
  const cloud::DrawingUnits u = cloud_units();
  const double d = v.dpr;
  const auto W = static_cast<std::uint32_t>(std::max(1.0, std::ceil(v.width * d)));
  const auto H = static_cast<std::uint32_t>(std::max(1.0, std::ceil(v.height * d)));
  std::string key;
  for (const double x : {v.cx, v.cy, v.zoom, v.width, v.height, d, ox, oy, static_cast<double>(cloud_.version)}) {
    base::append_number(key, x);
    key += '|';
  }
  key += u.units;
  if (key == cloud_key_) {
    return;  // the picture of the last frame still holds
  }
  cloud_key_ = key;
  const cloud::Cloud& c = *cloud_.cloud;
  cloud_img_.w = W;
  cloud_img_.h = H;
  cloud_img_.px.assign(std::size_t{W} * H * 4, 0);
  std::vector<float> zb(std::size_t{W} * H, -std::numeric_limits<float>::infinity());
  const double k = cloud_.scale(u);
  const double ox_d = cloud_.insert ? (*cloud_.insert)[0] + (c.origin[0] - c.min[0]) * k : c.origin[0] * k;
  const double oy_d = cloud_.insert ? (*cloud_.insert)[1] + (c.origin[1] - c.min[1]) * k : c.origin[1] * k;
  // drawing → device px of the canvas (the view's own map, dpr included)
  const render::Affine T = render::world_to_device(v);
  const double a = T[0] * k;
  const double b = T[1] * k;
  const double cc = T[2] * k;
  const double e = T[3] * k;
  const double tx = T[0] * ox_d + T[2] * oy_d + T[4];
  const double ty = T[1] * ox_d + T[3] * oy_d + T[5];
  const auto s = static_cast<std::int64_t>(std::max(1.0, base::js_round(cloud_.size * d * 0.75)));
  const std::int64_t h0 = s >> 1;
  const auto Wi = static_cast<std::int64_t>(W);
  const auto Hi = static_cast<std::int64_t>(H);
  const std::vector<std::uint8_t>& rgb = cloud_.colors;
  const auto plot = [&](std::size_t i) {
    const auto X = static_cast<double>(c.pos[i * 3]);
    const auto Y = static_cast<double>(c.pos[i * 3 + 1]);
    const float Z = c.pos[i * 3 + 2];
    const double fx = base::js_round(a * X + cc * Y + tx);
    const double fy = base::js_round(b * X + e * Y + ty);
    if (!std::isfinite(fx) || !std::isfinite(fy) || fx < -1e9 || fy < -1e9 || fx > 1e9 || fy > 1e9) {
      return;
    }
    const auto sx = static_cast<std::int64_t>(fx) - h0;
    const auto sy = static_cast<std::int64_t>(fy) - h0;
    if (sx + s <= 0 || sy + s <= 0 || sx >= Wi || sy >= Hi) {
      return;
    }
    for (std::int64_t yy = std::max<std::int64_t>(0, sy); yy < std::min(Hi, sy + s); ++yy) {
      const auto row = static_cast<std::size_t>(yy * Wi);
      for (std::int64_t xx = std::max<std::int64_t>(0, sx); xx < std::min(Wi, sx + s); ++xx) {
        const std::size_t at = row + static_cast<std::size_t>(xx);
        if (Z >= zb[at]) {
          zb[at] = Z;
          cloud_img_.px[at * 4] = i * 3 < rgb.size() ? rgb[i * 3] : 200;
          cloud_img_.px[at * 4 + 1] = i * 3 + 1 < rgb.size() ? rgb[i * 3 + 1] : 200;
          cloud_img_.px[at * 4 + 2] = i * 3 + 2 < rgb.size() ? rgb[i * 3 + 2] : 200;
          cloud_img_.px[at * 4 + 3] = 255;
        }
      }
    }
  };
  if (const std::vector<std::uint32_t>* idx = cloud_.shown(u)) {
    for (const std::uint32_t i : *idx) {
      plot(i);
    }
  } else {
    for (std::size_t i = 0; i < c.n; ++i) {
      plot(i);
    }
  }
  renderer_.set_underlay(&cloud_img_, {static_cast<float>(ox), static_cast<float>(oy)});
}

}  // namespace cc::ui
