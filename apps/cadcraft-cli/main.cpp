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
#include <iterator>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "cc/base/file.hpp"
#include "cc/base/reader.hpp"
#include "cc/base/value.hpp"
#include "cc/cad/import.hpp"
#include "cc/drawing/compile.hpp"
#include "cc/dwg/read.hpp"
#include "cc/dxf/read.hpp"
#include "cc/i18n/i18n.hpp"
#include "cc/importer/importer.hpp"

namespace {

constexpr std::string_view kVersion = "0.0.0";
constexpr std::uint64_t kMaxFile = std::uint64_t{1} << 31U;  // 2 GiB

void usage(std::FILE* out) {
  std::print(out,
             "usage: cadcraft-cli <command> [options]\n"
             "  info [--isolated] <file.dwg>        version, counts and notes\n"
             "  convert [--isolated] --json <file>  the drawing model as JSON\n"
             "                                      (--isolated: parse in the sandboxed import helper)\n"
             "  parity <engine> <file>              engine output for the parity kit (engines: dwg, dxf)\n"
             "  bench <engine> <file> [--runs N]    in-process timings for the parity kit\n"
             "  compile <file> --json | --bench N   the 2D view's draw lists (DWG or DXF)\n"
             "  import <file.dwg>                   CadCraft's drawing (what the app's document loads) as JSON\n"
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

int cmd_import(const std::filesystem::path& file, std::string_view argv0) {
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
  const cc::base::Value model = cc::dwg::read_dwg(*bytes);
  if (!model["blocks"].is_array() || model["blocks"].as_array().empty()) {
    print_error_json("invalid", "unreadable");
    return 1;
  }
  const std::span<const std::byte> head =
      std::span<const std::byte>(*bytes).first(std::min<std::size_t>(6, bytes->size()));
  const cc::base::Value drawing = cc::cad::import_dwg(model, cc::base::as_text(head), *words);
  std::string out;
  cc::base::write_json(drawing, out);
  out += '\n';
  (void)std::fwrite(out.data(), 1, out.size(), stdout);
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
