// SPDX-License-Identifier: Apache-2.0
#include "cc/engine/open.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "cc/base/error.hpp"
#include "cc/base/file.hpp"
#include "cc/base/js.hpp"
#include "cc/base/reader.hpp"
#include "cc/drawing/compile.hpp"
#include "cc/dwg/read.hpp"
#include "cc/dxf/read.hpp"

namespace cc::engine {

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

base::Result<std::unique_ptr<OpenDrawing>> parse(std::string name, std::span<const std::byte> bytes, OpenTimes times) {
  auto d = std::make_unique<OpenDrawing>();
  d->name = std::move(name);
  d->kind = kind_of(d->name, bytes);
  d->times = times;
  const auto t0 = Clock::now();
  if (d->kind == "dwg") {
    d->model = dwg::read_dwg(bytes);
    for (std::size_t i = 0; i < 6 && i < bytes.size(); ++i) {
      d->options.version_tag += static_cast<char>(std::to_integer<unsigned char>(bytes[i]));
    }
    if (d->model["report"]["error"].is_string()) {
      return base::fail(base::Errc::invalid, d->model["report"]["error"].as_string());
    }
  } else if (d->kind == "dxf") {
    auto m = dxf::read_dxf_model(dxf::decode_text(bytes));
    if (!m) {
      return std::unexpected(m.error());
    }
    d->model = std::move(*m);
  } else {
    return base::fail(base::Errc::unsupported, "This is not a DWG or DXF file.");
  }
  d->times.parse_ms = ms_since(t0);
  return d;
}

void finish(OpenDrawing& d) {
  const auto t0 = Clock::now();
  d.drawing = drawing::compile_drawing(d.model, d.options);
  d.times.compile_ms = ms_since(t0);
}

}  // namespace

std::string kind_of(std::string_view name, std::span<const std::byte> bytes) {
  const std::string_view head = base::as_text(bytes.first(std::min<std::size_t>(bytes.size(), 6)));
  if (head.starts_with("AC10")) {
    return "dwg";
  }
  const std::string lower = base::js_to_lower(name);
  if (lower.ends_with(".dxf")) {
    return "dxf";
  }
  const std::string_view text =
      base::js_trim_start(base::as_text(bytes.first(std::min<std::size_t>(bytes.size(), 64))));
  if (!text.empty() && text[0] >= '0' && text[0] <= '9') {
    return "dxf";  // a DXF starts with a group code
  }
  return {};
}

base::Result<std::unique_ptr<OpenDrawing>> open_bytes(std::string name, std::span<const std::byte> bytes) {
  auto d = parse(std::move(name), bytes, {});
  if (!d) {
    return d;
  }
  finish(**d);
  return d;
}

base::Result<std::unique_ptr<OpenDrawing>> open_file(const std::filesystem::path& path) {
  const auto t0 = Clock::now();
  auto bytes = base::read_file(path, kMaxFile);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  OpenTimes times;
  times.read_ms = ms_since(t0);
  auto d = parse(path.filename().string(), *bytes, times);
  if (!d) {
    return d;
  }
  // linked drawings: "<name>.dwg" beside the drawing, matched without regard to case
  std::error_code ec;
  const std::filesystem::path dir = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
  for (const base::Value& b : (*d)->model["blocks"].is_array() ? (*d)->model["blocks"].as_array() : base::Array{}) {
    if (!b["name"].is_string() || (static_cast<std::int64_t>(b["flags"].num_or(0)) & 4) == 0) {
      continue;
    }
    const std::string key = base::js_to_lower(b["name"].as_string());
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (base::js_to_lower(entry.path().filename().string()) != key + ".dwg") {
        continue;
      }
      const auto tx = Clock::now();
      const auto xb = base::read_file(entry.path(), kMaxFile);
      (*d)->times.read_ms += ms_since(tx);
      if (xb) {
        const auto tp = Clock::now();
        (*d)->xref_models.push_back(std::make_unique<base::Value>(dwg::read_dwg(*xb)));
        (*d)->times.parse_ms += ms_since(tp);
        (*d)->options.xrefs.push_back(drawing::Xref{key, (*d)->xref_models.back().get()});
      }
      break;
    }
  }
  finish(**d);
  return d;
}

}  // namespace cc::engine
