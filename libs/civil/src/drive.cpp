// SPDX-License-Identifier: Apache-2.0
// drive.js (CivilCraft 0.5), ported function by function; V8's sin, cos, tan, asin, acos and pow (libs/base).
#include "cc/civil/drive.hpp"

#include <cmath>
#include <numbers>

#include "cc/appkit/jsops.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::js_acos;
using base::js_asin;
using base::js_cos;
using base::js_pow;
using base::js_sin;
using base::js_tan;
using base::make_object;
using base::Value;
constexpr double kR = std::numbers::pi / 180;

double num(const Value& v) {
  return base::js_to_number(v);
}
Value get(const Value& o, std::string_view k) {
  return o.is_object() ? o.get(k) : Value();
}
double jmax(double a, double b) {
  return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::max(a, b);
}
double jmin(double a, double b) {
  return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::min(a, b);
}
double refraction(double h) {
  if (h > 85) {
    return 0;
  }
  const double te = js_tan(h * kR);
  double r = 0;
  if (h > 5) {
    r = 58.1 / te - 0.07 / js_pow(te, 3) + 0.000086 / js_pow(te, 5);
  } else if (h > -0.575) {
    r = 1735 + h * (-518.2 + h * (103.4 + h * (-12.79 + h * 0.711)));
  } else {
    r = -20.774 / te;
  }
  return r / 3600;
}

}  // namespace

Value drive_speeds(bool metres) {
  Array out;
  for (const double v : metres ? std::array<double, 4>{15, 40, 70, 100} : std::array<double, 4>{10, 25, 45, 65}) {
    out.push_back(make_object({{"label", appkit::num_str(v) + (metres ? " km/h" : " mph")},
                               {"perSec", metres ? (v * 1000) / 3600 : (v * 5280) / 3600}}));
  }
  return out;
}

double eye_height(bool metres) {
  return metres ? 1.07 : 3.5;
}

ElevationAlong::ElevationAlong(const Alignment& al, const Profile* profile,
                               std::function<std::optional<double>(double, double)> ground)
    : al_(&al), profile_(profile != nullptr && profile->ok() ? profile : nullptr), ground_(std::move(ground)) {}

std::optional<double> ElevationAlong::operator()(double sta) {
  std::optional<double> z = profile_ != nullptr ? profile_->elevation_at(sta) : std::nullopt;
  if (!z || !std::isfinite(*z)) {
    const auto p = al_->point_at(sta);
    if (!p) {
      throw std::runtime_error("Cannot read properties of null (reading 'x')");
    }
    z = ground_ ? ground_(p->x, p->y) : std::nullopt;
  }
  if (z && std::isfinite(*z)) {
    last_ = z;
  }
  return last_;
}

Value drive_frame(const Alignment& al, ElevationAlong& elev, double sta, const Value& opts) {
  auto opt = [&](std::string_view k, double d) {
    const Value v = get(opts, k);
    return v.is_undefined() ? d : num(v);
  };
  const double eye = opt("eye", 3.5);
  const double ahead = opt("ahead", 100);
  const double exag = opt("exag", 1);
  const Value origin_v = get(opts, "origin");
  const double ox = origin_v.is_undefined() ? 0 : num(get(origin_v, "x"));
  const double oy = origin_v.is_undefined() ? 0 : num(get(origin_v, "y"));
  const double oz = origin_v.is_undefined() ? 0 : num(get(origin_v, "z"));
  const double s = jmax(al.start, jmin(al.end, sta));
  const auto p = al.point_at(s);
  if (!p) {
    throw std::runtime_error("Cannot read properties of null (reading 'x')");
  }
  const double z = elev(s).value_or(oz);
  auto L = [&](double x, double y, double zz) {
    return Value(Array{Value(x - ox), Value(y - oy), Value((zz - oz) * exag)});
  };
  const double s2 = jmin(al.end, s + ahead);
  double qx = 0;
  double qy = 0;
  double z2 = z;
  if (s2 - s > ahead * 0.25) {
    const AlPoint q = al.point_at(s2).value_or(*p);  // s2 is on the alignment
    qx = q.x;
    qy = q.y;
    z2 = elev(s2).value_or(z);
  } else {
    qx = p->x + ahead * js_cos(p->dir);
    qy = p->y + ahead * js_sin(p->dir);
  }
  return make_object({{"eye", L(p->x, p->y, z + eye)}, {"target", L(qx, qy, z2 + eye)}, {"z", z}, {"sta", s}});
}

Value sun_position(double ms, double lat, double lon) {
  const double jd = ms / 86400000 + 2440587.5;
  const double T = (jd - 2451545) / 36525;
  const double L0 = std::fmod(std::fmod(280.46646 + T * (36000.76983 + T * 0.0003032), 360) + 360, 360);
  const double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
  const double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
  const double C = js_sin(M * kR) * (1.914602 - T * (0.004817 + 0.000014 * T)) +
                   js_sin(2 * M * kR) * (0.019993 - 0.000101 * T) + js_sin(3 * M * kR) * 0.000289;
  const double omega = 125.04 - 1934.136 * T;
  const double lambda = L0 + C - 0.00569 - 0.00478 * js_sin(omega * kR);
  const double eps0 = 23 + (26 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60) / 60;
  const double eps = eps0 + 0.00256 * js_cos(omega * kR);
  const double decl = js_asin(js_sin(eps * kR) * js_sin(lambda * kR)) / kR;
  const double y = js_pow(js_tan((eps / 2) * kR), 2);
  const double eot =
      (4 / kR) * (y * js_sin(2 * L0 * kR) - 2 * e * js_sin(M * kR) + 4 * e * y * js_sin(M * kR) * js_cos(2 * L0 * kR) -
                  0.5 * y * y * js_sin(4 * L0 * kR) - 1.25 * e * e * js_sin(2 * M * kR));
  // new Date(ms).getUTCHours() … getUTCMilliseconds(): the time of day of ms (an invalid date gives NaN)
  double minutes = std::nan("");
  if (std::isfinite(ms) && std::abs(ms) <= 8.64e15) {
    const double t = std::trunc(ms);
    double day = std::fmod(t, 86400000);
    if (day < 0) {
      day += 86400000;
    }
    const double h = std::floor(day / 3600000);
    const double mi = std::floor(std::fmod(day, 3600000) / 60000);
    const double se = std::floor(std::fmod(day, 60000) / 1000);
    const double msec = std::fmod(day, 1000);
    minutes = h * 60 + mi + se / 60 + msec / 60000;
  }
  double tst = std::fmod(minutes + eot + 4 * lon, 1440);
  if (tst < 0) {
    tst += 1440;
  }
  const double ha = tst / 4 < 0 ? tst / 4 + 180 : tst / 4 - 180;
  const double cos_z = js_sin(lat * kR) * js_sin(decl * kR) + js_cos(lat * kR) * js_cos(decl * kR) * js_cos(ha * kR);
  const double zen = js_acos(jmax(-1, jmin(1, cos_z))) / kR;
  const double elev = 90 - zen;
  const double sz = js_sin(zen * kR);
  double az = 0;
  if (std::abs(sz) < 1e-9) {
    az = lat > decl ? 180 : 0;
  } else {
    const double c = (js_sin(lat * kR) * js_cos(zen * kR) - js_sin(decl * kR)) / (js_cos(lat * kR) * sz);
    const double a = js_acos(jmax(-1, jmin(1, c))) / kR;
    az = ha > 0 ? std::fmod(a + 180, 360) : std::fmod(540 - a, 360);
  }
  return make_object({{"azimuth", az}, {"altitude", elev + refraction(elev)}, {"declination", decl}, {"eot", eot}});
}

Value sun_vector(double azimuth, double altitude) {
  const double a = azimuth * kR;
  const double h = altitude * kR;
  return Array{Value(js_cos(h) * js_sin(a)), Value(js_cos(h) * js_cos(a)), Value(js_sin(h))};
}

}  // namespace cc::civil
