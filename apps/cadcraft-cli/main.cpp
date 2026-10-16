// SPDX-License-Identifier: Apache-2.0
// cadcraft-cli — the headless entry point (AGENTS.md section 9). Commands arrive milestone by milestone.
#include <cstdio>
#include <exception>
#include <iterator>
#include <print>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kVersion = "0.0.0";

void usage(std::FILE* out) {
  std::print(out,
             "usage: cadcraft-cli <command> [options]\n"
             "  version            print the version\n"
             "  help               this text\n");
}

// The last word on a failure: never throws (std::print could).
void say_failure(const char* what) noexcept {
  if (std::fputs("cadcraft-cli: ", stderr) == EOF || std::fputs(what, stderr) == EOF) {
    return;
  }
  (void)std::fputc('\n', stderr);
}

int run(const std::vector<std::string_view>& args) {
  if (args.size() < 2 || args[1] == "help" || args[1] == "--help") {
    usage(stdout);
    return args.size() < 2 ? 2 : 0;
  }
  if (args[1] == "version" || args[1] == "--version") {
    std::println("cadcraft-cli {}", kVersion);
    return 0;
  }
  std::println(stderr, "cadcraft-cli: unknown command");
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
