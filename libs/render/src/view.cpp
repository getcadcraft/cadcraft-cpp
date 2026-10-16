// SPDX-License-Identifier: Apache-2.0
#include "cc/render/view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace cc::render {

namespace {

Rgba rgb8(unsigned r, unsigned g, unsigned b) {
  return {static_cast<float>(r) / 255.0F, static_cast<float>(g) / 255.0F, static_cast<float>(b) / 255.0F, 1};
}
Rgba hex(std::string_view h) {
  return parse_hex(h);
}
int hex_digit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}
/// dwg-view.js rgb(): "#rrggbb" → channels 0–255, grey for anything else.
std::array<int, 3> channels(std::string_view h) {
  if (!h.empty() && h[0] == '#') {
    h.remove_prefix(1);
  }
  std::array<int, 3> c{128, 128, 128};
  if (h.size() != 6) {
    return c;
  }
  for (int i = 0; i < 6; ++i) {
    if (hex_digit(h[static_cast<std::size_t>(i)]) < 0) {
      return {128, 128, 128};
    }
  }
  for (std::size_t i = 0; i < 3; ++i) {
    c.at(i) = hex_digit(h[i * 2]) * 16 + hex_digit(h[i * 2 + 1]);
  }
  return c;
}
double luminance(const std::array<int, 3>& c) {
  auto lin = [](int v) {
    const double x = v / 255.0;
    return x <= 0.03928 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * lin(c[0]) + 0.7152 * lin(c[1]) + 0.0722 * lin(c[2]);
}
Rgba mix(const std::array<int, 3>& a, const std::array<int, 3>& b, double t) {
  std::array<unsigned, 3> o{};
  for (std::size_t i = 0; i < 3; ++i) {
    o.at(i) = static_cast<unsigned>(std::floor(a.at(i) + (b.at(i) - a.at(i)) * t + 0.5));  // Math.round
  }
  return rgb8(o[0], o[1], o[2]);
}
std::array<int, 3> channels_of(const Rgba& c) {
  return {static_cast<int>(std::lround(c.r * 255)), static_cast<int>(std::lround(c.g * 255)),
          static_cast<int>(std::lround(c.b * 255))};
}

}  // namespace

Rgba parse_hex(std::string_view h) {
  const auto c = channels(h);
  return rgb8(static_cast<unsigned>(c[0]), static_cast<unsigned>(c[1]), static_cast<unsigned>(c[2]));
}

Theme Theme::model() {
  return {hex("#1D232B"), hex("#252D37"), hex("#E8EEF4"), hex("#6C7A8A"), true};
}
Theme Theme::light() {
  return {hex("#F3F6F8"), hex("#FFFFFF"), hex("#17202B"), hex("#7A8796"), false};
}
Theme Theme::print(bool sheet) {
  const Rgba bg = sheet ? hex("#c9ced3") : hex("#ffffff");
  return {bg, hex("#ffffff"), hex("#000000"), hex("#80868c"), luminance(channels_of(bg)) < 0.4};
}

Rgba draw_color(std::string_view color, const Theme& theme, bool print) {
  if (color == "ink" || print) {
    return theme.ink;
  }
  const auto c = channels(color);
  const double l = luminance(c);
  if (!theme.dark && l > 0.6) {
    return mix(c, {0, 0, 0}, 0.45);
  }
  if (theme.dark && l < 0.12) {
    return mix(c, {255, 255, 255}, 0.45);
  }
  return rgb8(static_cast<unsigned>(c[0]), static_cast<unsigned>(c[1]), static_cast<unsigned>(c[2]));
}

View fit(const drawing::Space& s, View v) {
  const drawing::Bounds& b = s.kind == "paper" && s.paper ? *s.paper : s.bounds;
  if (!std::isfinite(b.min_x)) {
    v.cx = 0;
    v.cy = 0;
    v.zoom = 1;
    return v;
  }
  const double w = std::max(b.max_x - b.min_x, 1e-6);
  const double h = std::max(b.max_y - b.min_y, 1e-6);
  v.zoom = std::min(v.width / w, v.height / h) * 0.92;
  v.cx = (b.min_x + b.max_x) / 2;
  v.cy = (b.min_y + b.max_y) / 2;
  return v;
}

std::array<double, 2> to_world(const View& v, double px, double py) {
  return {v.cx + (px - v.width / 2) / v.zoom, v.cy - (py - v.height / 2) / v.zoom};
}

View zoom_at(View v, double px, double py, double factor) {
  const auto [wx, wy] = to_world(v, px, py);
  v.zoom = std::clamp(v.zoom * factor, 1e-9, 1e9);
  v.cx = wx - (px - v.width / 2) / v.zoom;
  v.cy = wy + (py - v.height / 2) / v.zoom;
  return v;
}

View pan_by(View v, double dx, double dy) {
  v.cx -= dx / v.zoom;
  v.cy += dy / v.zoom;
  return v;
}

Affine world_to_device(const View& v) {
  return {v.zoom * v.dpr,
          0,
          0,
          -v.zoom * v.dpr,
          (v.width / 2 - v.cx * v.zoom) * v.dpr,
          (v.height / 2 + v.cy * v.zoom) * v.dpr};
}

Affine viewport_matrix(const drawing::Viewport& vp) {
  const double k = vp.h / vp.view_height;
  const double c = std::cos(vp.twist);
  const double s = std::sin(vp.twist);
  return {k * c, k * s, -k * s, k * c, vp.cx - k * (c * vp.vx - s * vp.vy), vp.cy - k * (s * vp.vx + c * vp.vy)};
}

Affine mul(const Affine& p, const Affine& q) {
  return {p[0] * q[0] + p[2] * q[1], p[1] * q[0] + p[3] * q[1],        p[0] * q[2] + p[2] * q[3],
          p[1] * q[2] + p[3] * q[3], p[0] * q[4] + p[2] * q[5] + p[4], p[1] * q[4] + p[3] * q[5] + p[5]};
}

double mm_per_unit(const drawing::Space& sheet) {
  return sheet.paper_unit == "mm" ? 1 : 25.4;
}

}  // namespace cc::render
