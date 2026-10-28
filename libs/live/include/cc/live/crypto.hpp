// SPDX-License-Identifier: Apache-2.0
// cc::live — L2. Studio Live's handshake and frame encryption (docs/LIVE-CRYPTO.md; CadCraft 0.5 live-crypto.js and
// desktop-shell live/crypto.rs, which both give tools/live-crypto/vectors.json). Every primitive comes from libsodium
// (ISC; maintained, fuzzed upstream): Ed25519 group arithmetic for SPAKE2, SHA-256, HMAC-SHA256, ChaCha20-Poly1305
// (IETF), Ed25519 signatures, the OS random generator. Secrets are compared in constant time (sodium_memcmp).
//
// Handshake (joiner = SPAKE2 side A, host = side B; the RustCrypto spake2 crate's Ed25519 scheme byte for byte):
//   JoinerHandshake j(secret, context);          send(j.message())                 // 1. A (33 bytes)
//   HostHandshake h(secret, context);            send(h.receive(a))                // 2. B (33 bytes)
//   send(j.receive(b))                                                              // 3. joiner's confirmation (32)
//   auto [confirm, session] = h.finish(c);       send(confirm)                     // 4. host's confirmation (32)
//   Session s = j.finish(confirm)
// A confirmation that does not match is `wrong_code` (on the host: a wrong try to count).
// Frames: header (version 1 | kind | epoch | counter u64 BE) ‖ ChaCha20-Poly1305(body, aad = header); each direction
// its own key and counter; only the exact next counter opens; the host rotates keys with rekey().
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cc::live {

using Bytes = std::vector<std::uint8_t>;
using Key = std::array<std::uint8_t, 32>;

/// LiveCryptoError's codes: bad_message wrong_code bad_frame order role exhausted state.
struct CryptoError {
  std::string code;
};
template <class T>
using CryptoResult = std::expected<T, CryptoError>;

inline constexpr std::size_t kPakeLen = 33;
inline constexpr std::size_t kConfirmLen = 32;
inline constexpr std::size_t kHeaderLen = 11;
inline constexpr std::size_t kTagLen = 16;
inline constexpr std::string_view kCodeAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/// libsodium's start-up (thread-safe, idempotent); false when the library cannot run here.
[[nodiscard]] bool crypto_ready();

class Session {
 public:
  enum class Side : std::uint8_t { joiner, host };
  struct Opened {
    bool rekey = false;
    Bytes body;
  };

  /// Encrypts one frame body (a MessagePack envelope).
  [[nodiscard]] CryptoResult<Bytes> seal(std::span<const std::uint8_t> body);
  /// Host only: the rekey frame to send before any other; both directions move to the next keys.
  [[nodiscard]] CryptoResult<Bytes> rekey();
  /// Decrypts one frame. Any error means: close the connection.
  [[nodiscard]] CryptoResult<Opened> open(std::span<const std::uint8_t> frame);
  /// 32 bytes for one purpose, the same on both ends and never sent.
  [[nodiscard]] Key exporter(std::string_view label) const;
  [[nodiscard]] Side side() const noexcept { return side_; }
  /// How many rotations the send direction has had.
  [[nodiscard]] int epoch() const noexcept { return send_.epoch; }

  Session(Side side, const Key& j2h, const Key& h2j, const Key& resume);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&& other) noexcept;
  Session& operator=(Session&& other) noexcept;

 private:
  struct Direction {
    Key key{};
    std::uint8_t dir = 0;
    int epoch = 0;
    std::uint64_t counter = 0;
  };
  [[nodiscard]] CryptoResult<Bytes> seal_kind(std::uint8_t kind, std::span<const std::uint8_t> body);
  [[nodiscard]] static CryptoResult<void> next(Direction& d);

  Side side_;
  Direction send_;
  Direction recv_;
  int recv_max_ = 0;  // host: the newest epoch it announced
  Key resume_{};
};

class JoinerHandshake {
 public:
  /// `secret`: a short code's 5 characters (parse_code) or an invite link's bytes (parse_link_secret). `rng`: 64
  /// fixed bytes, tests only (else the OS's).
  JoinerHandshake(std::span<const std::uint8_t> secret, std::string_view context,
                  std::optional<std::array<std::uint8_t, 64>> rng = std::nullopt);
  ~JoinerHandshake();
  JoinerHandshake(const JoinerHandshake&) = delete;
  JoinerHandshake& operator=(const JoinerHandshake&) = delete;
  JoinerHandshake(JoinerHandshake&&) noexcept;
  JoinerHandshake& operator=(JoinerHandshake&&) noexcept;

  /// Message 1, or the error the start met (bad_message).
  [[nodiscard]] CryptoResult<Bytes> message() const;
  /// Takes the host's B message; returns the confirmation to send.
  [[nodiscard]] CryptoResult<Bytes> receive(std::span<const std::uint8_t> host_msg);
  /// Checks the host's confirmation (wrong_code) and returns the session.
  [[nodiscard]] CryptoResult<Session> finish(std::span<const std::uint8_t> host_confirm);

 private:
  struct State;
  std::unique_ptr<State> st_;  // its secrets are wiped in the destructor
};

class HostHandshake {
 public:
  HostHandshake(std::span<const std::uint8_t> secret, std::string_view context);
  ~HostHandshake();
  HostHandshake(const HostHandshake&) = delete;
  HostHandshake& operator=(const HostHandshake&) = delete;
  HostHandshake(HostHandshake&&) noexcept;
  HostHandshake& operator=(HostHandshake&&) noexcept;

  /// Takes the joiner's A message; returns the B message. `rng`: tests only.
  [[nodiscard]] CryptoResult<Bytes> receive(std::span<const std::uint8_t> joiner_msg,
                                            std::optional<std::array<std::uint8_t, 64>> rng = std::nullopt);
  /// Checks the joiner's confirmation (wrong_code: count it). Returns the confirmation to send and the session.
  [[nodiscard]] CryptoResult<std::pair<Bytes, Session>> finish(std::span<const std::uint8_t> joiner_confirm);

 private:
  struct State;
  std::unique_ptr<State> st_;
};

/// A short code as typed (case, spaces and the dash ignored; O → 0, I and L → 1).
struct Code {
  std::string tag;
  std::string secret;   // the 5 normalized characters
  std::string context;  // "lan:<tag>"
};
[[nodiscard]] std::optional<Code> parse_code(std::string_view code);
/// A new short code, "K7-Q4M2X".
[[nodiscard]] std::string new_code();
/// The LAN tag of an invite-link room (8 Crockford characters).
[[nodiscard]] std::string room_tag(std::string_view room);
/// An invite link's fragment (base64url, no padding; a leading '#' is fine) → 16–32 secret bytes.
[[nodiscard]] std::optional<Bytes> parse_link_secret(std::string_view fragment);
/// A new 128-bit invite secret as a link fragment (22 characters).
[[nodiscard]] std::string new_link_secret();
/// base64url without padding.
[[nodiscard]] std::string to_b64url(std::span<const std::uint8_t> b);

/// verifyAssertion's answer.
struct Assertion {
  bool ok = false;
  std::string reason;  // format key signature expired audience
  std::string name;
  std::string sub;
  bool verified = false;
  std::optional<std::string> pic;
};
struct AssertionKey {
  std::string kid;
  std::string key;  // base64url, 32 bytes
};
/// getcadcraft.com's pinned keys (live-crypto.js ASSERTION_KEYS).
[[nodiscard]] std::span<const AssertionKey> assertion_keys();
/// Checks a joiner's signed-in name. `now` in seconds.
[[nodiscard]] Assertion verify_assertion(std::string_view token, std::string_view aud, double now,
                                         std::span<const AssertionKey> keys = assertion_keys(), double skew = 60);
/// base64url of exporter("assertion").
[[nodiscard]] std::string assertion_audience(const Session& s);
/// Does a picture match the hash in a checked assertion?
[[nodiscard]] bool picture_matches(std::span<const std::uint8_t> bytes, std::string_view pic);

}  // namespace cc::live
