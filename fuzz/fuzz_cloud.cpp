// SPDX-License-Identifier: Apache-2.0
// A point cloud file (pointcloud.js readCloudInfo, loadCloud; las.js readLasHeader): any bytes, as a LAS file and as a
// text cloud, give a cloud or a reason — never a crash, a hang or a sanitizer report. Colours, slices and the snapping
// grid run on what comes back.
#include <cstddef>
#include <cstdint>
#include <span>

#include "cc/cloud/cloud.hpp"
#include "cc/i18n/i18n.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > (std::size_t{1} << 20)) {
    return 0;
  }
  static const cc::i18n::Catalog kWords;
  const auto bytes = std::as_bytes(std::span(data, size));
  for (const char* name : {"f.las", "f.xyz"}) {
    const auto info = cc::cloud::read_info(name, bytes, kWords);
    if (!info) {
      continue;
    }
    const auto c = cc::cloud::load_cloud(name, bytes, *info, 1000, kWords);
    if (!c) {
      continue;
    }
    for (const char* m : {"rgb", "height", "intensity", "class"}) {
      (void)cc::cloud::colorize(*c, m);
    }
    const auto cut = cc::cloud::slice_of(*c, 'z', c->min[2] + 1, 0.5);
    (void)cc::cloud::PlanGrid(*c, &cut).nearest(c->min[0], c->min[1], 1);
    (void)cc::cloud::summary(*c, kWords);
  }
  return 0;
}
