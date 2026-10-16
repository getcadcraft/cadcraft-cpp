// SPDX-License-Identifier: Apache-2.0
// Test helper (never shipped): behaves like a broken importer. argv[1] picks the failure.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string_view>
#include <thread>

int main(int argc, char** argv) {
  const std::string_view mode = argc > 1 ? std::string_view(argv[1]) : std::string_view("segv");  // NOLINT
  if (mode == "segv") {
    (void)std::raise(SIGSEGV);
  }
  if (mode == "hang") {
    std::this_thread::sleep_for(std::chrono::hours(1));
  }
  if (mode == "garbage") {
    (void)std::fputs("{not json", stdout);
    return 0;
  }
  return 7;
}
