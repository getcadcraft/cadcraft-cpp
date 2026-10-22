// SPDX-License-Identifier: Apache-2.0
// CivilCraft 0.5's alignment.js (horizontal alignments by PIs with circular curves) and profile.js (vertical profiles
// by PVIs with symmetric parabolic curves; existing ground sampled from a surface).
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/civil/tin.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::civil {

/// normAng: an angle into (−π, π].
[[nodiscard]] double norm_ang(double a);

struct Pi {
  double x = 0, y = 0;
  base::Value r;  // as given (Number(r) || 0 is the radius)
};

struct Element {
  bool arc = false;
  P2 p0, p1;
  double dir = 0, sta = 0, len = 0;
  // arcs
  P2 c, pi;
  double r = 0, delta = 0, a0 = 0, T = 0, E = 0, M = 0, chord = 0, dir_in = 0, dir_out = 0;
  bool ccw = false;
  double pi_index = 0;
};

struct StaOff {
  double sta, off, x, y;
  bool outside;
  double dist;
};
struct AlPoint {
  double x, y, dir;
};

class Alignment {
 public:
  Alignment(std::vector<Element> elements_in, double start_in, std::vector<std::string> errors_in,
            std::vector<Pi> pis_in);
  std::vector<Element> elements;
  double start = 0;
  std::vector<std::string> errors;
  std::vector<Pi> pis;
  double length = 0, end = 0;
  [[nodiscard]] bool ok() const { return !elements.empty() && errors.empty(); }
  [[nodiscard]] const Element* element_at(double sta) const;
  [[nodiscard]] std::optional<AlPoint> point_at(double sta) const;
  [[nodiscard]] std::optional<StaOff> station_offset(double x, double y) const;
  struct PolyPt {
    double x, y, sta;
  };
  [[nodiscard]] std::vector<PolyPt> polyline(double max_seg, double max_turn) const;
  struct Tick {
    double sta, x, y, dir;
  };
  [[nodiscard]] std::vector<Tick> ticks(double step) const;
  struct Key {
    double sta;
    const char* kind;
  };
  [[nodiscard]] std::vector<Key> key_stations() const;
};

/// buildAlignment({ pis, start }): PIs with a non-finite x or y are dropped, then repeats.
[[nodiscard]] std::shared_ptr<Alignment> build_alignment(const std::vector<Pi>& pis, double start,
                                                         const i18n::Catalog& words);

struct ProfileCurve {
  double i = 0, pvi = 0, z_pvi = 0, bvc = 0, evc = 0, L = 0, g1 = 0, g2 = 0, A = 0, K = 0, z_bvc = 0, z_evc = 0;
  bool crest = false;
  std::optional<std::pair<double, double>> extreme;  // high (crest) or low (sag): sta, z
};
struct Grade {
  double from, to, g;
};

class Profile {
 public:
  std::vector<base::Value> pvis;  // the PVIs as given (copied), with L set
  std::vector<double> sta, z;     // their stations and elevations
  std::vector<Grade> grades;
  std::vector<ProfileCurve> curves;
  std::vector<std::string> errors;
  double start = 0, end = 0;
  [[nodiscard]] bool ok() const { return pvis.size() >= 2 && errors.empty(); }
  [[nodiscard]] std::optional<double> elevation_at(double s) const;
  [[nodiscard]] std::optional<double> grade_at(double s) const;
  struct Sample {
    double sta;
    std::optional<double> z;
  };
  /// step: nullopt = undefined (or 0: `step || L / 32`).
  [[nodiscard]] std::vector<Sample> sample(std::optional<double> step) const;
};

/// buildProfile(pvis): each PVI an object {sta, z, L | K, …}; those without finite numbers are dropped.
[[nodiscard]] std::shared_ptr<Profile> build_profile(const base::Value& pvis, const i18n::Catalog& words);

/// sampleSurface(surface, alignment, { step }): the ground along the alignment.
struct GroundSample {
  double sta;
  std::optional<double> z;
};
[[nodiscard]] std::vector<GroundSample> sample_surface(const Surface* surface, const Alignment& al,
                                                       std::optional<double> step);

/// Hostile-input caps (docs/gaps.md): the JS would run until it ran out of memory.
inline constexpr double kMaxPolylinePieces = 1e5;
inline constexpr double kMaxSamples = 1e5;

}  // namespace cc::civil
