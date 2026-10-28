// SPDX-License-Identifier: Apache-2.0
// cc::mcp — L2. The headless tools' surroundings (CadCraft 0.5 tools/cli/headless/paths.mjs and tools.mjs createEnv,
// docs/AGENTS-SPEC.md § 3, § 5.1): files only inside the folders the person names, and the open files by handle.
//
//   Roots roots({"~/Drawings"});                   // each resolved to its real path; missing → an error
//   roots.read("~/Drawings/Plan.dwg")             → the real path, or a PathError
//   roots.write("out/Plan.pdf", overwrite, mine)  → the real path for a new or replaced file
//
// Every path is resolved with realpath (links followed, `..` gone) and must stay inside a root's real path; a new
// file's folder must already exist (and be inside). Relative paths are taken from the first root. No roots = no file
// access. Each app's tools (W1 CadCraft, W3 BimCraft, W5 TakeoffCraft / MarkupCraft) keep their open sessions in an
// Env through keep / get, so handles (d1, d2, …; 16 at most) work across apps as in 0.5.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/mcp/dispatcher.hpp"

namespace cc::mcp {

/// paths.mjs PathError: code outside | missing | invalid | noroots | exists.
struct PathError {
  std::string message;
  std::string code = "outside";
};
template <class T>
using PathResult = std::expected<T, PathError>;

class Roots {
 public:
  [[nodiscard]] static PathResult<Roots> make(const std::vector<std::string>& list);

  [[nodiscard]] bool any() const noexcept { return !list_.empty(); }
  /// The roots' real paths, in the order given.
  [[nodiscard]] const std::vector<std::string>& list() const noexcept { return list_; }
  /// The root a real path is inside, if any.
  [[nodiscard]] std::optional<std::string> root_of(std::string_view real) const;
  /// An existing file to read.
  [[nodiscard]] PathResult<std::string> read(std::string_view p) const;
  /// A folder to list (empty = the first root).
  [[nodiscard]] PathResult<std::string> folder(std::string_view p) const;
  /// A file to write; anything that exists and is not in `mine` needs `overwrite`; never a link.
  [[nodiscard]] PathResult<std::string> write(std::string_view p, bool overwrite,
                                              const std::set<std::string>& mine) const;

 private:
  [[nodiscard]] PathResult<std::string> abs(std::string_view p) const;
  [[nodiscard]] PathResult<std::string> lexical(const std::string& a) const;
  [[nodiscard]] PathResult<std::string> check(const std::string& real, const std::string& shown) const;
  [[nodiscard]] std::string outside(const std::string& shown) const;

  std::vector<std::string> list_;  // real paths
  std::vector<std::string> raw_;   // the folders as named (one may be reached through a link)
};

/// One open file's session (an app's own type).
class DocSession {
 public:
  DocSession() = default;
  virtual ~DocSession() = default;
  DocSession(const DocSession&) = delete;
  DocSession& operator=(const DocSession&) = delete;
  DocSession(DocSession&&) = delete;
  DocSession& operator=(DocSession&&) = delete;
};

struct OpenDoc {
  std::string kind;  // cad ifc takeoff
  std::shared_ptr<DocSession> session;
  std::optional<std::string> path;
};

inline constexpr std::size_t kMaxOpen = 16;
inline constexpr std::uint64_t kMaxFile = 500ULL * 1024 * 1024;

/// tools.mjs createEnv: the roots, the open files, the paths this session opened or wrote.
struct Env {
  Roots roots;
  std::string undo_prefix = "Assistant: ";
  std::map<std::string, OpenDoc> docs;  // handle → file (order is irrelevant: handles are looked up)
  std::uint64_t seq = 0;
  std::set<std::string> mine;
};

/// tools.mjs readFile: the real path and the bytes (≤ kMaxFile).
struct ReadFile {
  std::string real;
  std::vector<std::byte> bytes;
};
[[nodiscard]] std::expected<ReadFile, ToolError> read_file(Env& env, std::string_view p);
/// tools.mjs keep: a new handle for an open file.
[[nodiscard]] std::expected<std::string, ToolError> keep(Env& env, std::string kind, std::shared_ptr<DocSession> s,
                                                         std::optional<std::string> real);
/// tools.mjs get: the open file of a handle (of that kind, when given).
[[nodiscard]] std::expected<OpenDoc*, ToolError> get(Env& env, std::string_view id, std::string_view kind = {});
/// tools.mjs write: atomically, inside the roots; `ext` = the extension it must have.
[[nodiscard]] std::expected<std::string, ToolError> write_file(Env& env, std::string_view p,
                                                               std::span<const std::byte> bytes, bool overwrite,
                                                               std::string_view ext = {});
/// tools.mjs extOf: the lower-case extension without the dot.
[[nodiscard]] std::string ext_of(std::string_view p);

/// The schema pieces every app's tools share (tools.mjs S and obj).
[[nodiscard]] base::Value schema_doc();
[[nodiscard]] base::Value schema_path();
[[nodiscard]] base::Value schema_offset();
[[nodiscard]] base::Value schema_overwrite();
[[nodiscard]] base::Value schema_object(base::Value properties, std::vector<std::string> required = {});

/// The tools that belong to no app, in 0.5's words; the CLI registers them in 0.5's order (files_list first,
/// doc_close after the drawing tools).
[[nodiscard]] Tool files_list_tool(Env& env);
[[nodiscard]] Tool doc_close_tool(Env& env);

}  // namespace cc::mcp
