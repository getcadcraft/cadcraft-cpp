// SPDX-License-Identifier: Apache-2.0
// cc::addons — L2. Add-on packages and their signatures (CadCraft 0.5 addons/trust.js; docs/ADDONS-SPEC.md § 8–9).
// An add-on is a package of files (scripts and data run through the command API — never native code: nothing here
// loads a library); getcadcraft.com signs a version when staff approve it, and an app installs only a package signed by
// a key it trusts, for exactly those files and that manifest.
//
// The package digest (UTF-8), what the signature covers:
//   cadcraft-addon-package 1
//   <id> <version>
//   <sha256 hex of the file>  <path>          one line per file, sorted by path
// Ed25519 (libsodium's strict RFC 8032 verification, as WebCrypto's).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/value.hpp"

namespace cc::addons {

inline constexpr std::string_view kPackageFormat = "cadcraft-addon-package";
inline constexpr int kPackageVersion = 1;
inline constexpr std::size_t kMaxFiles = 200;
inline constexpr std::size_t kMaxBytes = std::size_t{5} * 1024 * 1024;

using Bytes = std::vector<std::uint8_t>;

struct Signature {
  std::string alg;
  std::string key;
  std::string sig;  // base64
};

struct Package {
  base::Value manifest;
  std::map<std::string, Bytes> files;  // path → bytes, sorted by path (byte order = UTF-16 order for these ASCII paths)
  std::optional<Signature> signature;
};

/// getcadcraft.com's add-on signing keys (public; base64), by key id.
[[nodiscard]] const std::map<std::string, std::string, std::less<>>& trusted_keys();

/// PATH_RE /^[A-Za-z0-9_][A-Za-z0-9_./-]{0,200}$/ with no ".", ".." or empty segment.
[[nodiscard]] bool good_path(std::string_view p);
/// JSON with keys sorted at every level (manifest equality; trust.js canonical).
[[nodiscard]] std::string canonical(const base::Value& v);
/// The text a package's signature covers; nullopt on a bad path or too many / too big files.
[[nodiscard]] std::optional<std::string> package_digest(const Package& pkg);

struct Verdict {
  bool ok = false;
  std::string reason;  // unsigned key files manifest signature
};
/// Signed by a key we trust, for exactly these files and this manifest?
[[nodiscard]] Verdict verify_package(const Package& pkg,
                                     const std::map<std::string, std::string, std::less<>>& keys = trusted_keys());

/// A package as the directory serves it (files base64) → the package, or the reason (format, files).
struct ReadResult {
  bool ok = false;
  std::optional<Package> pkg;
  std::string reason;
};
[[nodiscard]] ReadResult read_package_json(const base::Value& obj);

/// atob(): base64 (standard alphabet, padding optional; whitespace refused), or nullopt.
[[nodiscard]] std::optional<Bytes> from_base64(std::string_view s);
/// Ed25519 verification of a 64-byte signature by a 32-byte key.
[[nodiscard]] bool ed25519_verify(std::span<const std::uint8_t> pub, std::span<const std::uint8_t> sig,
                                  std::span<const std::uint8_t> msg);

}  // namespace cc::addons
