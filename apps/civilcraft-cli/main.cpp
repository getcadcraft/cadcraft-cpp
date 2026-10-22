// SPDX-License-Identifier: Apache-2.0
// civilcraft-cli — CivilCraft without a window (AGENTS.md section 9).
//
//   civilcraft-cli script <calls.json> [--locale en|es-MX]   a call script (libs/civil script.hpp): one {ok} / {err}
//                                                            per call, as JSON on stdout (tools/parity/civil.mjs)
//   civilcraft-cli bench <calls.json> [--runs N]             the best and median wall time of N runs of the script
//   civilcraft-cli ops                                       the ops a call script may use, one per line
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/base/value.hpp"
#include "cc/civil/script.hpp"
#include "cc/i18n/i18n.hpp"

namespace {

constexpr std::uint64_t kMaxScript = std::uint64_t{1} << 30U;

void say(std::FILE* f, std::string_view s) {
  (void)std::fwrite(s.data(), 1, s.size(), f);
}

void usage() {
  say(stderr,
      "usage: civilcraft-cli script <calls.json> [--locale en|es-MX]\n"
      "       civilcraft-cli bench <calls.json> [--runs N]\n");
}

std::filesystem::path assets_dir(std::string_view argv0) {
  if (const auto env = cc::base::env_var("APP_ASSETS_DIR"); env && !env->empty()) {
    return *env;
  }
  std::error_code ec;
  auto beside = std::filesystem::canonical(std::filesystem::path(argv0), ec).parent_path() / "assets";
  if (!ec && std::filesystem::exists(beside / "lang/civilcraft/en.json", ec)) {
    return beside;
  }
  return CC_ASSETS_DIR;
}

cc::base::Result<cc::i18n::Catalog> load_words(std::string_view argv0, std::string_view locale) {
  const auto file = assets_dir(argv0) / "lang/civilcraft" / (std::string(locale) + ".json");
  const auto text = cc::base::read_file(file, std::uint64_t{1} << 26U);
  if (!text) {
    return std::unexpected(text.error());
  }
  return cc::i18n::Catalog::parse(std::string(locale), cc::base::as_text(*text));
}

int run(std::span<const std::string_view> args, std::string_view argv0) {
  if (args.size() == 1 && args[0] == "ops") {
    for (const std::string& op : cc::civil::script_ops()) {
      say(stdout, op + "\n");
    }
    return 0;
  }
  if (args.size() < 2) {
    usage();
    return 2;
  }
  const std::string_view verb = args[0];
  const std::filesystem::path file(args[1]);
  std::string locale = "en";
  int runs = 5;
  for (std::size_t i = 2; i + 1 < args.size(); i += 2) {
    if (args[i] == "--locale" && (args[i + 1] == "en" || args[i + 1] == "es-MX")) {
      locale = args[i + 1];
    } else if (args[i] == "--runs") {
      int n = 0;
      const std::string v(args[i + 1]);
      if (std::from_chars(v.data(), std::to_address(v.end()), n).ec != std::errc{}) {
        usage();
        return 2;
      }
      runs = std::clamp(n, 1, 1000);
    } else {
      usage();
      return 2;
    }
  }
  auto english = load_words(argv0, "en");
  if (!english) {
    say(stderr, "civilcraft-cli: the language files are missing: " + english.error().message + "\n");
    return 1;
  }
  auto words = locale == "en" ? std::move(english) : load_words(argv0, locale);
  if (!words) {
    say(stderr, "civilcraft-cli: " + words.error().message + "\n");
    return 1;
  }
  const auto text = cc::base::read_file(file, kMaxScript);
  if (!text) {
    say(stderr, "civilcraft-cli: " + text.error().message + "\n");
    return 1;
  }
  const auto script = cc::base::parse_json(cc::base::as_text(*text));
  if (!script) {
    say(stderr, "civilcraft-cli: not a call script: " + script.error().message + "\n");
    return 1;
  }
  if (verb == "script") {
    const cc::base::Value out = cc::civil::run_script(*script, *words);
    say(stdout, cc::base::to_json(out));
    say(stdout, "\n");
    return 0;
  }
  if (verb == "bench") {
    std::vector<double> ms;
    for (int r = 0; r < runs; ++r) {
      const auto t0 = std::chrono::steady_clock::now();
      const cc::base::Value out = cc::civil::run_script(*script, *words);
      const auto t1 = std::chrono::steady_clock::now();
      ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::ranges::sort(ms);
    say(stdout,
        "{\"best_ms\":" + std::to_string(ms.front()) + ",\"median_ms\":" + std::to_string(ms[ms.size() / 2]) + "}\n");
    return 0;
  }
  usage();
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
    const std::vector<std::string_view> args(raw.begin() + (raw.empty() ? 0 : 1), raw.end());
    return run(args, raw.empty() ? std::string_view() : std::string_view(raw.front()));
  } catch (const std::exception& e) {
    (void)std::fputs("civilcraft-cli: ", stderr);
    (void)std::fputs(e.what(), stderr);
    (void)std::fputs("\n", stderr);
  } catch (...) {
    (void)std::fputs("civilcraft-cli: unexpected failure\n", stderr);
  }
  return 1;
}
