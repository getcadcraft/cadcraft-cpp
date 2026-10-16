// SPDX-License-Identifier: Apache-2.0
// cadcraft-importer <format> — the out-of-process import helper (AGENTS.md section 4). Reads the file's bytes on stdin,
// locks itself down (memory and CPU limits; on Linux a seccomp filter: no files, no network, no new processes), parses,
// and writes the model as one JSON document on stdout. Exit 0 = a model (it may carry report.error), 1 = refused,
// 3 = a format this build does not have. It never opens a file and never reads its environment.
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "cc/base/value.hpp"
#include "cc/dwg/read.hpp"
#include "cc/importer/importer.hpp"

namespace {

constexpr std::size_t kMaxInput = std::size_t{1} << 31U;

bool write_all(std::string_view s) {
  while (!s.empty()) {
    const ssize_t n = ::write(1, s.data(), s.size());
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    s.remove_prefix(static_cast<std::size_t>(n));
  }
  return true;
}

int answer_error(std::string_view code, std::string_view message, int exit_code) {
  const cc::base::Value e = cc::base::make_object(
      {{"$error", cc::base::make_object({{"code", std::string(code)}, {"message", std::string(message)}})}});
  (void)write_all(cc::base::to_json(e));
  return exit_code;
}

int run(std::string_view format) {
  std::vector<std::byte> input;
  std::vector<std::byte> chunk(std::size_t{1} << 20U);
  for (;;) {
    const ssize_t n = ::read(0, chunk.data(), chunk.size());
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      return answer_error("io", "cannot read the input", 1);
    }
    if (n == 0) {
      break;
    }
    if (input.size() + static_cast<std::size_t>(n) > kMaxInput) {
      return answer_error("too_big", "the file is larger than the importer's limit", 1);
    }
    input.insert(input.end(), chunk.begin(), chunk.begin() + n);
  }
  chunk = {};
  if (!cc::importer::lock_down_helper(cc::importer::Limits{})) {
    return answer_error("sandbox", "the importer could not apply its limits", 1);
  }
  if (format != "dwg") {
    return answer_error("unsupported", "format not ported yet", 3);
  }
  // one thread: the seccomp filter and the NPROC cap forbid starting more (and a crash stays one process)
  const cc::base::Value model = cc::dwg::read_dwg(input, {.threads = 1});
  std::string out;
  out.reserve(input.size() * 8);
  cc::base::write_json(model, out);
  out += '\n';
  return write_all(out) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    if (args.size() != 2) {
      return answer_error("usage", "usage: cadcraft-importer <format>", 2);
    }
    return run(args[1]);
  } catch (...) {
    // Nothing that can throw here: a fixed answer.
    (void)write_all(R"({"$error":{"code":"failed","message":"the importer failed"}})");
    return 1;
  }
}
