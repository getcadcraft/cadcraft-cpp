// SPDX-License-Identifier: Apache-2.0
// units.js (CivilCraft 0.5).
#include "cc/civil/units.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string>

#include "cc/appkit/jsops.hpp"
#include "cc/base/js.hpp"

namespace cc::civil {
namespace {

constexpr Unit kFt{"ft", "ft", "sq ft", "cu yd", 27, 100, 2, 2, "USSurveyFoot"};
constexpr Unit kM{"m", "m", "sq m", "cu m", 1, 1000, 3, 3, "meter"};

std::string pad_start(std::string s, std::size_t n, char c) {
  if (s.size() < n) {
    s.insert(0, n - s.size(), c);
  }
  return s;
}
/// toFixed's digits argument: ToIntegerOrInfinity, then 0…100 (a RangeError outside: the callers never pass one).
int digits_of(double d) {
  if (std::isnan(d)) {
    return 0;
  }
  return static_cast<int>(std::clamp(std::trunc(d), 0.0, 100.0));
}
bool digit(char c) {
  return c >= '0' && c <= '9';
}

}  // namespace

const Unit& unit_of(std::string_view u) {
  return u == "m" ? kM : kFt;
}

std::string unit_name(const Unit& u, const i18n::Catalog& words) {
  return words.t(u.key == "m" ? "units.m" : "units.ft");
}

std::optional<double> metres_per(std::string_view name) {
  static constexpr std::array<std::pair<std::string_view, double>, 9> kTable{{{"USSurveyFoot", 1200.0 / 3937},
                                                                              {"foot", 0.3048},
                                                                              {"ft", 0.3048},
                                                                              {"meter", 1},
                                                                              {"m", 1},
                                                                              {"metre", 1},
                                                                              {"millimeter", 0.001},
                                                                              {"centimeter", 0.01},
                                                                              {"kilometer", 1000}}};
  for (const auto& [k, v] : kTable) {
    if (k == name) {
      return v;
    }
  }
  return std::nullopt;
}

std::string format_station(double sta, std::string_view unit, std::optional<double> dec) {
  if (!std::isfinite(sta)) {
    return {};
  }
  const Unit& u = unit_of(unit);
  const int d = digits_of(dec.value_or(u.sta_dec));
  const bool neg = sta < 0;
  const double r = base::js_number(base::js_to_fixed(std::abs(sta), d));
  const double big = std::floor(r / u.sta_len + 1e-9);
  const double rest = std::max(r - big * u.sta_len, 0.0);  // (rest < 0 → 0; NaN stays NaN, as the JS)
  const std::size_t w = appkit::num_str(u.sta_len).size() - 1;
  const std::string rest_str =
      pad_start(base::js_to_fixed(rest, d), w + (d != 0 ? static_cast<std::size_t>(d) + 1 : 0), '0');
  return (neg ? "-" : "") + appkit::num_str(big) + "+" + rest_str;
}

bool is_num(std::string_view s) {
  // /^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/ on String(s).trim()
  const std::string_view t = base::js_trim(s);
  std::size_t i = 0;
  if (i < t.size() && (t[i] == '+' || t[i] == '-')) {
    ++i;
  }
  if (i < t.size() && digit(t[i])) {
    while (i < t.size() && digit(t[i])) {
      ++i;
    }
    if (i < t.size() && t[i] == '.') {
      ++i;
      while (i < t.size() && digit(t[i])) {
        ++i;
      }
    }
  } else if (i + 1 < t.size() && t[i] == '.' && digit(t[i + 1])) {
    ++i;
    while (i < t.size() && digit(t[i])) {
      ++i;
    }
  } else {
    return false;
  }
  if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
    ++i;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) {
      ++i;
    }
    if (i >= t.size() || !digit(t[i])) {
      return false;
    }
    while (i < t.size() && digit(t[i])) {
      ++i;
    }
  }
  return i == t.size();
}

std::optional<double> parse_station(std::string_view s, std::string_view unit) {
  // String(s).trim().replace(/\s+/g, '')
  const std::string_view tr = base::js_trim(s);
  std::string t;
  for (std::size_t i = 0; i < tr.size();) {
    const std::size_t n = base::js_space_at(tr, i);
    if (n != 0) {
      i += n;
      continue;
    }
    t.push_back(tr[i]);
    ++i;
  }
  if (t.empty()) {
    return std::nullopt;
  }
  // /^([+-]?)(\d+)\+(\d+(?:\.\d*)?)$/
  std::size_t i = 0;
  bool neg = false;
  if (t[i] == '+' || t[i] == '-') {
    neg = t[i] == '-';
    ++i;
  }
  const std::size_t a0 = i;
  while (i < t.size() && digit(t[i])) {
    ++i;
  }
  const std::size_t a1 = i;
  if (a1 > a0 && i < t.size() && t[i] == '+') {
    ++i;
    const std::size_t b0 = i;
    while (i < t.size() && digit(t[i])) {
      ++i;
    }
    bool ok = i > b0;
    if (ok && i < t.size() && t[i] == '.') {
      ++i;
      while (i < t.size() && digit(t[i])) {
        ++i;
      }
    }
    if (ok && i == t.size()) {
      const double v = base::js_number(std::string_view(t).substr(a0, a1 - a0)) * unit_of(unit).sta_len +
                       base::js_number(std::string_view(t).substr(b0));
      return neg ? -v : v;
    }
  }
  if (is_num(t)) {
    return base::js_number(t);
  }
  return std::nullopt;
}

std::string fixed(double n, double d) {
  if (!std::isfinite(n)) {
    return {};
  }
  const double v = std::abs(n) < 0.5 * std::pow(10, -d) ? 0 : n;
  return base::js_to_fixed(v, digits_of(d));
}

std::string grouped(double n, double d) {
  if (!std::isfinite(n)) {
    return {};
  }
  const int k = digits_of(d);
  return base::js_locale_en(n, k, k);
}

std::string pct(double g, double d) {
  if (!std::isfinite(g)) {
    return {};
  }
  return std::string(g * 100 >= 0 ? "+" : "") + fixed(g * 100, d) + "%";
}

std::string volume_text(double v, std::string_view unit, double d) {
  const Unit& u = unit_of(unit);
  return grouped(v / u.vol_per, d) + " " + std::string(u.vol);
}
std::string area_text(double a, std::string_view unit, double d) {
  return grouped(a, d) + " " + std::string(unit_of(unit).area);
}

std::string dms(double deg) {
  double s = appkit::js_round(std::abs(deg) * 3600);
  const double d = std::floor(s / 3600);
  s -= d * 3600;
  const double m = std::floor(s / 60);
  s -= m * 60;
  return appkit::num_str(d) + "°" + pad_start(appkit::num_str(m), 2, '0') + "'" +
         pad_start(appkit::num_str(s), 2, '0') + "\"";
}

std::string bearing(double a) {
  double az = std::fmod(90 - a * 180 / std::numbers::pi, 360);
  if (az < 0) {
    az += 360;
  }
  const char* q0 = "N";
  const char* q1 = "E";
  double ang = 0;
  if (az <= 90) {
    ang = az;
  } else if (az <= 180) {
    q0 = "S";
    ang = 180 - az;
  } else if (az <= 270) {
    q0 = "S";
    q1 = "W";
    ang = az - 180;
  } else {
    q1 = "W";
    ang = 360 - az;
  }
  return std::string(q0) + " " + dms(ang) + " " + q1;
}

}  // namespace cc::civil
