// SPDX-License-Identifier: Apache-2.0
// cc::live — L2. The decisions of a Studio Live session: desktop-shell live/session.rs `check` / `commit` (Studio 0.5,
// desktop-shell d132555; docs/LIVE-PROTOCOL.md § 6, § 9) — who may edit, locks, holds, the order, and whether an edit
// commits or is refused, from its `touched` keys alone. The server never reads an app's ops; this core never sees them.
// No sockets and no clock of its own (the caller passes `now_ms`); the shared scenarios of tools/live-test/vectors.json
// run on it.
//
// Rules, in order: role (only host, editor and assistant edit) → locks (any touched key, any mode, the host's edits
// too) → holds (someone else's hold stops `s`, `d` and guarded reads `r` + `g`) → each key:
//   p  `deleted` if k is deleted; with `r` and `f`, `changed` unless f was last written at seq r (0 = never).
//   c  `exists` if k is known (any commit wrote it) and not deleted.
//   d  `deleted` if deleted; with `r`, `changed` if a structural commit (any mode but p and r) wrote k after r.
//   s, r (and any other mode)  `deleted` if deleted; with `r`, `changed` unless rev(k) = r.
// `by` is the key's last writer (for p's `changed`: the property's, none when never written). All or nothing: every
// key is checked against the state before the edit; then all are applied with one seq. After a commit: p sets rev(k)
// and rev(k.f); every other mode but r sets rev(k), the structural rev, deleted = (mode is d); c clears k's property
// revs; d drops a hold on k (returned, for `holds {clear}`).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cc/live/msgpack.hpp"

namespace cc::live {

enum class Role : std::uint8_t { host, editor, viewer, assistant, replica };

[[nodiscard]] std::string_view role_name(Role r) noexcept;
[[nodiscard]] std::optional<Role> parse_role(std::string_view s) noexcept;
/// host, editor and assistant edit; viewer and replica do not.
[[nodiscard]] bool can_edit(Role r) noexcept;

/// One `touched` entry {k, m, f?, r?, g?}.
struct Touch {
  std::string k;
  char m = 's';                    // p s c d r (session.rs: the first character of `m`, `s` when absent)
  std::optional<std::string> f;    // the property
  std::optional<std::uint64_t> r;  // the version the edit is based on (an undo / redo, a structural edit)
  bool g = false;                  // a guarded read
  friend bool operator==(const Touch&, const Touch&) = default;
};

/// From a `touched` item as session.rs reads it: nullopt without a string `k`; `f` when a string; `r` when a
/// non-negative integer; `g` when true or a positive integer.
[[nodiscard]] std::optional<Touch> touch_from(const Mp& item);
/// An edit's `touched` (session.rs: items without a key skipped, at most 100 000; not an array = none).
[[nodiscard]] std::vector<Touch> touched_from(const Mp* list);
/// What `commit` carries: the keys without versions ({k, m, f?}).
[[nodiscard]] Mp commit_touched(std::span<const Touch> touched);

/// An edit's outcome.
struct Decision {
  bool ok = false;
  std::uint64_t q = 0;   // the seq it committed at
  std::string reason;    // gone role locked held deleted changed exists
  std::uint64_t by = 0;  // the client whose commit or hold caused a refusal (0 = none)
};

class SessionCore {
 public:
  static constexpr std::uint64_t kHoldMs = 30'000;

  /// Adds a client (or changes its role).
  void join(std::uint64_t c, Role role);
  /// Removes a client: its holds go (returned: the keys cleared).
  std::vector<std::string> leave(std::uint64_t c);
  [[nodiscard]] std::optional<Role> role_of(std::uint64_t c) const;
  void set_role(std::uint64_t c, Role role);

  /// Hold keys for `c` at `now_ms` (a hold lasts kHoldMs unless refreshed). Keys someone else holds are skipped.
  /// Returns the keys now held by c that were not before (for `holds {set}`).
  std::vector<std::string> hold(std::uint64_t c, std::span<const std::string> keys, std::uint64_t now_ms);
  /// Releases c's holds on those keys (all of them when `keys` is empty and `all`). Returns the keys cleared.
  std::vector<std::string> release(std::uint64_t c, std::span<const std::string> keys, bool all = false);
  /// Drops holds not refreshed within kHoldMs. Returns the keys cleared.
  std::vector<std::string> expire(std::uint64_t now_ms);
  /// The holder of `key`, or 0.
  [[nodiscard]] std::uint64_t holder(std::string_view key) const;
  /// The holds table (key → client), sorted by key.
  [[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> holds() const;

  /// Host only (the caller checks the role). Return the keys added / removed.
  std::vector<std::string> lock(std::span<const std::string> keys);
  std::vector<std::string> unlock(std::span<const std::string> keys);
  [[nodiscard]] bool locked(std::string_view key) const;
  [[nodiscard]] std::vector<std::string> locks() const;

  /// Decides an edit by client c; on success it is committed at the next seq. `cleared` gets the holds a delete
  /// dropped.
  Decision edit(std::uint64_t c, std::span<const Touch> touched, std::vector<std::string>* cleared = nullptr);
  /// Only checks (as `edit` would decide now), changes nothing.
  [[nodiscard]] Decision check(std::uint64_t c, std::span<const Touch> touched) const;

  /// The session's seq (the last commit's).
  [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }
  /// rev(k): the seq of the last commit that wrote k in any way (0 = as in the file).
  [[nodiscard]] std::uint64_t rev(std::string_view key) const;
  /// Every key's rev, sorted by key (what `revs` sends before a snapshot).
  [[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> revs() const;

 private:
  struct Key {
    std::uint64_t rev = 0;
    std::uint64_t by = 0;
    bool deleted = false;
  };
  struct Write {
    std::uint64_t q = 0;
    std::uint64_t by = 0;
  };
  struct Hold {
    std::uint64_t c = 0;
    std::uint64_t at = 0;
  };
  [[nodiscard]] const Key* key(std::string_view k) const;

  std::map<std::uint64_t, Role> clients_;
  std::map<std::string, Key, std::less<>> keys_;
  std::map<std::string, Write, std::less<>> props_;  // "k\0f" → the property's last write
  std::map<std::string, std::uint64_t, std::less<>> struct_rev_;
  std::map<std::string, Hold, std::less<>> holds_;
  std::map<std::string, bool, std::less<>> locks_;
  std::uint64_t seq_ = 0;
};

}  // namespace cc::live
