// SPDX-License-Identifier: Apache-2.0
// cadcraft-cli — the headless entry point (AGENTS.md section 9).
//
//   cadcraft-cli info <file.dwg>                 version, counts, notes
//   cadcraft-cli convert --json <file.dwg>       the drawing model as JSON (the JS readDwg model's shape)
//   … info / convert take --isolated: parse in the sandboxed helper (cadcraft-importer) instead of in this process
//   cadcraft-cli parity <engine> <file>          the parity kit's contract (tools/fork-parity in the site repo):
//                                                dwg, dxf → the model as JSON; other engines → exit 3 (not ported yet)
//                                                (dxf is this fork's own check against BimCraft's readDxfModel)
//   cadcraft-cli bench <engine> <file> [--runs N]  {"open_ms":[…],"parse_ms":[…],"first_frame_ms":null,"runtime":"…"}
//   cadcraft-cli compile <file.dwg|dxf> --json   what the 2D view draws (BimCraft compileDrawing's result as plain
//                                                JSON: tools/parity/compile.mjs, forks/tools/cdiff.mjs --cli)
//   cadcraft-cli compile <file> --bench N        best of N in-process compiles (the read is not timed)
//   cadcraft-cli import <file.dwg>               CadCraft's drawing (dwg-import.js importDwg) as JSON: what the 0.5
//                                                app's doc.load() takes (tools/parity/import.mjs); notes in English
//   cadcraft-cli doc <file.dwg>                  the document after load (doc.js) as tools/parity/doc.mjs compares it
//   cadcraft-cli dwg-write <in.dwg> <version> <out.dwg> [--created <ms>] [--seeded] [--bench <runs>]
//                                                read a DWG and write it in a version (cc-dwg writeDwg); prints the
//                                                report. --created fixes the date, --seeded the GUIDs' random bytes
//                                                (the parity harness gives the JS writer the same:
//                                                tools/parity/dwg-write.mjs)
//   cadcraft-cli script <scripts.json>          command scripts (libs/cmd run_script) on a fresh drawing: what was
//                                                printed and prompted, the model, undo labels
//                                                (tools/parity/commands.mjs)
//   cadcraft-cli dxf-save <in.dwg> r12|2000 <out.dxf> [--bench <runs>]
//                                                the drawing saved as DXF R12 (dxf.js) or R2000 (dxf2000.js); prints
//                                                the report (tools/parity/dxf-save.mjs)
//   cadcraft-cli dwg-save <in.dwg> <version> <out.dwg> [--created <ms>] [--seeded] [--model] [--bench <runs>]
//                                                open a DWG as CadCraft does (import + document) and save it as a DWG
//                                                of that version (dwg-save.js); prints the report, or with --model the
//                                                writer model (tools/parity/dwg-save.mjs)
//   cadcraft-cli version | help
//
// Exit codes: 0 ok, 1 the file was refused ({"$error":{code,message}} on stdout for parity/bench), 2 usage, 3 an
// engine this build does not have yet.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "cc/base/file.hpp"
#include "cc/base/js.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/base/value.hpp"
#include "cc/cad/import.hpp"
#include "cc/cad/read_dxf.hpp"
#include "cc/cloud/cloud.hpp"
#include "cc/cmd/script.hpp"
#include "cc/doc/doc.hpp"
#include "cc/docview/walk.hpp"
#include "cc/drawing/compile.hpp"
#include "cc/dwg/read.hpp"
#include "cc/dwg/write.hpp"
#include "cc/dxf/read.hpp"
#include "cc/engine/script_save.hpp"
#include "m3d_cli.hpp"
#if defined(CC_HAVE_IMAGE)
#include "cc/image/png.hpp"
#endif
#include "cc/i18n/i18n.hpp"
#include "cc/importer/importer.hpp"
#include "cc/plot/plot.hpp"
#include "cc/save/dwg.hpp"
#include "cc/save/dxf.hpp"

namespace {

constexpr std::string_view kVersion = "0.0.0";
constexpr std::uint64_t kMaxFile = std::uint64_t{1} << 31U;  // 2 GiB

void usage(std::FILE* out) {
  std::print(
      out,
      "usage: cadcraft-cli <command> [options]\n"
      "  info [--isolated] <file.dwg>        version, counts and notes\n"
      "  convert [--isolated] --json <file>  the drawing model as JSON\n"
      "                                      (--isolated: parse in the sandboxed import helper)\n"
      "  parity <engine> <file>              engine output for the parity kit (engines: dwg, dxf)\n"
      "  bench <engine> <file> [--runs N]    in-process timings for the parity kit\n"
      "  compile <file> --json | --bench N   the 2D view's draw lists (DWG or DXF)\n"
      "  import <file.dwg>                   CadCraft's drawing (what the app's document loads) as JSON\n"
      "  doc <file.dwg>                      the document after loading that drawing, as JSON\n"
      "  dwg-write <in.dwg> <version> <out.dwg> [--created <ms>] [--seeded] [--bench <runs>]\n"
      "                                      write the drawing as a DWG of that version; prints the report\n"
      "  dwg-save <in.dwg> <version> <out.dwg> [--created <ms>] [--seeded] [--model] [--bench <runs>]\n"
      "  dxf-save <in.dwg> r12|2000 <out.dxf> [--bench <runs>]\n"
      "  walk <in.dwg>                         what the drawing draws (walk.js events of each space, JSON)\n"
      "  script <scripts.json>                 command scripts on a fresh drawing (one JSON line each)\n"
      "  plot <in.dwg> --out <out.pdf> [--sheet NAME | --model] [--pen MM] [--colour]\n"
      "                                        a PDF of a sheet, of the model at its extents, or of every sheet\n"
      "  plot-parity <in.dwg>                  each sheet and the model as uncompressed PDFs (the parity kit)\n"
      "                                      open a DWG as CadCraft does and save it as a DWG of that version\n"
      "  cloud <file> [--budget N]           a point cloud (LAS, LAZ, XYZ, PTS) as CadCraft reads it\n"
      "  manifold <cases.json>               SolidOps parity cases (tools/parity/manifold.mjs)\n"
      "  version                             print the version\n");
}

// The last word on a failure: never throws (std::print could).
void say_failure(const char* what) noexcept {
  if (std::fputs("cadcraft-cli: ", stderr) == EOF || std::fputs(what, stderr) == EOF) {
    return;
  }
  (void)std::fputc('\n', stderr);
}

void print_error_json(std::string_view code, std::string_view message) {
  cc::base::Value e = cc::base::make_object(
      {{"$error", cc::base::make_object({{"code", std::string(code)}, {"message", std::string(message)}})}});
  std::string s = cc::base::to_json(e);
  s += '\n';
  (void)std::fwrite(s.data(), 1, s.size(), stdout);
}

bool engine_known(std::string_view e) {
  return e == "dwg" || e == "dxf" || e == "ifc-step" || e == "ifc-geom" || e == "skp";
}
bool engine_ported(std::string_view e) {
  return e == "dwg" || e == "dxf";
}

/// The engine's model of the file's bytes, as the 0.5 app builds it (DXF: BimCraft's decode, then readDxfModel).
cc::base::Result<cc::base::Value> run_engine(std::string_view engine, std::span<const std::byte> bytes) {
  if (engine == "dxf") {
    return cc::dxf::read_dxf_model(cc::dxf::decode_text(bytes));
  }
  return cc::dwg::read_dwg(bytes);
}
std::string_view errc_name(cc::base::Errc c) {
  switch (c) {
    case cc::base::Errc::out_of_range: return "out_of_range";
    case cc::base::Errc::overflow: return "overflow";
    case cc::base::Errc::invalid: return "invalid";
    case cc::base::Errc::too_big: return "too_big";
    case cc::base::Errc::unsupported: return "unsupported";
    case cc::base::Errc::io: return "io";
  }
  return "invalid";
}

int cmd_parity(std::string_view engine, const std::filesystem::path& file) {
  if (!engine_ported(engine)) {
    print_error_json("unsupported", engine_known(engine) ? "engine not ported yet" : "unknown engine");
    return 3;
  }
  const auto bytes = cc::base::read_file(file, kMaxFile);
  if (!bytes) {
    print_error_json("io", bytes.error().message);
    return 1;
  }
  auto parsed = run_engine(engine, *bytes);
  if (!parsed) {
    print_error_json(errc_name(parsed.error().code), parsed.error().message);
    return 1;
  }
  cc::base::Value model = std::move(*parsed);
  if (cc::base::Value* rep = model.find("report")) {
    rep->erase("ms");
  }
  std::string out;
  out.reserve(bytes->size() * 8);
  cc::base::write_json(model, out);
  out += '\n';
  (void)std::fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}

/// assets/ (lang/): APP_ASSETS_DIR, else beside this program, else the source tree.
std::filesystem::path assets_dir(std::string_view argv0) {
  if (const auto env = cc::base::env_var("APP_ASSETS_DIR"); env && !env->empty()) {
    return *env;
  }
  std::error_code ec;
  auto beside = std::filesystem::canonical(std::filesystem::path(argv0), ec).parent_path() / "assets";
  if (!ec && std::filesystem::exists(beside / "lang/en.json", ec)) {
    return beside;
  }
  return CC_ASSETS_DIR;
}

int cmd_import(const std::filesystem::path& file, std::string_view argv0, bool as_doc = false) {
  const auto bytes = cc::base::read_file(file, kMaxFile);
  if (!bytes) {
    print_error_json("io", bytes.error().message);
    return 1;
  }
  const auto lang = cc::base::read_file(assets_dir(argv0) / "lang/en.json", std::uint64_t{1} << 26U);
  if (!lang) {
    print_error_json("io", "the language file is missing: " + lang.error().message);
    return 1;
  }
  auto words = cc::i18n::Catalog::parse("en", cc::base::as_text(*lang));
  if (!words) {
    print_error_json("invalid", words.error().message);
    return 1;
  }
  cc::base::Value drawing;
  std::string ext = file.extension().string();
  std::ranges::transform(ext, ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (ext == ".dxf") {  // cadcraft.js readDrawing: the text (UTF-8, else Windows-1252), then dxf.js readDxf
    auto r = cc::cad::read_dxf(cc::dxf::decode_text(*bytes), *words,
                               [](std::string_view t) { return cc::dxf::read_dxf_model(t); });
    if (!r) {
      print_error_json("invalid", r.error().message);
      return 1;
    }
    drawing = std::move(*r);
  } else {
    const cc::base::Value model = cc::dwg::read_dwg(*bytes);
    if (!model["blocks"].is_array() || model["blocks"].as_array().empty()) {
      print_error_json("invalid", "unreadable");
      return 1;
    }
    const std::span<const std::byte> head =
        std::span<const std::byte>(*bytes).first(std::min<std::size_t>(6, bytes->size()));
    drawing = cc::cad::import_dwg(model, cc::base::as_text(head), *words);
  }
  std::string out;
  if (as_doc) {
    cc::doc::Doc d;
    d.load(std::move(drawing), *words);
    cc::base::write_json(d.dump(), out);
  } else {
    cc::base::write_json(drawing, out);
  }
  out += '\n';
  (void)std::fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}

int cmd_dwg_write(const std::filesystem::path& in, const std::string& version, const std::filesystem::path& out,
                  std::optional<double> created, bool seeded, int bench) {
  const auto bytes = cc::base::read_file(in, kMaxFile);
  if (!bytes) {
    print_error_json("io", bytes.error().message);
    return 1;
  }
  const cc::base::Value model = cc::dwg::read_dwg(*bytes);
  cc::dwg::WriteOptions o;
  o.version = version;
  o.created_ms = created;
  if (seeded) {
    // the harness's generator: s = s * 1103515245 + 12345 (32 bits), byte = s >> 24, s = 1 at the start of a write
    auto s = std::make_shared<std::uint32_t>(1);
    o.random = [s](std::span<std::uint8_t> b) {
      for (std::uint8_t& x : b) {
        *s = *s * 1103515245U + 12345U;
        x = static_cast<std::uint8_t>(*s >> 24U);
      }
    };
  }
  cc::dwg::WriteResult r;
  cc::base::Array write_ms;
  try {
    for (int i = 0; i < std::max(1, bench); ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      r = cc::dwg::write_dwg(model, o);
      write_ms.emplace_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
  } catch (const std::exception& e) {
    print_error_json("invalid", e.what());
    return 1;
  }
  if (const auto ok = cc::base::write_file_atomic(out, std::as_bytes(std::span(r.bytes))); !ok) {
    print_error_json("io", ok.error().message);
    return 1;
  }
  std::string js;
  cc::base::write_json(bench > 0 ? cc::base::make_object({{"write_ms", write_ms}}) : r.report, js);
  js += '\n';
  (void)std::fwrite(js.data(), 1, js.size(), stdout);
  return 0;
}

/// The harness's random bytes: s = s * 1103515245 + 12345 (32 bits), byte = s >> 24, s = 1 at the start of a write.
std::function<void(std::span<std::uint8_t>)> seeded_random() {
  auto s = std::make_shared<std::uint32_t>(1);
  return [s](std::span<std::uint8_t> b) {
    for (std::uint8_t& x : b) {
      *s = *s * 1103515245U + 12345U;
      x = static_cast<std::uint8_t>(*s >> 24U);
    }
  };
}

struct SaveArgs {
  std::optional<double> created;
  bool seeded = false;
  bool model = false;
  int bench = 0;
};

int cmd_dwg_save(const std::filesystem::path& in, const std::string& version, const std::filesystem::path& out,
                 const SaveArgs& a, std::string_view argv0) {
  const auto bytes = cc::base::read_file(in, kMaxFile);
  if (!bytes) {
    print_error_json("io", bytes.error().message);
    return 1;
  }
  const auto lang = cc::base::read_file(assets_dir(argv0) / "lang/en.json", std::uint64_t{1} << 26U);
  if (!lang) {
    print_error_json("io", "the language file is missing: " + lang.error().message);
    return 1;
  }
  auto words = cc::i18n::Catalog::parse("en", cc::base::as_text(*lang));
  if (!words) {
    print_error_json("invalid", words.error().message);
    return 1;
  }
  const cc::base::Value model = cc::dwg::read_dwg(*bytes);
  if (!model["blocks"].is_array() || model["blocks"].as_array().empty()) {
    print_error_json("invalid", "unreadable");
    return 1;
  }
  const std::span<const std::byte> head =
      std::span<const std::byte>(*bytes).first(std::min<std::size_t>(6, bytes->size()));
  cc::doc::Doc d;
  d.load(cc::cad::import_dwg(model, cc::base::as_text(head), *words), *words);
  cc::save::DwgSaveOptions o;
  o.version = version;
  o.created_ms = a.created;
  cc::save::DwgSaveResult r;
  cc::base::Array save_ms;
  try {
    for (int i = 0; i < std::max(1, a.bench); ++i) {
      if (a.seeded) {
        o.random = seeded_random();
      }
      const auto t0 = std::chrono::steady_clock::now();
      r = cc::save::save_dwg(d, o, *words);
      save_ms.emplace_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
  } catch (const std::exception& e) {
    print_error_json("invalid", e.what());
    return 1;
  }
  if (const auto ok = cc::base::write_file_atomic(out, std::as_bytes(std::span(r.bytes))); !ok) {
    print_error_json("io", ok.error().message);
    return 1;
  }
  std::string js;
  if (a.bench > 0) {
    cc::base::write_json(cc::base::make_object({{"save_ms", save_ms}}), js);
  } else {
    cc::base::write_json(a.model ? r.model : r.report, js);
  }
  js += '\n';
  (void)std::fwrite(js.data(), 1, js.size(), stdout);
  return 0;
}

int cmd_dxf_save(const std::filesystem::path& in, const std::string& format, const std::filesystem::path& out,
                 int bench, std::string_view argv0) {
  const auto bytes = cc::base::read_file(in, kMaxFile);
  if (!bytes) {
    print_error_json("io", bytes.error().message);
    return 1;
  }
  const auto lang = cc::base::read_file(assets_dir(argv0) / "lang/en.json", std::uint64_t{1} << 26U);
  if (!lang) {
    print_error_json("io", "the language file is missing: " + lang.error().message);
    return 1;
  }
  auto words = cc::i18n::Catalog::parse("en", cc::base::as_text(*lang));
  if (!words) {
    print_error_json("invalid", words.error().message);
    return 1;
  }
  const cc::base::Value model = cc::dwg::read_dwg(*bytes);
  if (!model["blocks"].is_array() || model["blocks"].as_array().empty()) {
    print_error_json("invalid", "unreadable");
    return 1;
  }
  const std::span<const std::byte> head =
      std::span<const std::byte>(*bytes).first(std::min<std::size_t>(6, bytes->size()));
  cc::doc::Doc d;
  d.load(cc::cad::import_dwg(model, cc::base::as_text(head), *words), *words);
  cc::save::DxfSaveResult r;
  cc::base::Array save_ms;
  try {
    for (int i = 0; i < std::max(1, bench); ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      r = format == "2000" ? cc::save::write_dxf2000(d, *words) : cc::save::write_dxf(d, *words);
      save_ms.emplace_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
  } catch (const std::exception& e) {
    print_error_json("invalid", e.what());
    return 1;
  }
  if (const auto ok = cc::base::write_file_atomic(out, std::as_bytes(std::span(r.text))); !ok) {
    print_error_json("io", ok.error().message);
    return 1;
  }
  std::string js;
  cc::base::write_json(bench > 0 ? cc::base::make_object({{"save_ms", save_ms}}) : r.report, js);
  js += '\n';
  (void)std::fwrite(js.data(), 1, js.size(), stdout);
  return 0;
}

/// A script's {"plot"} as the parity harness plots it: the model to PDF, on the harness's clock (2026-10-16 UTC).
void script_pdf(const cc::base::Value& script, const cc::doc::Doc& doc, cc::base::Value& result) {
  if (const cc::base::Value* p = script.find("plot"); p != nullptr && p->is_object() && !p->is_point()) {
    cc::plot::script_pdf(*p, doc, 1792108800000.0, result);
  }
  if (const cc::base::Value* s = script.find("save"); s != nullptr && s->is_array()) {
    static const cc::i18n::Catalog kKeys;
    cc::engine::script_save(*s, doc, 1792108800000.0, doc.catalog != nullptr ? *doc.catalog : kKeys, result);
  }
}

int cmd_script(const std::filesystem::path& file, std::string_view argv0) {
  const auto text = cc::base::read_file(file, std::uint64_t{1} << 26U);
  if (!text) {
    print_error_json("io", text.error().message);
    return 1;
  }
  const auto lang = cc::base::read_file(assets_dir(argv0) / "lang/en.json", std::uint64_t{1} << 26U);
  if (!lang) {
    print_error_json("io", "the language file is missing: " + lang.error().message);
    return 1;
  }
  auto words = cc::i18n::Catalog::parse("en", cc::base::as_text(*lang));
  if (!words) {
    print_error_json("invalid", words.error().message);
    return 1;
  }
  const auto scripts = cc::base::parse_json(cc::base::as_text(*text));
  if (!scripts || !scripts->is_array()) {
    print_error_json("invalid", scripts ? std::string("a script file is a JSON array") : scripts.error().message);
    return 1;
  }
  for (const cc::base::Value& sc : scripts->as_array()) {
    std::string js;
    try {
      cc::base::write_json(cc::cmd::run_script(sc, *words, {}, script_pdf), js);
    } catch (const std::exception& e) {
      js.clear();
      cc::base::write_json(cc::base::make_object({{"$error", cc::base::make_object({{"message", e.what()}})}}), js);
    }
    js += '\n';
    (void)std::fwrite(js.data(), 1, js.size(), stdout);
  }
  return 0;
}

/// A DWG opened as CadCraft opens it (readDwg → importDwg → Doc.load), with the English words; false + an error
/// printed.
bool open_cad(const std::filesystem::path& in, std::string_view argv0, std::unique_ptr<cc::i18n::Catalog>& words,
              cc::doc::Doc& d) {
  const auto bytes = cc::base::read_file(in, kMaxFile);
  if (!bytes) {
    print_error_json("io", bytes.error().message);
    return false;
  }
  const auto lang = cc::base::read_file(assets_dir(argv0) / "lang/en.json", std::uint64_t{1} << 26U);
  if (!lang) {
    print_error_json("io", "the language file is missing: " + lang.error().message);
    return false;
  }
  auto w = cc::i18n::Catalog::parse("en", cc::base::as_text(*lang));
  if (!w) {
    print_error_json("invalid", w.error().message);
    return false;
  }
  words = std::make_unique<cc::i18n::Catalog>(std::move(*w));
  const cc::base::Value model = cc::dwg::read_dwg(*bytes);
  if (!model["blocks"].is_array() || model["blocks"].as_array().empty()) {
    print_error_json("invalid", "unreadable");
    return false;
  }
  const std::span<const std::byte> head =
      std::span<const std::byte>(*bytes).first(std::min<std::size_t>(6, bytes->size()));
  d.load(cc::cad::import_dwg(model, cc::base::as_text(head), *words), *words);
  return true;
}

/// The sheets a drawing plots as for the parity kit: each layout, then the model at its extents on A3.
std::vector<std::pair<std::string, cc::plot::DisplayList>> plot_lists(const cc::doc::Doc& d) {
  std::vector<std::pair<std::string, cc::plot::DisplayList>> out;
  for (const cc::doc::Layout& l : d.layouts()) {
    out.emplace_back(l.name, cc::plot::display_list(d, cc::plot::layout_sheet(d, l)));
  }
  if (const auto ext = cc::plot::extents_of(d)) {
    out.emplace_back("*model*", cc::plot::display_list(d, cc::plot::model_sheet(d, *ext)));
  }
  return out;
}

int cmd_plot_parity(const std::filesystem::path& in, std::string_view argv0, int runs = 0) {
  std::unique_ptr<cc::i18n::Catalog> words;
  cc::doc::Doc d;
  if (!open_cad(in, argv0, words, d)) {
    return 1;
  }
  if (runs > 0) {  // --time N: the plotting alone (display lists + PDFs), best of N, on stderr
    double best = 1e300;
    double best_lists = 1e300;
    for (int r = 0; r < runs; ++r) {
      const auto t0 = std::chrono::steady_clock::now();
      const auto lists = plot_lists(d);
      const auto t1 = std::chrono::steady_clock::now();
      std::size_t bytes = 0;
      for (const auto& [name, list] : lists) {
        bytes += cc::plot::to_pdf(list, {.pen = 0.25, .title = {}, .date_ms = 0, .compress = {}}).bytes.size();
      }
      best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      best_lists = std::min(best_lists, std::chrono::duration<double, std::milli>(t1 - t0).count());
      (void)bytes;
    }
    std::println(stderr, "plot ms (best of {}): {:.1f} (display lists {:.1f})", runs, best, best_lists);
    return 0;
  }
  cc::base::Array out;
  for (const auto& [name, list] : plot_lists(d)) {
    // tools/parity/plot.mjs: the content uncompressed, the file dated 2026-10-16 (UTC), as the JS side
    const cc::plot::PdfResult pdf =
        cc::plot::to_pdf(list, {.pen = 0.25, .title = {}, .date_ms = 1792108800000.0, .compress = {}});
    out.emplace_back(cc::base::make_object({{"name", name}, {"pdf", cc::base::base64(pdf.bytes)}}));
  }
  std::string js;
  cc::base::write_json(cc::base::Value(std::move(out)), js);
  js += '\n';
  (void)std::fwrite(js.data(), 1, js.size(), stdout);
  return 0;
}

/// plot <file.dwg> --out <file.pdf> [--sheet <layout name> | --model] [--pen mm] [--colour]: one PDF page, or every
/// sheet (no --sheet / --model) as one PDF with a page each.
int cmd_plot(std::span<const std::string_view> args, std::string_view argv0) {
  std::filesystem::path in;
  std::filesystem::path out_path;
  std::optional<std::string> sheet;
  bool model = false;
  bool colour = false;
  double pen = 0.25;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a = args[i];
    const bool has = i + 1 < args.size();
    if (a == "--out" && has) {
      out_path = args[++i];
    } else if (a == "--sheet" && has) {
      sheet = std::string(args[++i]);
    } else if (a == "--model") {
      model = true;
    } else if (a == "--colour" || a == "--color") {
      colour = true;
    } else if (a == "--pen" && has) {
      double v = 0;
      const std::span<const char> txt(args[++i]);
      const auto [ptr, ec] = std::from_chars(txt.data(), std::to_address(txt.end()), v);
      if (ec != std::errc{} || ptr != std::to_address(txt.end())) {
        v = 0;
      }
      if (v <= 0 || v > 5 || std::isnan(v)) {
        std::println(stderr, "cadcraft-cli plot: --pen takes a width in mm (up to 5)");
        return 2;
      }
      pen = v;
    } else if (!a.starts_with("--") && in.empty()) {
      in = a;
    } else {
      std::println(stderr,
                   "usage: cadcraft-cli plot <file.dwg> --out <file.pdf> [--sheet NAME | --model] [--pen MM] "
                   "[--colour]");
      return 2;
    }
  }
  if (in.empty() || out_path.empty()) {
    std::println(stderr, "usage: cadcraft-cli plot <file.dwg> --out <file.pdf> [--sheet NAME | --model]");
    return 2;
  }
  std::unique_ptr<cc::i18n::Catalog> words;
  cc::doc::Doc d;
  if (!open_cad(in, argv0, words, d)) {
    return 1;
  }
  std::vector<cc::plot::DisplayList> lists;
  std::vector<std::string> names;
  for (const cc::doc::Layout& l : d.layouts()) {
    if ((!sheet && !model) || (sheet && *sheet == l.name)) {
      lists.push_back(cc::plot::display_list(d, cc::plot::layout_sheet(d, l), !colour));
      names.push_back(l.name);
    }
  }
  if (model || (!sheet && lists.empty())) {
    const auto ext = cc::plot::extents_of(d);
    if (!ext) {
      print_error_json("empty", "the model is empty: nothing to plot");
      return 1;
    }
    lists.push_back(cc::plot::display_list(d, cc::plot::model_sheet(d, *ext), !colour));
    names.emplace_back("Model");
  }
  if (lists.empty()) {
    print_error_json("invalid", "no sheet named " + sheet.value_or(std::string()));
    return 1;
  }
  const double now = static_cast<double>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
  cc::plot::PdfOptions o{.pen = pen, .title = {}, .date_ms = now, .compress = {}};
#if defined(CC_HAVE_IMAGE)
  o.compress = [](std::span<const std::uint8_t> b) { return cc::image::zlib_compress(b); };
#endif
  const cc::plot::PdfResult pdf =
      lists.size() == 1 ? cc::plot::to_pdf(lists.front(), o) : cc::plot::to_pdf_pages(lists, o, names);
  if (auto w = cc::base::write_file_atomic(out_path, std::as_bytes(std::span(pdf.bytes))); !w) {
    print_error_json("io", w.error().message);
    return 1;
  }
  std::string js;
  cc::base::write_json(cc::base::make_object({{"pages", static_cast<double>(pdf.pages)},
                                              {"texts", static_cast<double>(pdf.texts)},
                                              {"missing", static_cast<double>(pdf.missing)},
                                              {"size", static_cast<double>(pdf.bytes.size())}}),
                       js);
  js += '\n';
  (void)std::fwrite(js.data(), 1, js.size(), stdout);
  return 0;
}

int cmd_walk(const std::filesystem::path& in, std::string_view argv0) {
  std::unique_ptr<cc::i18n::Catalog> words;
  cc::doc::Doc d;
  if (!open_cad(in, argv0, words, d)) {
    return 1;
  }
  cc::base::Array lays;
  for (const cc::doc::Layout& l : d.layouts()) {
    lays.push_back(cc::docview::walk_events(d, *l.entities));
  }
  std::string js;
  cc::base::write_json(
      cc::base::make_object({{"model", cc::docview::walk_events(d, *d.model())}, {"layouts", std::move(lays)}}), js);
  js += '\n';
  (void)std::fwrite(js.data(), 1, js.size(), stdout);
  return 0;
}

int cmd_bench(std::string_view engine, const std::filesystem::path& file, int runs) {
  if (!engine_ported(engine)) {
    print_error_json("unsupported", engine_known(engine) ? "engine not ported yet" : "unknown engine");
    return 3;
  }
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
  {
    const auto warm = cc::base::read_file(file, kMaxFile);
    if (!warm) {
      print_error_json("io", warm.error().message);
      return 1;
    }
    const auto warm_model = run_engine(engine, *warm);
    (void)warm_model;
  }
  cc::base::Array open_ms;
  cc::base::Array parse_ms;
  for (int i = 0; i < runs; ++i) {
    const auto t0 = Clock::now();
    const auto bytes = cc::base::read_file(file, kMaxFile);
    const auto t1 = Clock::now();
    if (!bytes) {
      print_error_json("io", bytes.error().message);
      return 1;
    }
    const auto model = run_engine(engine, *bytes);
    const auto t2 = Clock::now();
    open_ms.emplace_back(std::round(ms(t1 - t0) * 1000) / 1000);
    parse_ms.emplace_back(std::round(ms(t2 - t1) * 1000) / 1000);
  }
  std::string runtime = "C++ ";
#if defined(__clang__)
  runtime += "clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(__GNUC__)
  runtime += "gcc " + std::to_string(__GNUC__);
#elif defined(_MSC_VER)
  runtime += "msvc " + std::to_string(_MSC_VER);
#endif
#if defined(NDEBUG)
  runtime += " release";
#else
  runtime += " debug";
#endif
  const cc::base::Value out = cc::base::make_object({{"open_ms", std::move(open_ms)},
                                                     {"parse_ms", std::move(parse_ms)},
                                                     {"first_frame_ms", nullptr},
                                                     {"runtime", runtime}});
  std::println("{}", cc::base::to_json(out));
  return 0;
}

/// The helper beside this executable (installed layout) or in the build tree's apps/ folder; CC_IMPORTER overrides.
std::filesystem::path importer_path(const char* argv0) {
  if (const auto env = cc::base::env_var("CC_IMPORTER"); env && !env->empty()) {
    return *env;
  }
  std::error_code ec;
  const std::filesystem::path self = std::filesystem::canonical(std::filesystem::path(argv0), ec);
  const std::filesystem::path dir = ec ? std::filesystem::path(".") : self.parent_path();
  for (const auto& c : {dir / "cadcraft-importer", dir / ".." / "cadcraft-importer" / "cadcraft-importer"}) {
    if (std::filesystem::exists(c, ec)) {
      return c;
    }
  }
  return dir / "cadcraft-importer";
}

const char* g_argv0 = "cadcraft-cli";  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) — set once in main

cc::base::Result<cc::base::Value> load_model(const std::filesystem::path& file, bool isolated) {
  auto bytes = cc::base::read_file(file, kMaxFile);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  if (isolated) {
    return cc::importer::run_helper(importer_path(g_argv0), "dwg", *bytes);
  }
  return cc::dwg::read_dwg(*bytes);
}

int cmd_convert(const std::filesystem::path& file, bool isolated) {
  auto m = load_model(file, isolated);
  if (!m) {
    print_error_json("failed", m.error().message);
    return 1;
  }
  if (cc::base::Value* rep = m->find("report")) {
    rep->erase("ms");
  }
  std::string out;
  cc::base::write_json(*m, out);
  out += '\n';
  (void)std::fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}

int cmd_info(const std::filesystem::path& file, bool isolated) {
  auto loaded = load_model(file, isolated);
  if (!loaded) {
    std::println(stderr, "cadcraft-cli: {}", loaded.error().message);
    return 1;
  }
  const cc::base::Value& m = *loaded;
  const cc::base::Value& rep = m["report"];
  if (rep["error"].is_string()) {
    std::println(stderr, "cadcraft-cli: {}", rep["error"].as_string());
    return 1;
  }
  std::println("version     {}", m["version"].is_string() ? m["version"].as_string() : "?");
  std::println("codepage    {}", m["codepage"].is_string() ? m["codepage"].as_string() : "?");
  std::println("objects     {}", rep["objects"].num_or(0));
  std::println("layers      {}", m["tables"]["layers"].as_array().size());
  std::println("blocks      {}", m["blocks"].as_array().size());
  std::println("layouts     {}", m["layouts"].as_array().size());
  std::println("read in     {} ms", rep["ms"].num_or(0));
  std::println("entities:");
  for (const cc::base::Member& c : rep["counts"].as_object()) {
    std::println("  {:<24} {}", c.key, c.value.num_or(0));
  }
  if (!rep["notes"].as_array().empty()) {
    std::println("notes:");
    for (const cc::base::Value& n : rep["notes"].as_array()) {
      std::println("  {}", n.is_string() ? n.as_string() : "");
    }
  }
  return 0;
}

bool is_dxf(const std::filesystem::path& f) {
  std::string ext = f.extension().string();
  std::ranges::transform(ext, ext.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
  return ext == ".dxf";
}

/// A drawing and the drawings it links (xrefs): "<name>.dwg" beside it, matched without regard to case, as BimCraft's
/// tests and the Rust fork's CLI find them.
struct Loaded {
  cc::base::Value model;
  std::vector<cc::base::Value> xref_models;
  cc::drawing::CompileOptions options;
};
cc::base::Result<Loaded> load_drawing(const std::filesystem::path& file) {
  auto bytes = cc::base::read_file(file, kMaxFile);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  Loaded l;
  if (is_dxf(file)) {
    auto m = cc::dxf::read_dxf_model(cc::dxf::decode_text(*bytes));
    if (!m) {
      return std::unexpected(m.error());
    }
    l.model = std::move(*m);
  } else {
    l.model = cc::dwg::read_dwg(*bytes);
    for (std::size_t i = 0; i < 6 && i < bytes->size(); ++i) {
      l.options.version_tag += static_cast<char>(std::to_integer<unsigned char>((*bytes)[i]));
    }
  }
  auto lower = [](std::string s) {
    std::ranges::transform(s, s.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
    return s;
  };
  std::vector<std::pair<std::string, std::filesystem::path>> found;
  std::error_code ec;
  const std::filesystem::path dir = file.has_parent_path() ? file.parent_path() : std::filesystem::path(".");
  for (const cc::base::Value& b : l.model["blocks"].is_array() ? l.model["blocks"].as_array() : cc::base::Array{}) {
    if (!b["name"].is_string() || (static_cast<std::int64_t>(b["flags"].num_or(0)) & 4) == 0) {
      continue;
    }
    const std::string want = lower(b["name"].as_string() + ".dwg");
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (lower(entry.path().filename().string()) == want) {
        found.emplace_back(lower(b["name"].as_string()), entry.path());
        break;
      }
    }
  }
  l.xref_models.reserve(found.size());
  for (const auto& [key, path] : found) {
    const auto xb = cc::base::read_file(path, kMaxFile);
    if (xb) {
      l.xref_models.push_back(cc::dwg::read_dwg(*xb));
      l.options.xrefs.push_back(cc::drawing::Xref{key, nullptr});
    }
  }
  for (std::size_t i = 0; i < l.options.xrefs.size(); ++i) {
    l.options.xrefs[i].model = &l.xref_models[i];  // after the vector stops growing
  }
  return l;
}

int cmd_compile(const std::filesystem::path& file, int bench_runs) {
  auto loaded = load_drawing(file);
  if (!loaded) {
    print_error_json("failed", loaded.error().message);
    return 1;
  }
  if (bench_runs > 0) {
    using Clock = std::chrono::steady_clock;
    double best = 1e300;
    (void)cc::drawing::compile_drawing(loaded->model, loaded->options);
    for (int i = 0; i < bench_runs; ++i) {
      const auto t0 = Clock::now();
      const cc::drawing::Drawing d = cc::drawing::compile_drawing(loaded->model, loaded->options);
      best = std::min(best, std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    }
    std::println("C++ compile best of {} = {:.2f} ms", bench_runs, best);
    return 0;
  }
  const cc::drawing::Drawing d = cc::drawing::compile_drawing(loaded->model, loaded->options);
  std::string out;
  cc::drawing::write_json(d, out);
  out += '\n';
  (void)std::fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}

/// `cloud <file> [--budget N]`: a point cloud as pointcloud.js reads it (tools/parity/cloud.mjs compares)
cc::base::Value v3_value(const cc::cloud::V3& v) {
  return cc::base::Value(cc::base::Array{v[0], v[1], v[2]});
}
template <class T>
cc::base::Value array_of(const std::vector<T>& v) {
  cc::base::Array a;
  a.reserve(v.size());
  for (const T x : v) {
    a.emplace_back(static_cast<double>(x));
  }
  return {std::move(a)};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — one object, field by field
int cmd_cloud(const std::filesystem::path& file, double budget, std::string_view argv0) {
  const auto lang = cc::base::read_file(assets_dir(argv0) / "lang/en.json", std::uint64_t{1} << 26U);
  if (!lang) {
    print_error_json("io", "the language file is missing: " + lang.error().message);
    return 1;
  }
  auto words = cc::i18n::Catalog::parse("en", cc::base::as_text(*lang));
  if (!words) {
    print_error_json("invalid", words.error().message);
    return 1;
  }
  const auto bytes = cc::base::read_file(file, cc::cloud::kMaxFileBytes + 1);
  if (!bytes) {
    print_error_json("io", bytes.error().message);
    return 1;
  }
  const std::string name = file.filename().string();
  cc::base::Value r = cc::base::make_object({{"name", name}, {"budget", budget}});
  const auto write = [&r] {
    std::string js;
    cc::base::write_json(r, js);
    js += '\n';
    (void)std::fwrite(js.data(), 1, js.size(), stdout);
    return 0;
  };
  const auto info = cc::cloud::read_info(name, *bytes, *words);
  if (!info) {
    r.set("error", info.error().message);
    return write();
  }
  cc::base::Array notes;
  for (const std::string& s : info->notes) {
    notes.emplace_back(s);
  }
  cc::base::Value iv = cc::base::make_object({{"kind", info->kind},
                                              {"name", info->name},
                                              {"size", static_cast<double>(info->size)},
                                              {"count", info->count},
                                              {"estimate", info->estimate},
                                              {"min", info->min ? v3_value(*info->min) : cc::base::Value(nullptr)},
                                              {"max", info->max ? v3_value(*info->max) : cc::base::Value(nullptr)},
                                              {"color", info->color},
                                              {"intensity", info->intensity},
                                              {"classes", info->classes},
                                              {"notes", cc::base::Value(std::move(notes))}});
  iv.set("columns", info->columns ? cc::base::make_object(
                                        {{"n", info->columns->n}, {"i", info->columns->i}, {"rgb", info->columns->rgb}})
                                  : cc::base::Value(nullptr));
  if (info->version) {
    iv.set("version", *info->version);
  }
  if (info->format) {
    iv.set("format", *info->format);
  }
  r.set("info", std::move(iv));
  const auto c = cc::cloud::load_cloud(name, *bytes, *info, budget, *words);
  if (!c) {
    r.set("error", c.error().message);
    return write();
  }
  cc::base::Array counts;
  for (const auto& [k, v] : c->class_counts) {
    counts.emplace_back(cc::base::Array{k, static_cast<double>(v)});
  }
  cc::base::Array cnotes;
  for (const std::string& s : c->notes) {
    cnotes.emplace_back(s);
  }
  r.set("cloud", cc::base::make_object({{"name", c->name},
                                        {"kind", c->kind},
                                        {"read", static_cast<double>(c->read)},
                                        {"n", static_cast<double>(c->n)},
                                        {"origin", v3_value(c->origin)},
                                        {"pos", array_of(c->pos)},
                                        {"rgb", c->rgb ? array_of(*c->rgb) : cc::base::Value(nullptr)},
                                        {"inten", c->inten ? array_of(*c->inten) : cc::base::Value(nullptr)},
                                        {"cls", c->cls ? array_of(*c->cls) : cc::base::Value(nullptr)},
                                        {"min", v3_value(c->min)},
                                        {"max", v3_value(c->max)},
                                        {"classCounts", cc::base::Value(std::move(counts))},
                                        {"notes", cc::base::Value(std::move(cnotes))}}));
  cc::base::Value colors = cc::base::Value::object();
  for (const char* m : {"rgb", "height", "intensity", "class", "single"}) {
    colors.set(m, array_of(cc::cloud::colorize(*c, m)));
  }
  r.set("colors", std::move(colors));
  r.set("single120", array_of(cc::cloud::colorize(*c, "single", {120, 120, 120})));
  cc::base::Array modes;
  for (const std::string& m : cc::cloud::color_modes(*c)) {
    modes.emplace_back(m);
  }
  r.set("modes", cc::base::Value(std::move(modes)));
  const auto mid = [&c](std::size_t a) { return (c->min.at(a) + c->max.at(a)) / 2; };
  struct Slice {
    char axis;
    double at, thick;
  };
  const std::array<Slice, 4> slices{
      {{'z', c->min[2] + 1.2, 0.1}, {'z', c->min[2] + 0.5, 0.3}, {'x', mid(0), 0.2}, {'y', mid(1), -0.4}}};
  cc::base::Array sl;
  for (const Slice& s : slices) {
    sl.emplace_back(array_of(cc::cloud::slice_of(*c, s.axis, s.at, s.thick)));
  }
  r.set("slices", cc::base::Value(std::move(sl)));
  const std::array<std::array<double, 3>, 5> queries{{{mid(0), mid(1), 0.05},
                                                      {mid(0), mid(1), 0.5},
                                                      {c->min[0], c->min[1], 0.2},
                                                      {c->max[0] + 5, c->max[1], 1},
                                                      {mid(0), mid(1), 1e9}}};
  const auto near = [&queries](const cc::cloud::PlanGrid& g) {
    cc::base::Array a;
    for (const auto& q : queries) {
      a.emplace_back(static_cast<double>(g.nearest(q[0], q[1], q[2])));
    }
    return cc::base::Value(std::move(a));
  };
  r.set("grid", near(cc::cloud::PlanGrid(*c, nullptr)));
  const std::vector<std::uint32_t> cut = cc::cloud::slice_of(*c, 'z', c->min[2] + 1.2, 0.1);
  r.set("gridSlice", near(cc::cloud::PlanGrid(*c, &cut)));
  r.set("gridCell", near(cc::cloud::PlanGrid(*c, nullptr, 0.25)));
  const cc::cloud::Summary s = cc::cloud::summary(*c, *words);
  cc::base::Array classes;
  for (const cc::cloud::ClassRow& row : s.classes) {
    classes.emplace_back(cc::base::make_object({{"c", row.c}, {"name", row.name}, {"n", static_cast<double>(row.n)}}));
  }
  r.set("summary", cc::base::make_object({{"read", static_cast<double>(s.read)},
                                          {"kept", static_cast<double>(s.kept)},
                                          {"size", v3_value(s.size)},
                                          {"classes", cc::base::Value(std::move(classes))}}));
  return write();
}

int run(const std::vector<std::string_view>& args) {
  if (args.size() < 2 || args[1] == "help" || args[1] == "--help") {
    usage(stdout);
    return args.size() < 2 ? 2 : 0;
  }
  const std::string_view cmd = args[1];
  if (cmd == "version" || cmd == "--version") {
    std::println("cadcraft-cli {}", kVersion);
    return 0;
  }
  if (const auto r = cc::cli::run_m3d(args)) {  // the 3D commands: m3d_cli.cpp
    return *r;
  }
  const bool isolated = args.size() > 2 && args[2] == "--isolated";
  const std::size_t a = isolated ? 3 : 2;
  if (cmd == "info" && args.size() == a + 1) {
    return cmd_info(std::filesystem::path(args[a]), isolated);
  }
  if (cmd == "convert" && args.size() == a + 2 && args[a] == "--json") {
    return cmd_convert(std::filesystem::path(args[a + 1]), isolated);
  }
  if (cmd == "compile" && args.size() == 4 && args[3] == "--json") {
    return cmd_compile(std::filesystem::path(args[2]), 0);
  }
  if (cmd == "compile" && args.size() == 5 && args[3] == "--bench") {
    int n = 0;
    const std::span<const char> txt(args[4]);
    const auto [ptr, ec] = std::from_chars(txt.data(), std::to_address(txt.end()), n);
    if (ec != std::errc{} || ptr != std::to_address(txt.end()) || n < 1 || n > 1000) {
      usage(stderr);
      return 2;
    }
    return cmd_compile(std::filesystem::path(args[2]), n);
  }
  if (cmd == "dwg-write" && args.size() >= 5) {
    std::optional<double> created;
    bool seeded = false;
    int bench = 0;
    for (std::size_t i = 5; i < args.size(); ++i) {
      if (args[i] == "--seeded") {
        seeded = true;
      } else if (args[i] == "--bench" && i + 1 < args.size()) {
        const std::span<const char> txt(args[i + 1]);
        const auto [ptr, ec] = std::from_chars(txt.data(), std::to_address(txt.end()), bench);
        if (ec != std::errc{} || ptr != std::to_address(txt.end()) || bench < 1 || bench > 1000) {
          usage(stderr);
          return 2;
        }
        ++i;
      } else if (args[i] == "--created" && i + 1 < args.size()) {
        double ms = 0;
        const std::span<const char> txt(args[i + 1]);
        const auto [ptr, ec] = std::from_chars(txt.data(), std::to_address(txt.end()), ms);
        if (ec != std::errc{} || ptr != std::to_address(txt.end())) {
          usage(stderr);
          return 2;
        }
        created = ms;
        ++i;
      } else {
        usage(stderr);
        return 2;
      }
    }
    return cmd_dwg_write(std::filesystem::path(args[2]), std::string(args[3]), std::filesystem::path(args[4]), created,
                         seeded, bench);
  }
  if (cmd == "dwg-save" && args.size() >= 5) {
    SaveArgs sa;
    for (std::size_t i = 5; i < args.size(); ++i) {
      if (args[i] == "--seeded") {
        sa.seeded = true;
      } else if (args[i] == "--model") {
        sa.model = true;
      } else if (args[i] == "--bench" && i + 1 < args.size()) {
        const std::span<const char> txt(args[i + 1]);
        const auto [ptr, ec] = std::from_chars(txt.data(), std::to_address(txt.end()), sa.bench);
        if (ec != std::errc{} || ptr != std::to_address(txt.end()) || sa.bench < 1 || sa.bench > 1000) {
          usage(stderr);
          return 2;
        }
        ++i;
      } else if (args[i] == "--created" && i + 1 < args.size()) {
        double ms = 0;
        const std::span<const char> txt(args[i + 1]);
        const auto [ptr, ec] = std::from_chars(txt.data(), std::to_address(txt.end()), ms);
        if (ec != std::errc{} || ptr != std::to_address(txt.end())) {
          usage(stderr);
          return 2;
        }
        sa.created = ms;
        ++i;
      } else {
        usage(stderr);
        return 2;
      }
    }
    return cmd_dwg_save(std::filesystem::path(args[2]), std::string(args[3]), std::filesystem::path(args[4]), sa,
                        args[0]);
  }
  if (cmd == "dxf-save" && (args.size() == 5 || args.size() == 7) && (args[3] == "r12" || args[3] == "2000")) {
    int bench = 0;
    if (args.size() == 7) {
      const std::span<const char> txt(args[6]);
      const auto [ptr, ec] = std::from_chars(txt.data(), std::to_address(txt.end()), bench);
      if (args[5] != "--bench" || ec != std::errc{} || ptr != std::to_address(txt.end()) || bench < 1 || bench > 1000) {
        usage(stderr);
        return 2;
      }
    }
    return cmd_dxf_save(std::filesystem::path(args[2]), std::string(args[3]), std::filesystem::path(args[4]), bench,
                        args[0]);
  }
  if (cmd == "plot-parity" && args.size() == 3) {
    return cmd_plot_parity(std::filesystem::path(args[2]), args[0]);
  }
  if (cmd == "plot-parity" && args.size() == 5 && args[3] == "--time") {
    return cmd_plot_parity(std::filesystem::path(args[2]), args[0], args[4] == "1" ? 1 : 5);
  }
  if (cmd == "plot" && args.size() >= 3) {
    return cmd_plot(std::span<const std::string_view>(args).subspan(2), args[0]);
  }
  if (cmd == "walk" && args.size() == 3) {
    return cmd_walk(std::filesystem::path(args[2]), args[0]);
  }
  if (cmd == "cloud" && (args.size() == 3 || (args.size() == 5 && args[3] == "--budget"))) {
    const double budget = args.size() == 5 ? cc::base::js_number(args[4]) : 2e6;
    return cmd_cloud(std::filesystem::path(args[2]), budget, args[0]);
  }
  if (cmd == "script" && args.size() == 3) {
    return cmd_script(std::filesystem::path(args[2]), args[0]);
  }
  if (cmd == "doc" && args.size() == 3) {
    return cmd_import(std::filesystem::path(args[2]), args[0], true);
  }
  if (cmd == "import" && args.size() == 3) {
    return cmd_import(std::filesystem::path(args[2]), args[0]);
  }
  if (cmd == "parity" && args.size() == 4) {
    return cmd_parity(args[2], std::filesystem::path(args[3]));
  }
  if (cmd == "bench" && (args.size() == 4 || args.size() == 6)) {
    int runs = 5;
    if (args.size() == 6) {
      if (args[4] != "--runs") {
        usage(stderr);
        return 2;
      }
      int n = 0;
      const std::span<const char> txt(args[5]);
      const auto [ptr, ec] = std::from_chars(txt.data(), std::to_address(txt.end()), n);
      if (ec != std::errc{} || ptr != std::to_address(txt.end()) || n < 1 || n > 1000) {
        usage(stderr);
        return 2;
      }
      runs = n;
    }
    return cmd_bench(args[2], std::filesystem::path(args[3]), runs);
  }
  std::println(stderr, "cadcraft-cli: unknown command or wrong arguments");
  usage(stderr);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
    if (!raw.empty()) {
      g_argv0 = raw.front();
    }
    return run(std::vector<std::string_view>(raw.begin(), raw.end()));
  } catch (const std::exception& e) {
    say_failure(e.what());
  } catch (...) {
    say_failure("unexpected failure");
  }
  return 1;
}
