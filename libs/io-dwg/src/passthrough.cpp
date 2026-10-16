// SPDX-License-Identifier: Apache-2.0
// Port of the reading half of cc-dwg passthrough.js: captureRaw — the undecoded bits of an object after its common
// part, its handle references (code + absolute handle) and, for entities, the proxy graphics (ODA spec 2.13, 20.1,
// 20.2, 20.4.90). The writer half arrives with the DWG writer (M4).
#include <algorithm>

#include "cc/base/checked.hpp"
#include "parts.hpp"
#include "record.hpp"

namespace cc::dwg {

using base::Array;

std::optional<Value> capture_raw(const Rec& rec, int ver) {
  if (rec.buf.data() == nullptr || rec.endbit < rec.data_start) {
    return std::nullopt;
  }
  Value raw = Value::object();
  raw.set("ver", ver);
  raw.set("n", static_cast<double>(rec.endbit - rec.data_start));
  raw.set("data", base::base64(slice_bits_of(rec.buf, rec.data_start, rec.endbit)));
  Array hs;
  Bits hr(rec.buf, ver, nullptr, rec.h_pos, rec.obj_end);
  while (hr.left() >= 8 && hs.size() < 1000000) {
    try {
      const std::int64_t v = hr.H(rec.handle);
      hs.emplace_back(Array{hr.hcode, base::hex_upper(static_cast<std::uint64_t>(std::max<std::int64_t>(0, v)))});
    } catch (const std::exception&) {
      break;
    }
  }
  raw.set("h", std::move(hs));
  if (rec.gfx_pos && rec.graphics > 0) {
    const std::uint64_t from = *rec.gfx_pos;
    raw.set("gfx",
            base::base64(slice_bits_of(rec.buf, from, from + base::clamp_cast<std::uint64_t>(rec.graphics * 8))));
  }
  if (rec.ds != 0) {
    raw.set("ds", 1);
  }
  return raw;
}

}  // namespace cc::dwg
