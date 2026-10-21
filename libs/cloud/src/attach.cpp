// SPDX-License-Identifier: Apache-2.0
// cloud-cad.js CloudAttach, step for step: see attach.hpp.
#include "cc/cloud/attach.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "cc/base/js.hpp"

namespace cc::cloud {

std::optional<double> unit_m(std::string_view unit) {
  if (unit == "m") {
    return 1.0;
  }
  if (unit == "ft") {
    return 0.3048;
  }
  if (unit == "ftUS") {
    return 1200.0 / 3937;
  }
  return std::nullopt;
}

namespace {

std::string num(double v) {
  std::string s;
  base::append_number(s, v);
  return s;
}

}  // namespace

void Attached::attach(Cloud c, std::optional<std::array<double, 2>> at) {
  cloud = std::move(c);
  insert = at;
  const std::vector<std::string> modes = color_modes(*cloud);
  if (std::ranges::find(modes, color) == modes.end()) {
    color = cloud->rgb ? "rgb" : "height";
  }
  colors = colorize(*cloud, color);
  // a scan of a building (taller than 2 m, under 200 m across) starts as a plan cut: seen from above it is its ceiling
  // or roof; a site or aerial scan starts with every point
  const double um = unit_m(cloud->unit.value_or("m")).value_or(1);
  const double zm = unit_m(cloud->z_unit.value_or(cloud->unit.value_or("m"))).value_or(1);
  const double across = std::max(cloud->max[0] - cloud->min[0], cloud->max[1] - cloud->min[1]) * um;
  const double tall = (cloud->max[2] - cloud->min[2]) * zm;
  slice = Slice{.on = tall > 2 && across < 200, .h = std::nullopt, .thick = std::nullopt};
  visible = true;
  changed();
}

void Attached::detach() {
  cloud.reset();
  colors.clear();
  changed();
}

void Attached::changed() {
  ++version;
  grid_.reset();
  grid_of_.clear();
}

void Attached::recolor(std::array<std::uint8_t, 3> color_one) {
  if (cloud) {
    colors = colorize(*cloud, color, color_one);
  }
}

double Attached::scale(const DrawingUnits& u) const {
  if (u.units.empty() || !cloud || !cloud->unit) {
    return 1;
  }
  return unit_m(*cloud->unit).value_or(NAN) / u.m_per_unit;
}

double Attached::z_scale(const DrawingUnits& u) const {
  if (!cloud) {
    return 1;
  }
  const std::optional<std::string> zu = cloud->z_unit ? cloud->z_unit : cloud->unit;
  if (u.units.empty() || !zu) {
    return 1;
  }
  return unit_m(*zu).value_or(NAN) / u.m_per_unit;
}

V3 Attached::to_drawing(double x, double y, double z, const DrawingUnits& u) const {
  if (!cloud) {
    return {x, y, z};
  }
  const double k = scale(u);
  const Cloud& c = *cloud;
  if (!insert) {
    return {x * k, y * k, (z - c.min[2]) * z_scale(u)};
  }
  return {(*insert)[0] + (x - c.min[0]) * k, (*insert)[1] + (y - c.min[1]) * k, (z - c.min[2]) * z_scale(u)};
}

std::array<double, 2> Attached::to_file(double x, double y, const DrawingUnits& u) const {
  if (!cloud) {
    return {x, y};
  }
  const double k = scale(u);
  const Cloud& c = *cloud;
  if (!insert) {
    return {x / k, y / k};
  }
  return {(x - (*insert)[0]) / k + c.min[0], (y - (*insert)[1]) / k + c.min[1]};
}

V3 Attached::drawing_point(std::size_t i, const DrawingUnits& u) const {
  if (!cloud) {
    return {};
  }
  const V3 p = point_at(*cloud, i);
  return to_drawing(p[0], p[1], p[2], u);
}

std::optional<Bbox> Attached::bbox(const DrawingUnits& u) const {
  if (!cloud) {
    return std::nullopt;
  }
  const V3 a = to_drawing(cloud->min[0], cloud->min[1], 0, u);
  const V3 b = to_drawing(cloud->max[0], cloud->max[1], 0, u);
  return Bbox{.min_x = std::min(a[0], b[0]),
              .min_y = std::min(a[1], b[1]),
              .max_x = std::max(a[0], b[0]),
              .max_y = std::max(a[1], b[1])};
}

Attached::SliceSpec Attached::slice_spec(const DrawingUnits& u) const {
  double per = u.m_per_unit;
  if (per == 0 || std::isnan(per)) {
    per = 0.001;
  }
  const bool unitless = u.units.empty();
  const auto m = [&](double v) {
    const std::string cu = cloud && cloud->unit ? *cloud->unit : std::string("m");
    return unitless ? v / unit_m(cu).value_or(1) : v / per;
  };
  return {.h = slice.h.value_or(m(1.2)), .thick = slice.thick.value_or(m(0.1))};
}

const std::vector<std::uint32_t>* Attached::shown(const DrawingUnits& u) {
  if (!cloud || !slice.on) {
    return nullptr;
  }
  const SliceSpec s = slice_spec(u);
  const std::string key = std::to_string(version) + ":" + num(s.h) + "," + num(s.thick);
  if (key != shown_key_) {
    const double zs = z_scale(u);
    shown_ = slice_of(*cloud, 'z', cloud->min[2] + s.h / zs, s.thick / zs);
    shown_key_ = key;
  }
  return &shown_;
}

std::optional<std::array<double, 2>> Attached::snap(double px, double py, double r, const DrawingUnits& u) {
  if (!cloud || !visible) {
    return std::nullopt;
  }
  const std::vector<std::uint32_t>* idx = shown(u);
  const std::string of = (idx != nullptr ? shown_key_ : std::string()) + "|" + std::to_string(version);
  if (!cloud) {
    return std::nullopt;
  }
  if (!grid_ || grid_of_ != of) {
    grid_.emplace(*cloud, idx);
    grid_of_ = of;
  }
  const auto f = to_file(px, py, u);
  const std::int64_t i = grid_->nearest(f[0], f[1], r / scale(u));
  if (i < 0) {
    return std::nullopt;
  }
  const V3 q = drawing_point(static_cast<std::size_t>(i), u);
  return std::array<double, 2>{q[0], q[1]};
}

}  // namespace cc::cloud
