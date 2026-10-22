// SPDX-License-Identifier: Apache-2.0
// cc::skp — L3. cc-skp, CadCraft 0.5's own SketchUp (.skp) reader (lib/skp, docs/SKP-LIB-SPEC.md), ported: SketchUp
// 2021+ files (the header records, then a ZIP archive: vff.cpp) and classic files, SketchUp 3 … 2020 (one MFC object
// stream: legacy.cpp). A reader only. Format knowledge in cc-skp came from OpenSKP (MIT, © 2024-2026 Ahsan Mehmood;
// see this library's NOTICE), used as a map by cc-skp; this port follows cc-skp's JavaScript, not OpenSKP.
//
//   auto skp = cc::skp::read_skp(bytes);         // Result<Model, Error>
//   skp->defs[skp->root]                          // the model's own geometry; instances point at other defs
//
// The model is the JS model, typed (docs/SKP-LIB-SPEC.md "The model"): coordinates in inches, Z up; faces keep
// SketchUp's front side (outer loop counter-clockwise about the front normal, holes the other way); a face's null
// material is "default", kept raw (the importer decides; ModelCraft leaves such a face uncoloured); instance
// matrices are column-major 4×4 (m[12..14] = translation). `to_value` gives the JS object field for field, which is
// what the parity kit compares (`cadcraft-cli parity skp`).
//
// Errors: Error{code, message} with code not_skp | too_new | unsupported | damaged | too_big and the JS message
// (English, or the catalog's words when `ReadOptions::catalog` has the key). Untrusted input: every read is
// bounds-checked, the file ≤ 1 GB, inflated parts ≤ 512 MB each and ≤ 2 GB in all, ≤ 5 M vertices, nesting ≤ 64,
// object nesting in classic files ≤ 160, and nothing a file controls drives unbounded recursion.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/value.hpp"

namespace cc::i18n {
class Catalog;
}

namespace cc::skp {

struct Error {
  std::string code;  // not_skp too_new unsupported damaged too_big
  std::string message;
};
template <class T>
using Result = std::expected<T, Error>;

using P3 = std::array<double, 3>;
using P2 = std::array<double, 2>;
using Rgb = std::array<int, 3>;  // 0–255
using Bytes = std::vector<std::uint8_t>;

struct Texture {
  std::string name;
  std::string mime;
  Bytes bytes;
  double width = 1;  // inches per tile
  double height = 1;
};

struct Material {
  std::string name;
  std::optional<Rgb> color;
  double alpha = 1;  // 0–1
  std::optional<Texture> texture;
};

/// A SketchUp tag; index 0 is Layer0 ("Untagged").
struct Layer {
  std::string name;
  bool visible = true;
  std::optional<Rgb> color;
};

struct Face {
  std::vector<std::vector<P3>> loops;  // first = outer, the rest holes
  std::optional<std::uint32_t> front;  // material index; nullopt = default
  std::optional<std::uint32_t> back;
  std::optional<std::uint32_t> layer;              // nullopt only where a 2021+ file names a tag it lacks
  std::optional<std::vector<std::vector<P2>>> uv;  // front texture coordinates per loop vertex, when textured
  bool hidden = false;
};

/// A loose edge (no face uses it).
struct Edge {
  P3 a{};
  P3 b{};
  std::optional<std::uint32_t> layer;
  bool soft = false;
  bool smooth = false;
  bool hidden = false;
};

struct Instance {
  std::uint32_t def = 0;
  std::array<double, 16> m{};  // column-major 4×4
  std::string name;
  std::optional<std::uint32_t> layer;
  bool hidden = false;
  std::optional<std::uint32_t> material;  // painted on the instance: its default faces take it
};

enum class DefKind : std::uint8_t { model, component, group, image };

struct Definition {
  std::string name;
  DefKind kind = DefKind::component;
  std::optional<std::string> guid;
  std::vector<Face> faces;
  std::vector<Edge> edges;              // loose edges only
  std::vector<std::array<P3, 2>> soft;  // face edges marked soft or smooth
  std::vector<Instance> insts;
};

struct Thumbnail {
  std::string mime;
  Bytes bytes;
};

struct Counts {
  std::size_t defs = 0, faces = 0, edges = 0, insts = 0, materials = 0, layers = 0;
};

struct Model {
  bool vff = false;     // container: "vff" (2021+) or "legacy"
  std::string version;  // "25.0.575"
  double major = 0;     // 3 … 26 (2013 → 13); a JS number, as the header says it
  std::vector<Material> materials;
  std::vector<Layer> layers;
  std::vector<Definition> defs;
  std::uint32_t root = 0;
  std::optional<Thumbnail> thumbnail;
  std::optional<std::string> length_unit;  // in ft mm cm m: the display unit (coordinates stay inches)
  std::vector<std::string> notes;          // one line per kind of thing skipped, with counts
  Counts counts;
};

struct Progress {
  std::string_view stage;  // unzip, read (2021+); legacy
  std::uint64_t done = 0;
  std::uint64_t total = 0;
};

struct ReadOptions {
  /// The page's words for the ccskp.* keys it has (else cc-skp's English).
  const i18n::Catalog* catalog = nullptr;
  std::function<void(const Progress&)> on_progress;
  /// Polled between phases; true = stop (the read returns Error{"aborted", …}).
  std::function<bool()> cancelled;
};

/// The header records: the version, its major and where the body starts, and the container.
struct Sniff {
  std::string version;
  double major = 0;
  std::size_t body_start = 0;
  bool vff = false;
};
[[nodiscard]] Result<Sniff> sniff_skp(std::span<const std::uint8_t> bytes, const ReadOptions& opts = {});

/// readSkp(bytes).
[[nodiscard]] Result<Model> read_skp(std::span<const std::uint8_t> bytes, const ReadOptions& opts = {});

/// The JS model object (field names, presence and order as readSkp returns it), bytes as base::Value bytes.
[[nodiscard]] base::Value to_value(const Model& m);

}  // namespace cc::skp
