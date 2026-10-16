// SPDX-License-Identifier: Apache-2.0
// cc::importer — L4. Out-of-process import (AGENTS.md section 4): readers of binary formats from strangers run in a
// helper process (apps/cadcraft-importer) with no window, a memory limit, a CPU-time limit and (Linux) a seccomp
// filter, and send the parsed document back as JSON over a pipe. A crash or hang in a parser costs a message, not the
// caller's open work.
//
//   run_helper(helper, "dwg", bytes, limits) → the model, or an Error ("the importer stopped: …")
//
// The helper never opens files: the caller reads the file (it owns path confinement) and passes the bytes on stdin.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"

namespace cc::importer {

struct Limits {
  std::uint64_t max_memory = std::uint64_t{4} << 30U;  // address space of the helper (bytes)
  int max_seconds = 120;                               // wall clock and CPU time
  std::uint64_t max_output = std::uint64_t{2} << 30U;  // JSON coming back (bytes)
};

[[nodiscard]] base::Result<base::Value> run_helper(const std::filesystem::path& helper, std::string_view format,
                                                   std::span<const std::byte> bytes, const Limits& limits = {});

/// The helper's side: apply the resource limits and, where the platform has one, the system-call filter. Call after
/// the input is read and before parsing. Returns false when a limit could not be applied (the helper then refuses).
[[nodiscard]] bool lock_down_helper(const Limits& limits);

}  // namespace cc::importer
