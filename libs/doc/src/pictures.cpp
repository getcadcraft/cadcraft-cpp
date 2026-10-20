// SPDX-License-Identifier: Apache-2.0
// pictures.js (CadCraft 0.5) in C++: the file side — what a picture file is, and the files a drawing keeps.
#include "cc/doc/pictures.hpp"

#include <algorithm>
#include <string>

#include "cc/base/js.hpp"
#include "cc/doc/doc.hpp"

namespace cc::doc {

namespace {

constexpr std::size_t kMaxBytes = std::size_t{64} * 1024 * 1024;
constexpr double kMaxPixels = 100e6;

std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t i) {
  return (static_cast<std::uint32_t>(b[i]) << 24U) | (static_cast<std::uint32_t>(b[i + 1]) << 16U) |
         (static_cast<std::uint32_t>(b[i + 2]) << 8U) | static_cast<std::uint32_t>(b[i + 3]);
}

/// the stem without its extension: k.replace(/\.[a-z0-9]+$/, '')
std::string stem_of(const std::string& k) {
  const std::size_t dot = k.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= k.size()) {
    return k;
  }
  const bool ext = std::ranges::all_of(k.substr(dot + 1),
                                       [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'); });
  return ext ? k.substr(0, dot) : k;
}

}  // namespace

std::string picture_key(std::string_view file) {
  const std::size_t sl = file.find_last_of("\\/");
  return base::js_to_lower(sl == std::string_view::npos ? file : file.substr(sl + 1));
}

std::optional<Sniffed> sniff_picture(std::span<const std::uint8_t> b) {
  if (b.size() > 24 && b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4e && b[3] == 0x47) {
    const double w = be32(b, 16);
    const double h = be32(b, 20);
    if (w > 0 && h > 0 && w * h <= kMaxPixels) {
      return Sniffed{.mime = "image/png", .w = w, .h = h};
    }
    return std::nullopt;
  }
  if (b.size() > 4 && b[0] == 0xff && b[1] == 0xd8) {
    const auto s = jpeg_info(b);
    if (s && s->w * s->h <= kMaxPixels) {
      return Sniffed{.mime = "image/jpeg", .w = s->w, .h = s->h};
    }
  }
  return std::nullopt;
}

const base::Value* picture_frame(const base::Value& block) {
  const base::Value* ents = block.find("entities");
  if (ents == nullptr || !ents->is_array()) {
    return nullptr;
  }
  for (const base::Value& e : ents->as_array()) {
    if (e.get("type") == base::Value("poly") && e.get("pic").truthy()) {
      return &e;
    }
  }
  return nullptr;
}

std::string free_picture_name(const Doc& doc) {
  int n = 1;
  while (doc.blocks().get("*I" + std::to_string(n)) != nullptr) {
    ++n;
  }
  return "*I" + std::to_string(n);
}

std::expected<const PictureFile*, std::string> add_picture(Doc& doc, std::string_view name,
                                                           std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxBytes) {
    return std::unexpected(std::string("pic.too_big"));
  }
  const auto s = sniff_picture(bytes);
  if (!s) {
    return std::unexpected(std::string("pic.not_picture"));
  }
  const std::size_t sl = name.find_last_of("\\/");
  PictureFile rec{.name = std::string(sl == std::string_view::npos ? name : name.substr(sl + 1)),
                  .bytes = std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
                  .mime = s->mime,
                  .w = s->w,
                  .h = s->h};
  const std::string key = picture_key(name);
  ++doc.picture_version;
  for (auto& [k, v] : doc.pictures) {  // a Map keeps an existing key's place
    if (k == key) {
      v = std::move(rec);
      return &v;
    }
  }
  doc.pictures.emplace_back(key, std::move(rec));
  return &doc.pictures.back().second;
}

const PictureFile* find_picture(const Doc& doc, std::string_view file) {
  const std::string k = picture_key(file);
  if (k.empty()) {
    return nullptr;
  }
  for (const auto& [key, rec] : doc.pictures) {
    if (key == k) {
      return &rec;
    }
  }
  const std::string stem = stem_of(k);
  for (const auto& [key, rec] : doc.pictures) {
    if (base::utf16_length(stem) >= 3 && key.contains(stem)) {
      return &rec;
    }
  }
  return nullptr;
}

}  // namespace cc::doc
