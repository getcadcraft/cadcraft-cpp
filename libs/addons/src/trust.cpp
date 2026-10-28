// SPDX-License-Identifier: Apache-2.0
// addons/trust.js, ported (libsodium for SHA-256 and Ed25519). See trust.hpp.
#include "cc/addons/trust.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#if CC_ADDONS_SODIUM
#include <sodium.h>
#endif

#include "cc/base/js.hpp"
#include "cc/base/json.hpp"

namespace cc::addons {

using base::Value;

namespace {

#if CC_ADDONS_SODIUM
bool ready() {
  static const bool ok = sodium_init() >= 0;
  return ok;
}

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> b) {
  std::array<std::uint8_t, crypto_hash_sha256_BYTES> h{};
  crypto_hash_sha256(h.data(), b.data(), b.size());
  return h;
}
#else
/// FIPS 180-4 SHA-256 (the web build has no libsodium; the digest still names the files).
std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> b) {
  static constexpr std::array<std::uint32_t, 64> k = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::vector<std::uint8_t> m(b.begin(), b.end());
  const std::uint64_t bits = static_cast<std::uint64_t>(b.size()) * 8;
  m.push_back(0x80);
  while (m.size() % 64 != 56) {
    m.push_back(0);
  }
  for (int i = 7; i >= 0; --i) {
    m.push_back(static_cast<std::uint8_t>(bits >> (static_cast<unsigned>(i) * 8U)));
  }
  const auto rotr = [](std::uint32_t x, unsigned n) { return (x >> n) | (x << (32U - n)); };
  for (std::size_t at = 0; at < m.size(); at += 64) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
      w.at(i) = (std::uint32_t{m[at + (4 * i)]} << 24U) | (std::uint32_t{m[at + (4 * i) + 1]} << 16U) |
                (std::uint32_t{m[at + (4 * i) + 2]} << 8U) | std::uint32_t{m[at + (4 * i) + 3]};
    }
    for (std::size_t i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w.at(i - 15), 7) ^ rotr(w.at(i - 15), 18) ^ (w.at(i - 15) >> 3U);
      const std::uint32_t s1 = rotr(w.at(i - 2), 17) ^ rotr(w.at(i - 2), 19) ^ (w.at(i - 2) >> 10U);
      w.at(i) = w.at(i - 16) + s0 + w.at(i - 7) + s1;
    }
    auto v = h;
    for (std::size_t i = 0; i < 64; ++i) {
      const std::uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
      const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
      const std::uint32_t t1 = v[7] + s1 + ch + k.at(i) + w.at(i);
      const std::uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
      const std::uint32_t mj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
      const std::uint32_t t2 = s0 + mj;
      v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
    }
    for (std::size_t i = 0; i < 8; ++i) {
      h.at(i) += v.at(i);
    }
  }
  std::array<std::uint8_t, 32> out{};
  for (std::size_t i = 0; i < 8; ++i) {
    for (std::size_t j = 0; j < 4; ++j) {
      out.at((4 * i) + j) = static_cast<std::uint8_t>(h.at(i) >> (24U - (8U * j)));
    }
  }
  return out;
}
#endif

std::string sha256_hex(std::span<const std::uint8_t> b) {
  const auto h = sha256(b);
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string s;
  for (const std::uint8_t x : h) {
    s += kHex[x >> 4U];
    s += kHex[x & 15U];
  }
  return s;
}

/// Array#sort's default order for strings: by UTF-16 code units.
void sort_utf16(std::vector<std::string>& v) {
  std::vector<std::pair<std::vector<std::uint16_t>, std::string>> keyed;
  keyed.reserve(v.size());
  for (auto& s : v) {
    keyed.emplace_back(base::to_utf16(s), std::move(s));
  }
  std::ranges::sort(keyed, [](const auto& a, const auto& b) { return a.first < b.first; });
  v.clear();
  for (auto& k : keyed) {
    v.push_back(std::move(k.second));
  }
}

// NOLINTNEXTLINE(misc-no-recursion) — manifests come through the JSON parser's depth cap
void write_canonical(const Value& v, std::string& out) {
  if (v.is_array()) {
    out += '[';
    const auto& a = v.as_array();
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (i != 0) {
        out += ',';
      }
      write_canonical(a[i], out);
    }
    out += ']';
    return;
  }
  if (v.is_object()) {
    Value copy = v;
    std::vector<std::string> keys;
    for (const auto& m : copy.as_object()) {
      if (!m.value.is_undefined()) {
        keys.push_back(m.key);
      }
    }
    sort_utf16(keys);
    keys.erase(std::ranges::unique(keys).begin(), keys.end());
    out += '{';
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (i != 0) {
        out += ',';
      }
      base::append_json_string(out, keys[i]);
      out += ':';
      write_canonical(copy.get(keys[i]), out);
    }
    out += '}';
    return;
  }
  switch (v.kind()) {
    case Value::Kind::null: out += "null"; return;
    case Value::Kind::boolean: out += v.as_bool() ? "true" : "false"; return;
    case Value::Kind::number:
      if (std::isfinite(v.as_number())) {
        base::append_number(out, v.as_number());
      } else {
        out += "null";
      }
      return;
    case Value::Kind::string: base::append_json_string(out, v.as_string()); return;
    default: out += "null"; return;  // undefined in an array (JSON.stringify gives undefined; not in parsed JSON)
  }
}

}  // namespace

const std::map<std::string, std::string, std::less<>>& trusted_keys() {
  static const std::map<std::string, std::string, std::less<>> kKeys = {
      {"cc-addons-2026-1", "osQRAZ6eDOd+PTxSKjDJ7eInmbEqm6pLGfkek4D+yX8="},
  };
  return kKeys;
}

bool good_path(std::string_view p) {
  const auto first = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  };
  const auto rest = [&first](char c) { return first(c) || c == '.' || c == '/' || c == '-'; };
  if (p.empty() || p.size() > 201 || !first(p[0]) || !std::ranges::all_of(p.substr(1), rest)) {
    return false;
  }
  std::size_t a = 0;
  for (;;) {
    const std::size_t b = p.find('/', a);
    const std::string_view seg = p.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a);
    if (seg.empty() || seg == "." || seg == "..") {
      return false;
    }
    if (b == std::string_view::npos) {
      return true;
    }
    a = b + 1;
  }
}

std::string canonical(const Value& v) {
  std::string out;
  write_canonical(v, out);
  return out;
}

std::optional<std::string> package_digest(const Package& pkg) {
  std::vector<std::string> paths;
  paths.reserve(pkg.files.size());
  for (const auto& [p, b] : pkg.files) {
    paths.push_back(p);
  }
  sort_utf16(paths);
  if (paths.empty() || paths.size() > kMaxFiles) {
    return std::nullopt;
  }
  std::string text = std::string(kPackageFormat) + " " + std::to_string(kPackageVersion) + "\n" +
                     base::js_string(pkg.manifest.get("id")) + " " + base::js_string(pkg.manifest.get("version")) +
                     "\n";
  std::size_t total = 0;
  for (const std::string& p : paths) {
    if (!good_path(p)) {
      return std::nullopt;
    }
    const Bytes& b = pkg.files.at(p);
    total += b.size();
    if (total > kMaxBytes) {
      return std::nullopt;
    }
    text += sha256_hex(b) + "  " + p + "\n";
  }
  return text;
}

std::optional<Bytes> from_base64(std::string_view s) {
  // the forgiving-base64 decode of atob(): ASCII whitespace removed, then one or two '=' at the end of a multiple of 4
  std::string t;
  for (const char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r') {
      continue;
    }
    t += c;
  }
  if (t.size() % 4 == 0 && t.ends_with("==")) {
    t.resize(t.size() - 2);
  } else if (t.size() % 4 == 0 && t.ends_with("=")) {
    t.pop_back();
  }
  if (t.size() % 4 == 1) {
    return std::nullopt;
  }
  static constexpr std::string_view kA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  Bytes out;
  unsigned bits = 0;
  unsigned n = 0;
  for (const char c : t) {
    const std::size_t v = kA.find(c);
    if (v == std::string_view::npos) {
      return std::nullopt;
    }
    bits = (bits << 6U) | static_cast<unsigned>(v);
    n += 6;
    if (n >= 8) {
      n -= 8;
      out.push_back(static_cast<std::uint8_t>((bits >> n) & 255U));
      bits &= (1U << n) - 1;
    }
  }
  return out;
}

bool ed25519_verify(std::span<const std::uint8_t> pub, std::span<const std::uint8_t> sig,
                    std::span<const std::uint8_t> msg) {
#if CC_ADDONS_SODIUM
  if (!ready() || pub.size() != crypto_sign_PUBLICKEYBYTES || sig.size() != crypto_sign_BYTES) {
    return false;
  }
  return crypto_sign_verify_detached(sig.data(), msg.data(), msg.size(), pub.data()) == 0;
#else
  (void)pub;
  (void)sig;
  (void)msg;
  return false;  // the web build checks no signature (every package is refused as code anyway)
#endif
}

Verdict verify_package(const Package& pkg, const std::map<std::string, std::string, std::less<>>& keys) {
  if (!pkg.signature || pkg.signature->alg != "Ed25519") {
    return {false, "unsigned"};
  }
  const auto k = keys.find(pkg.signature->key);
  if (k == keys.end()) {
    return {false, "key"};
  }
  const auto manifest_file = pkg.files.find("addon.json");
  if (manifest_file == pkg.files.end()) {
    return {false, "manifest"};
  }
  // TextDecoder (U+FFFD for bad UTF-8, a BOM dropped) then JSON.parse
  std::string text;
  {
    std::span<const std::uint8_t> b = manifest_file->second;
    if (b.size() >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf) {
      b = b.subspan(3);
    }
    text.assign(b.begin(), b.end());
  }
  auto in_file = base::parse_json(text);
  if (!in_file) {
    return {false, "manifest"};
  }
  if (canonical(*in_file) != canonical(pkg.manifest)) {
    return {false, "manifest"};
  }
  const auto digest = package_digest(pkg);
  if (!digest) {
    return {false, "files"};
  }
  const auto pub = from_base64(k->second);
  const auto sig = from_base64(pkg.signature->sig);
  if (!pub || !sig) {
    return {false, "signature"};
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const std::span<const std::uint8_t> msg(reinterpret_cast<const std::uint8_t*>(digest->data()), digest->size());
  return ed25519_verify(*pub, *sig, msg) ? Verdict{true, ""} : Verdict{false, "signature"};
}

ReadResult read_package_json(const Value& obj) {
  const auto refuse = [](const char* why) { return ReadResult{false, std::nullopt, why}; };
  if (!obj.is_object() || !obj.get("format").is_string() || obj.get("format").as_string() != kPackageFormat) {
    return refuse("format");
  }
  if (!obj.get("version").is_number() || obj.get("version").as_number() != kPackageVersion) {
    return refuse("format");
  }
  const Value manifest = obj.get("manifest");
  const Value files = obj.get("files");
  if (!manifest.truthy() || !(manifest.is_object() || manifest.is_array()) || !files.truthy() ||
      !(files.is_object() || files.is_array())) {
    return refuse("format");
  }
  Package pkg;
  pkg.manifest = manifest;
  std::vector<std::pair<std::string, Value>> entries;
  if (files.is_array()) {
    for (std::size_t i = 0; i < files.as_array().size(); ++i) {
      entries.emplace_back(std::to_string(i), files.as_array()[i]);
    }
  } else {
    const Value& copy = files;
    for (const base::Member* m : base::js_members(copy.as_object())) {
      entries.emplace_back(m->key, m->value);
    }
  }
  if (entries.empty() || entries.size() > kMaxFiles) {
    return refuse("files");
  }
  std::size_t total = 0;
  for (const auto& [p, v] : entries) {
    if (!good_path(p) || !v.is_string()) {
      return refuse("files");
    }
    auto u = from_base64(v.as_string());
    if (!u) {
      return refuse("files");
    }
    total += u->size();
    if (total > kMaxBytes) {
      return refuse("files");
    }
    pkg.files[p] = std::move(*u);
  }
  if (const Value s = obj.get("signature"); s.truthy() && (s.is_object() || s.is_array())) {
    const auto str = [&s](const char* k) {
      const Value x = s.get(k);
      return x.truthy() ? base::js_string(x) : std::string();
    };
    pkg.signature = Signature{str("alg"), str("key"), str("sig")};
  }
  return ReadResult{true, std::move(pkg), ""};
}

}  // namespace cc::addons
