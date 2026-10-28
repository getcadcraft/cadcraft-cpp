// SPDX-License-Identifier: Apache-2.0
// CivilCraft 0.5's drive.js: driving along an alignment in the 3D view (eye and target per station) and where the sun
// is (NOAA's solar calculator equations).
#pragma once

#include <functional>
#include <optional>

#include "cc/base/value.hpp"
#include "cc/civil/alignment.hpp"

namespace cc::civil {

[[nodiscard]] base::Value drive_speeds(bool metres);
[[nodiscard]] double eye_height(bool metres);
/// elevationAlong(al, profile, ground): the design profile where it has one, else the ground on the centreline; the
/// last known elevation where neither has one
class ElevationAlong {
 public:
  ElevationAlong(const Alignment& al, const Profile* profile,
                 std::function<std::optional<double>(double, double)> ground);
  std::optional<double> operator()(double sta);

 private:
  const Alignment* al_;
  const Profile* profile_;
  std::function<std::optional<double>(double, double)> ground_;
  std::optional<double> last_;
};
/// driveFrame(al, elevAt, sta, { eye, ahead, origin, exag })
[[nodiscard]] base::Value drive_frame(const Alignment& al, ElevationAlong& elev, double sta, const base::Value& opts);
/// sunPosition(date ms, lat, lon) → { azimuth, altitude, declination, eot }
[[nodiscard]] base::Value sun_position(double ms, double lat, double lon);
[[nodiscard]] base::Value sun_vector(double azimuth, double altitude);

}  // namespace cc::civil
