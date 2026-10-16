// SPDX-License-Identifier: Apache-2.0
// cadcraft-cli — the headless entry point (AGENTS.md section 9).
//
//   cadcraft-cli info <file.dwg>                 version, counts, notes
//   cadcraft-cli convert --json <file.dwg>       the drawing model as JSON (the JS readDwg model's shape)
//   cadcraft-cli parity <engine> <file>          the parity kit's contract (tools/fork-parity in the site repo):
//                                                dwg → the model as JSON; other engines → exit 3 (not ported yet)
//   cadcraft-cli bench <engine> <file> [--runs N]  {"open_ms":[…],"parse_ms":[…],"first_frame_ms":null,"runtime":"…"}
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
#include "cc/base/value.hpp"
#include "cc/dwg/read.hpp"

namespace {

constexpr std::string_view kVersion = "0.0.0";
constexpr std::uint64_t kMaxFile = std::uint64_t{1} << 31U;  // 2 GiB

void usage(std::FILE* out) {
  std::print(out,
             "usage: cadcraft-cli <command> [options]\n"
             "  info <file.dwg>                     version, counts and notes\n"
             "  convert --json <file.dwg>           the drawing model as JSON\n"
             "  parity <engine> <file>              engine output for the parity kit (engines: dwg)\n"
             "  bench <engine> <file> [--runs N]    in-process timings for the parity kit\n"
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
  return e == "dwg" || e == "ifc-step" || e == "ifc-geom" || e == "skp";
}

int cmd_parity(std::string_view engine, const std::filesystem::path& file) {
  if (engine != "dwg") {
    print_error_json("unsupported", engine_known(engine) ? "engine not ported yet" : "unknown engine");
    return 3;
  }
  const auto bytes = cc::base::read_file(file, kMaxFile);
  if (!bytes) {
    print_error_json("io", bytes.error().message);
    return 1;
  }
  cc::base::Value model = cc::dwg::read_dwg(*bytes);
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

int cmd_bench(std::string_view engine, const std::filesystem::path& file, int runs) {
  if (engine != "dwg") {
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
    (void)cc::dwg::read_dwg(*warm);
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
    const cc::base::Value model = cc::dwg::read_dwg(*bytes);
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

int cmd_info(const std::filesystem::path& file) {
  const auto bytes = cc::base::read_file(file, kMaxFile);
  if (!bytes) {
    std::println(stderr, "cadcraft-cli: {}", bytes.error().message);
    return 1;
  }
  const cc::base::Value m = cc::dwg::read_dwg(*bytes);
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
  if (cmd == "info" && args.size() == 3) {
    return cmd_info(std::filesystem::path(args[2]));
  }
  if (cmd == "convert" && args.size() == 4 && args[2] == "--json") {
    return cmd_parity("dwg", std::filesystem::path(args[3]));
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
      const std::span<const char> a(args[5]);
      const auto [ptr, ec] = std::from_chars(a.data(), std::to_address(a.end()), n);
      if (ec != std::errc{} || ptr != std::to_address(a.end()) || n < 1 || n > 1000) {
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
    return run(std::vector<std::string_view>(argv, std::next(argv, argc)));
  } catch (const std::exception& e) {
    say_failure(e.what());
  } catch (...) {
    say_failure("unexpected failure");
  }
  return 1;
}
