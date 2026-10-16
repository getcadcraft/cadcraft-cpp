// SPDX-License-Identifier: Apache-2.0
// Whole-file reads with a size cap, and atomic writes (temp file in the same directory, flush, fsync, rename,
// fsync the directory; AGENTS.md section 6, native format).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cc/base/error.hpp"

namespace cc::base {

/// Reads a whole file; refuses one larger than `max_bytes`.
[[nodiscard]] Result<std::vector<std::byte>> read_file(const std::filesystem::path& path, std::uint64_t max_bytes);

/// Writes `data` to `path` atomically: readers see the old file or the new one, never a part.
[[nodiscard]] Result<void> write_file_atomic(const std::filesystem::path& path, std::span<const std::byte> data);

/// An environment variable's value, or nothing when it is unset (getenv, or _dupenv_s on MSVC). Read it at start-up
/// on one thread: the environment is not thread-safe on any platform.
[[nodiscard]] std::optional<std::string> env_var(const char* name);

}  // namespace cc::base
