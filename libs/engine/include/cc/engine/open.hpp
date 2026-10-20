// SPDX-License-Identifier: Apache-2.0
// cc::engine — L5. Opening a drawing: the bytes → its model (DWG through cc-dwg, DXF through dxf-model, told apart by
// their first bytes, as the 0.5 apps do) → the draw lists. The linked drawings it uses ("<xref>.dwg" beside it, any
// case) are opened with it on desktop.
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"
#include "cc/drawing/compile.hpp"

namespace cc::engine {

struct OpenTimes {
  double read_ms = 0;     // the file (and its xrefs) from disk
  double parse_ms = 0;    // bytes → model
  double compile_ms = 0;  // model → draw lists
};

struct OpenDrawing {
  std::string name;    // the file name
  std::string kind;    // "dwg" | "dxf" | "sketch" (a SolidWorks sketch export, .json: its text in `text`)
  std::string sketch;  // a sketch export: the sketch that opens (empty: not chosen yet)
  base::Value model;
  std::string text;  // a DXF: its text as read (UTF-8, else Windows-1252), for CadCraft's own reader
  std::vector<std::unique_ptr<base::Value>> xref_models;  // stable addresses: the compile options point at them
  drawing::CompileOptions options;
  drawing::Drawing drawing;
  OpenTimes times;
};

/// The largest file opened (2 GiB).
inline constexpr std::uint64_t kMaxFile = std::uint64_t{1} << 31U;

/// Opens a file from disk, with its xrefs.
[[nodiscard]] base::Result<std::unique_ptr<OpenDrawing>> open_file(const std::filesystem::path& path);
/// Opens bytes already in memory (the web's file picker, a drop): no xrefs.
[[nodiscard]] base::Result<std::unique_ptr<OpenDrawing>> open_bytes(std::string name, std::span<const std::byte> bytes);

/// "dwg" when the bytes start like a DWG (AC10…), else "sketch" when the name ends in .json (cadcraft.js: a
/// SolidWorks sketch export), else "dxf" when the name ends in .dxf or the text starts with a group code, else empty.
[[nodiscard]] std::string kind_of(std::string_view name, std::span<const std::byte> bytes);

}  // namespace cc::engine
