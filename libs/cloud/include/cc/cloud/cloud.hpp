// SPDX-License-Identifier: Apache-2.0
// cc::cloud — L2. Point clouds under a drawing (CadCraft 0.5 SG.1: pointcloud.js and las.js). Files are untrusted:
// every size, count and offset is checked before it is used.
//
//   read_las_header(bytes) → the LAS 1.0–1.4 header, its variable-length records, how many points the file holds
//   read_info(name, bytes) → what the file is: LAS / LAZ (header) or text (the first 64 KB: columns, a count)
//   load_cloud(name, bytes, info, budget) → every point read once; at most `budget` kept, evenly through the file;
//     positions as float32 RELATIVE to `origin` (file units), so state-plane coordinates keep their millimetres
//   colorize(cloud, mode) · color_modes(cloud) · slice_of(cloud, axis, at, thick) · PlanGrid (nearest in plan) ·
//   summary(cloud)
//
// Text clouds: "x y z", "x y z r g b", "x y z i", "x y z i r g b" (PTS: a first line with the count), separated by
// spaces, tabs, commas or semicolons; lines that are not numbers are skipped and counted. Not yet (docs/gaps.md):
// the file's coordinate system (M5.8h2), LAZ (M5.8h3).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cc/base/error.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::cloud {

using Bytes = std::span<const std::byte>;
using V3 = std::array<double, 3>;

struct Vlr {
  std::string user, desc;
  std::uint16_t id = 0;
  std::uint64_t offset = 0, length = 0;
  bool ext = false;
};

struct LasHeader {
  std::string version;  // "1.4"
  int format = 0;
  std::uint32_t record_length = 0, offset_to_points = 0, header_size = 0;
  std::uint64_t count = 0, available = 0;
  V3 scale{}, offset{}, min{}, max{};
  std::vector<Vlr> vlrs, evlrs;
  bool compressed = false;
  std::vector<std::string> notes;
};
/// readLasHeader: the header of a LAS or LAZ file, or why it cannot be read
[[nodiscard]] base::Result<LasHeader> read_las_header(Bytes file, const i18n::Catalog& words);

/// Which column of a text cloud is what (textColumns): -1 = none
struct Columns {
  int n = 0, i = -1, rgb = -1;
};
[[nodiscard]] Columns text_columns(int n);

/// "las" | "xyz" | "" (sniffCloud: the LASF signature, else the extension)
[[nodiscard]] std::string sniff(std::string_view name, Bytes head);

struct Info {
  std::string kind;  // las | laz | xyz
  std::string name;
  std::uint64_t size = 0;
  double count = 0;  // a text cloud's declared count may be anything (a number, as the JS keeps it)
  bool estimate = false;
  std::optional<V3> min, max;
  bool color = false, intensity = false, classes = false;
  std::optional<std::string> unit, z_unit, crs_note;  // the coordinate system's (M5.8h2): none yet
  std::vector<std::string> notes;
  std::optional<LasHeader> las;
  std::optional<Columns> columns;
  std::optional<std::string> version;
  std::optional<int> format;
};
/// readCloudInfo: the header (LAS / LAZ) or the first 64 KB (text)
[[nodiscard]] base::Result<Info> read_info(std::string_view name, Bytes file, const i18n::Catalog& words);

struct Cloud {
  std::string name, kind;
  std::uint64_t read = 0;
  std::size_t n = 0;
  V3 origin{};
  std::vector<float> pos;                           // 3n, relative to origin
  std::optional<std::vector<std::uint8_t>> rgb;     // 3n
  std::optional<std::vector<std::uint16_t>> inten;  // n
  std::optional<std::vector<std::uint8_t>> cls;     // n
  V3 min{}, max{};                                  // absolute, of every point read
  std::map<int, std::uint64_t> class_counts;        // class → points read
  std::optional<std::string> unit, z_unit;
  std::vector<std::string> notes;
};
inline constexpr std::uint64_t kMaxFileBytes = std::uint64_t{2000} * 1024 * 1024;
inline constexpr std::uint64_t kMaxTextBytes = std::uint64_t{400} * 1024 * 1024;
inline constexpr double kMaxBudget = 20e6;
/// loadCloud(file, info, { budget })
[[nodiscard]] base::Result<Cloud> load_cloud(std::string_view name, Bytes file, const Info& info, double budget,
                                             const i18n::Catalog& words);

/// colorize(cloud, mode, { color }): rgb (the file's own; height when it has none) | height | intensity | class |
/// single
[[nodiscard]] std::vector<std::uint8_t> colorize(const Cloud& c, std::string_view mode,
                                                 std::array<std::uint8_t, 3> color = {200, 200, 200});
[[nodiscard]] std::vector<std::string> color_modes(const Cloud& c);
/// sliceOf(cloud, { axis, at, thick }): the points within thick/2 of `at` (absolute file units)
[[nodiscard]] std::vector<std::uint32_t> slice_of(const Cloud& c, char axis, double at, double thick);
[[nodiscard]] V3 point_at(const Cloud& c, std::size_t i);

/// A bucket grid of points in plan (x, y), for the nearest point within a radius (snapping)
class PlanGrid {
 public:
  PlanGrid(const Cloud& c, const std::vector<std::uint32_t>* indices, double cell = 0);
  /// the point nearest (x, y) within r (absolute file units), or -1
  [[nodiscard]] std::int64_t nearest(double x, double y, double r) const;
  [[nodiscard]] double cell() const noexcept { return cell_; }

 private:
  const Cloud* c_;
  double cell_ = 1;
  std::unordered_map<double, std::vector<std::uint32_t>> map_;
};

struct ClassRow {
  int c = 0;
  std::string name;
  std::uint64_t n = 0;
};
struct Summary {
  std::uint64_t read = 0, kept = 0;
  V3 size{};
  std::vector<ClassRow> classes;
};
[[nodiscard]] Summary summary(const Cloud& c, const i18n::Catalog& words);
/// className: the ASPRS standard class name (0–18), or nothing
[[nodiscard]] std::optional<std::string> class_name(int c, const i18n::Catalog& words);

}  // namespace cc::cloud
