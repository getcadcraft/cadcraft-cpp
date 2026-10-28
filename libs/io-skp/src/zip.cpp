// SPDX-License-Identifier: Apache-2.0
// lib/skp zip.js, ported (the PKWARE APPNOTE layout; deflate through zlib). See zip.hpp.
#include "zip.hpp"

#include <limits>
#include <utility>

#include <zlib.h>

namespace cc::skp::detail {

namespace {

constexpr std::uint32_t kSigLocal = 0x04034b50;
constexpr std::uint32_t kSigCentral = 0x02014b50;
constexpr std::uint32_t kSigEnd = 0x06054b50;
constexpr std::uint32_t kSigEnd64Locator = 0x07064b50;

std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t p) noexcept {
  return static_cast<std::uint16_t>(b[p] | (b[p + 1] << 8U));
}
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t p) noexcept {
  return static_cast<std::uint32_t>(b[p]) | (static_cast<std::uint32_t>(b[p + 1]) << 8U) |
         (static_cast<std::uint32_t>(b[p + 2]) << 16U) | (static_cast<std::uint32_t>(b[p + 3]) << 24U);
}

/// Entry names are UTF-8 even without the UTF-8 flag (SketchUp never sets it); else Latin-1.
std::string decode_name(std::span<const std::uint8_t> raw) {
  if (utf8_valid(raw)) {
    std::size_t at = 0;
    if (raw.size() >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf) {
      at = 3;  // TextDecoder drops a BOM
    }
    return {raw.begin() + static_cast<std::ptrdiff_t>(at), raw.end()};
  }
  return latin1(raw);
}

/// Raw deflate → bytes, as zip.js inflateRaw through DecompressionStream('deflate-raw'): more than `cap` bytes out =
/// too_big; a stream that does not end, or bytes after its end, = damaged.
Bytes inflate_raw(std::span<const std::uint8_t> data, std::uint64_t cap, const i18n::Catalog* cat) {
  z_stream z{};
  if (inflateInit2_(&z, -15, ZLIB_VERSION, static_cast<int>(sizeof(z_stream))) != Z_OK) {
    damaged(cat, "ccskp.damaged.inflate");
  }
  const ScopeExit guard([&z] { inflateEnd(&z); });
  Bytes out;
  // The directory's size is the answer of a sound entry: one buffer, and one byte more to see a lie. Deflate gives at
  // most 1032 bytes per input byte, so a small entry that claims a big size is not given that size up front (the
  // fuzzer's 2 kB files that claimed hundreds of MB); the buffer grows if the stream does give more.
  const std::uint64_t most = (static_cast<std::uint64_t>(data.size()) * 1032U) + 1024U;
  out.resize(static_cast<std::size_t>(std::min(cap + 1, most)));
  std::size_t in_at = 0;
  std::size_t out_at = 0;
  for (;;) {
    if (out_at == out.size()) {
      if (out.size() > cap) {
        fail("too_big", msg(cat, "ccskp.too_big.part_unpacks",
                            {{"mb", num_text(std::round(static_cast<double>(cap) / 1048576.0))}}));
      }
      out.resize(static_cast<std::size_t>(std::min<std::uint64_t>(cap + 1, out.size() * 2ULL)));
    }
    const std::size_t in_n = std::min<std::size_t>(data.size() - in_at, std::numeric_limits<uInt>::max());
    const std::size_t out_n = std::min<std::size_t>(out.size() - out_at, std::numeric_limits<uInt>::max());
    z.next_in =
        const_cast<Bytef*>(data.subspan(in_at).data());  // NOLINT(cppcoreguidelines-pro-type-const-cast) zlib's API
    z.avail_in = static_cast<uInt>(in_n);
    z.next_out = std::span(out).subspan(out_at).data();
    z.avail_out = static_cast<uInt>(out_n);
    const int rc = inflate(&z, Z_NO_FLUSH);
    in_at += in_n - z.avail_in;
    out_at += out_n - z.avail_out;
    if (out_at > cap) {
      fail("too_big", msg(cat, "ccskp.too_big.part_unpacks",
                          {{"mb", num_text(std::round(static_cast<double>(cap) / 1048576.0))}}));
    }
    if (rc == Z_STREAM_END) {
      if (in_at != data.size()) {
        damaged(cat, "ccskp.damaged.inflate");  // trailing junk
      }
      break;
    }
    if (rc == Z_BUF_ERROR && z.avail_out != 0) {
      damaged(cat, "ccskp.damaged.inflate");  // the input ended first
    }
    if (rc != Z_OK && rc != Z_BUF_ERROR) {
      damaged(cat, "ccskp.damaged.inflate");
    }
    if (in_at == data.size() && z.avail_out != 0) {
      damaged(cat, "ccskp.damaged.inflate");
    }
  }
  out.resize(out_at);
  return out;
}

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
  uLong c = ::crc32(0L, Z_NULL, 0);
  std::size_t at = 0;
  while (at < bytes.size()) {  // zlib takes a uInt length
    const std::size_t n = std::min<std::size_t>(bytes.size() - at, std::numeric_limits<uInt>::max());
    c = ::crc32(c, bytes.subspan(at).data(), static_cast<uInt>(n));
    at += n;
  }
  return static_cast<std::uint32_t>(c);
}

Zip::Zip(std::span<const std::uint8_t> bytes, std::size_t start, const i18n::Catalog* cat)
    : b_(bytes), start_(start), cat_(cat) {
  const std::size_t len = bytes.size();
  // End of central directory: 22 bytes + up to 65535 of comment, searched backwards.
  std::int64_t eocd = -1;
  if (len >= 22) {
    const std::int64_t lowest =
        std::max<std::int64_t>(static_cast<std::int64_t>(start), static_cast<std::int64_t>(len) - 22 - 65535);
    for (std::int64_t i = static_cast<std::int64_t>(len) - 22; i >= lowest; --i) {
      const auto p = static_cast<std::size_t>(i);
      if (u32(bytes, p) == kSigEnd && p + 22 + u16(bytes, p + 20) <= len) {
        eocd = i;
        break;
      }
    }
  }
  if (eocd < 0) {
    damaged(cat, "ccskp.damaged.no_end");
  }
  const auto e = static_cast<std::size_t>(eocd);
  if (e >= 20 && u32(bytes, e - 20) == kSigEnd64Locator) {
    fail("unsupported", msg(cat, "ccskp.unsupported.zip64"));
  }
  const std::uint16_t disk = u16(bytes, e + 4);
  const std::uint16_t count = u16(bytes, e + 10);
  const std::uint32_t cd_size = u32(bytes, e + 12);
  const std::uint32_t cd_offset = u32(bytes, e + 16);
  if (count == 0xffff || cd_size == 0xffffffffU || cd_offset == 0xffffffffU) {
    fail("unsupported", msg(cat, "ccskp.unsupported.zip64"));
  }
  if (disk != 0) {
    fail("unsupported", msg(cat, "ccskp.unsupported.multipart"));
  }
  // where the directory is versus where it says it is = the archive's offset inside the file
  const std::int64_t cd_start = eocd - static_cast<std::int64_t>(cd_size);
  if (std::cmp_less(cd_start, start)) {
    damaged(cat, "ccskp.damaged.dir_outside");
  }
  const std::int64_t base = cd_start - static_cast<std::int64_t>(cd_offset);
  if (base < 0) {
    damaged(cat, "ccskp.damaged.dir_outside");
  }

  auto p = static_cast<std::size_t>(cd_start);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (p + 46 > e || u32(bytes, p) != kSigCentral) {
      damaged(cat, "ccskp.damaged.dir_broken");
    }
    Entry en;
    en.flags = u16(bytes, p + 8);
    en.method = u16(bytes, p + 10);
    en.crc = u32(bytes, p + 16);
    en.csize = u32(bytes, p + 20);
    en.usize = u32(bytes, p + 24);
    const std::size_t name_len = u16(bytes, p + 28);
    const std::size_t extra_len = u16(bytes, p + 30);
    const std::size_t comment_len = u16(bytes, p + 32);
    const std::uint32_t local = u32(bytes, p + 42);
    if (p + 46 + name_len + extra_len + comment_len > e) {
      damaged(cat, "ccskp.damaged.dir_broken");
    }
    en.name = decode_name(bytes.subspan(p + 46, name_len));
    if (en.csize == 0xffffffffU || en.usize == 0xffffffffU || local == 0xffffffffU) {
      fail("unsupported", msg(cat, "ccskp.unsupported.zip64"));
    }
    en.local = static_cast<std::uint64_t>(base) + local;
    if (find(en.name) == nullptr) {
      names_.push_back(en.name);
      entries_.push_back(std::move(en));
    }
    p += 46 + name_len + extra_len + comment_len;
  }
}

const Zip::Entry* Zip::find(std::string_view name) const {
  for (const Entry& e : entries_) {
    if (e.name == name) {
      return &e;
    }
  }
  return nullptr;
}

bool Zip::has(std::string_view name) const {
  return find(name) != nullptr;
}

Bytes Zip::read(std::string_view name, std::uint64_t& used) const {
  const Entry* e = find(name);
  const std::string nm(name);
  if (e == nullptr) {
    damaged(cat_, "ccskp.damaged.entry_missing", {{"name", nm}});
  }
  if ((e->flags & 1U) != 0) {
    fail("unsupported", msg(cat_, "ccskp.unsupported.encrypted"));
  }
  if (e->usize > Limits::entry_bytes) {
    fail("too_big", msg(cat_, "ccskp.too_big.part",
                        {{"mb", num_text(std::round(static_cast<double>(Limits::entry_bytes) / 1048576.0))}}));
  }
  if (used + e->usize > Limits::inflated_bytes) {
    fail("too_big", msg(cat_, "ccskp.too_big.unpacked"));
  }
  const std::uint64_t lh = e->local;
  const std::size_t len = b_.size();
  if (lh < start_ || lh + 30 > len || u32(b_, static_cast<std::size_t>(lh)) != kSigLocal) {
    damaged(cat_, "ccskp.damaged.entry_outside", {{"name", nm}});
  }
  const auto l = static_cast<std::size_t>(lh);
  const std::uint64_t data_start = lh + 30 + u16(b_, l + 26) + u16(b_, l + 28);
  if (data_start + e->csize > len) {
    damaged(cat_, "ccskp.damaged.entry_past_end", {{"name", nm}});
  }
  const auto raw = b_.subspan(static_cast<std::size_t>(data_start), e->csize);
  Bytes out;
  if (e->method == 0) {
    if (e->csize != e->usize) {
      damaged(cat_, "ccskp.damaged.entry_size", {{"name", nm}});
    }
    out.assign(raw.begin(), raw.end());
  } else if (e->method == 8) {
    out = inflate_raw(raw, std::min<std::uint64_t>(e->usize, Limits::entry_bytes), cat_);
  } else {
    fail("unsupported", msg(cat_, "ccskp.unsupported.method", {{"name", nm}, {"method", std::to_string(e->method)}}));
  }
  if (out.size() != e->usize) {
    damaged(cat_, "ccskp.damaged.entry_unpacked", {{"name", nm}});
  }
  if (crc32(out) != e->crc) {
    damaged(cat_, "ccskp.damaged.entry_crc", {{"name", nm}});
  }
  used += out.size();
  return out;
}

}  // namespace cc::skp::detail
