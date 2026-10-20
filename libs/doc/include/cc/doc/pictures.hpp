// SPDX-License-Identifier: Apache-2.0
// cc::doc — pictures.js (CadCraft 0.5 WL.14 / WL.13): pictures in drawings. A picture is an unnamed block "*I<n>"
// placed by an insert: its frame (a closed polyline, the clip boundary or the whole picture) carries `pic = { file, x,
// y, ux, uy, vx, vy, w, h, clipped?, fade? }` and the file's name rides along as text (`picName`). The files live with
// the drawing while it is open (Doc::pictures, key = the file's base name, lower case); they are not undoable. File
// data is untrusted: only PNG and JPEG, sniffed by content, with size limits.
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/image/picture.hpp"

namespace cc::doc {

class Doc;

/// pictureKey(file): the file's base name, lower case ("C:\\Logos\\Logo.PNG" → "logo.png").
[[nodiscard]] std::string picture_key(std::string_view file);

struct Sniffed {
  std::string mime;  // "image/png" | "image/jpeg"
  double w = 0, h = 0;
};
/// sniffPicture(bytes): a PNG or a JPEG we can read, by its content; nothing otherwise (or past 100 Mpixels).
[[nodiscard]] std::optional<Sniffed> sniff_picture(std::span<const std::uint8_t> bytes);
using image::jpeg_info;

/// pictureFrame(block): the frame entity of a picture block (the poly carrying `pic`), or nullptr.
[[nodiscard]] const base::Value* picture_frame(const base::Value& block);
/// freePictureName(doc): a free "*I<n>" block name.
[[nodiscard]] std::string free_picture_name(const Doc& doc);

/// A picture file the drawing keeps while it is open.
struct PictureFile {
  std::string name;  // the file's base name
  std::vector<std::uint8_t> bytes;
  std::string mime;
  double w = 0, h = 0;
};
/// addPicture(doc, name, bytes): the file kept with the drawing, or the language key of why not ("pic.too_big",
/// "pic.not_picture"; the caller fills in {file}).
[[nodiscard]] std::expected<const PictureFile*, std::string> add_picture(Doc& doc, std::string_view name,
                                                                         std::span<const std::uint8_t> bytes);
/// findPicture(doc, file): the stored file — the exact name, else one whose name contains it (exports add a prefix).
[[nodiscard]] const PictureFile* find_picture(const Doc& doc, std::string_view file);

}  // namespace cc::doc
