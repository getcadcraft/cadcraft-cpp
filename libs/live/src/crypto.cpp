// SPDX-License-Identifier: Apache-2.0
// live-crypto.js, ported on libsodium. See crypto.hpp.
#include "cc/live/crypto.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include <sodium.h>

#include "cc/base/json.hpp"
#include "cc/base/value.hpp"

namespace cc::live {

namespace {

constexpr std::uint8_t kVersion = 1;
constexpr std::uint8_t kKindData = 0;
constexpr std::uint8_t kKindRekey = 1;
constexpr std::uint8_t kDirJoiner = 0x4a;  // 'J'
constexpr std::uint8_t kDirHost = 0x48;    // 'H'
constexpr std::string_view kLabel = "cadcraft-live/1";

using Point = std::array<std::uint8_t, crypto_core_ed25519_BYTES>;
using Scalar = std::array<std::uint8_t, crypto_core_ed25519_SCALARBYTES>;

// The spake2 crate's Ed25519 M and N (python-spake2's ParamsEd25519).
constexpr Point kM = {0x15, 0xcf, 0xd1, 0x8e, 0x38, 0x59, 0x52, 0x98, 0x2b, 0x6a, 0x8f, 0x8c, 0x78, 0x54, 0x96, 0x3b,
                      0x58, 0xe3, 0x43, 0x88, 0xc8, 0xe6, 0xda, 0xe8, 0x91, 0xdb, 0x75, 0x64, 0x81, 0xa0, 0x23, 0x12};
constexpr Point kN = {0xf0, 0x4f, 0x2e, 0x7e, 0xb7, 0x34, 0xb2, 0xa8, 0xf8, 0xb4, 0x72, 0xea, 0xf9, 0xc3, 0xc6, 0x32,
                      0x57, 0x6a, 0xc6, 0x4a, 0xea, 0x65, 0x0b, 0x49, 0x6a, 0x8a, 0x20, 0xff, 0x00, 0xe5, 0x83, 0xc3};

std::unexpected<CryptoError> fail(const char* code) {
  return std::unexpected(CryptoError{code});
}

std::span<const std::uint8_t> as_bytes(std::string_view s) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

void append(Bytes& out, std::span<const std::uint8_t> b) {
  out.insert(out.end(), b.begin(), b.end());
}

Key sha256(std::span<const std::uint8_t> b) {
  Key k{};
  crypto_hash_sha256(k.data(), b.data(), b.size());
  return k;
}

Key hmac(std::span<const std::uint8_t> key, std::span<const std::uint8_t> msg) {
  crypto_auth_hmacsha256_state st;
  crypto_auth_hmacsha256_init(&st, key.data(), key.size());
  crypto_auth_hmacsha256_update(&st, msg.data(), msg.size());
  Key out{};
  crypto_auth_hmacsha256_final(&st, out.data());
  sodium_memzero(&st, sizeof st);
  return out;
}

/// HKDF-SHA256 (RFC 5869): `len` bytes (≤ 255 × 32).
Bytes hkdf(std::span<const std::uint8_t> ikm, std::span<const std::uint8_t> salt, std::span<const std::uint8_t> info,
           std::size_t len) {
  const Key zero{};
  const Key prk = hmac(salt.empty() ? std::span<const std::uint8_t>(zero) : salt, ikm);
  Bytes out;
  Bytes t;
  for (std::uint8_t i = 1; out.size() < len; ++i) {
    Bytes block = t;
    append(block, info);
    block.push_back(i);
    const Key k = hmac(prk, block);
    t.assign(k.begin(), k.end());
    append(out, t);
    sodium_memzero(block.data(), block.size());
  }
  out.resize(len);
  return out;
}

/// HKDF-SHA256(salt "cadcraft-live/1", ikm, info, 32 bytes).
Key derive(std::span<const std::uint8_t> ikm, std::string_view info) {
  const Bytes b = hkdf(ikm, as_bytes(kLabel), as_bytes(info), 32);
  Key k{};
  std::ranges::copy(b, k.begin());
  return k;
}

/// The password scalar: HKDF-SHA256(salt "", ikm pw, info "SPAKE2 pw", 48 bytes) read big-endian, mod L.
std::optional<Scalar> pw_scalar(std::span<const std::uint8_t> pw) {
  const Bytes h = hkdf(pw, {}, as_bytes("SPAKE2 pw"), 48);
  std::array<std::uint8_t, 64> le{};
  for (std::size_t i = 0; i < 48; ++i) {
    le[i] = h[47 - i];
  }
  Scalar s{};
  crypto_core_ed25519_scalar_reduce(s.data(), le.data());
  sodium_memzero(le.data(), le.size());
  if (sodium_is_zero(s.data(), s.size()) != 0) {
    return std::nullopt;
  }
  return s;
}

struct Spake {
  Scalar x{};
  Scalar w{};
  Bytes msg;  // 'A' | 'B' ‖ X
};

/// side 'A' or 'B'; rng: 64 bytes read little-endian mod L (curve25519-dalek's Scalar::random).
CryptoResult<Spake> spake_start(char side, std::span<const std::uint8_t> pw,
                                const std::optional<std::array<std::uint8_t, 64>>& rng) {
  Spake st;
  std::array<std::uint8_t, 64> r{};
  if (rng) {
    r = *rng;
  } else {
    randombytes_buf(r.data(), r.size());
  }
  crypto_core_ed25519_scalar_reduce(st.x.data(), r.data());
  sodium_memzero(r.data(), r.size());
  if (sodium_is_zero(st.x.data(), st.x.size()) != 0) {
    return fail("bad_message");
  }
  const auto w = pw_scalar(pw);
  if (!w) {
    return fail("bad_message");
  }
  st.w = *w;
  Point xg{};
  Point wm{};
  Point big_x{};
  if (crypto_scalarmult_ed25519_base_noclamp(xg.data(), st.x.data()) != 0 ||
      crypto_scalarmult_ed25519_noclamp(wm.data(), st.w.data(), side == 'A' ? kM.data() : kN.data()) != 0 ||
      crypto_core_ed25519_add(big_x.data(), xg.data(), wm.data()) != 0) {
    return fail("bad_message");
  }
  st.msg.push_back(side == 'A' ? 0x41 : 0x42);
  append(st.msg, big_x);
  return st;
}

CryptoResult<Key> spake_finish(char side, const Spake& st, std::span<const std::uint8_t> pw,
                               std::span<const std::uint8_t> id_a, std::span<const std::uint8_t> id_b,
                               std::span<const std::uint8_t> other) {
  if (other.size() != kPakeLen || other[0] != (side == 'A' ? 0x42 : 0x41)) {
    return fail("bad_message");
  }
  const auto y = other.subspan(1);
  if (crypto_core_ed25519_is_valid_point(y.data()) == 0) {
    return fail("bad_message");
  }
  Scalar neg_w{};
  crypto_core_ed25519_scalar_negate(neg_w.data(), st.w.data());  // L − w
  Point t{};
  Point sum{};
  Point k{};
  if (crypto_scalarmult_ed25519_noclamp(t.data(), neg_w.data(), side == 'A' ? kN.data() : kM.data()) != 0 ||
      crypto_core_ed25519_add(sum.data(), y.data(), t.data()) != 0 ||
      crypto_scalarmult_ed25519_noclamp(k.data(), st.x.data(), sum.data()) != 0) {
    return fail("bad_message");
  }
  const auto mine = std::span<const std::uint8_t>(st.msg).subspan(1);
  const auto first = side == 'A' ? mine : y;
  const auto second = side == 'A' ? y : mine;
  Bytes all;
  append(all, sha256(pw));
  append(all, sha256(id_a));
  append(all, sha256(id_b));
  append(all, first);
  append(all, second);
  append(all, k);
  const Key out = sha256(all);
  sodium_memzero(all.data(), all.size());
  sodium_memzero(k.data(), k.size());
  return out;
}

std::pair<Bytes, Bytes> identities(std::string_view context) {
  Bytes a(kLabel.begin(), kLabel.end());
  Bytes b = a;
  append(a, as_bytes(" joiner "));
  append(a, as_bytes(context));
  append(b, as_bytes(" host "));
  append(b, as_bytes(context));
  return {a, b};
}

struct Agreed {
  Bytes a;
  Bytes b;
  Key k{};
  [[nodiscard]] Key confirm(bool joiner) const {
    const Key key = derive(k, joiner ? "confirm joiner" : "confirm host");
    Bytes msg;
    append(msg, as_bytes(joiner ? "joiner" : "host"));
    append(msg, a);
    append(msg, b);
    return hmac(key, msg);
  }
  [[nodiscard]] Session session(Session::Side side) const {
    return {side, derive(k, "frames j2h"), derive(k, "frames h2j"), derive(k, "resume")};
  }
};

bool same(std::span<const std::uint8_t> x, std::span<const std::uint8_t> y) {
  return x.size() == y.size() && sodium_memcmp(x.data(), y.data(), x.size()) == 0;
}

}  // namespace

bool crypto_ready() {
  static const bool ok = sodium_init() >= 0;
  return ok;
}

// ---------------------------------------------------------------------------------------------------- Session
Session::Session(Side side, const Key& j2h, const Key& h2j, const Key& resume) : side_(side), resume_(resume) {
  const bool joiner = side == Side::joiner;
  send_ = Direction{joiner ? j2h : h2j, joiner ? kDirJoiner : kDirHost, 0, 0};
  recv_ = Direction{joiner ? h2j : j2h, joiner ? kDirHost : kDirJoiner, 0, 0};
}

Session::~Session() {
  sodium_memzero(send_.key.data(), send_.key.size());
  sodium_memzero(recv_.key.data(), recv_.key.size());
  sodium_memzero(resume_.data(), resume_.size());
}

Session::Session(Session&& other) noexcept
    : side_(other.side_), send_(other.send_), recv_(other.recv_), recv_max_(other.recv_max_), resume_(other.resume_) {
  sodium_memzero(other.send_.key.data(), other.send_.key.size());
  sodium_memzero(other.recv_.key.data(), other.recv_.key.size());
  sodium_memzero(other.resume_.data(), other.resume_.size());
}

Session& Session::operator=(Session&& other) noexcept {
  if (this != &other) {
    side_ = other.side_;
    send_ = other.send_;
    recv_ = other.recv_;
    recv_max_ = other.recv_max_;
    resume_ = other.resume_;
    sodium_memzero(other.send_.key.data(), other.send_.key.size());
    sodium_memzero(other.recv_.key.data(), other.recv_.key.size());
    sodium_memzero(other.resume_.data(), other.resume_.size());
  }
  return *this;
}

namespace {
std::array<std::uint8_t, 12> nonce_of(std::uint8_t dir, int epoch, std::uint64_t counter) {
  std::array<std::uint8_t, 12> n{};
  n[0] = dir;
  n[1] = static_cast<std::uint8_t>(epoch);
  for (std::size_t i = 0; i < 8; ++i) {
    n[4 + i] = static_cast<std::uint8_t>((counter >> (8U * (7 - i))) & 0xffU);
  }
  return n;
}
std::array<std::uint8_t, kHeaderLen> header_of(std::uint8_t kind, int epoch, std::uint64_t counter) {
  std::array<std::uint8_t, kHeaderLen> h{};
  h[0] = kVersion;
  h[1] = kind;
  h[2] = static_cast<std::uint8_t>(epoch);
  for (std::size_t i = 0; i < 8; ++i) {
    h[3 + i] = static_cast<std::uint8_t>((counter >> (8U * (7 - i))) & 0xffU);
  }
  return h;
}
}  // namespace

CryptoResult<void> Session::next(Direction& d) {
  if (d.epoch == 255) {
    return fail("exhausted");
  }
  const Key k = derive(d.key, "next");
  sodium_memzero(d.key.data(), d.key.size());
  d.key = k;
  ++d.epoch;
  d.counter = 0;
  return {};
}

CryptoResult<Bytes> Session::seal_kind(std::uint8_t kind, std::span<const std::uint8_t> body) {
  if (send_.counter >= (std::uint64_t{1} << 63U)) {
    return fail("exhausted");
  }
  const auto header = header_of(kind, send_.epoch, send_.counter);
  const auto nonce = nonce_of(send_.dir, send_.epoch, send_.counter);
  Bytes out(kHeaderLen + body.size() + kTagLen);
  std::ranges::copy(header, out.begin());
  unsigned long long n = 0;  // NOLINT(google-runtime-int) — libsodium's type
  crypto_aead_chacha20poly1305_ietf_encrypt(std::span(out).subspan(kHeaderLen).data(), &n, body.data(), body.size(),
                                            header.data(), header.size(), nullptr, nonce.data(), send_.key.data());
  ++send_.counter;
  return out;
}

CryptoResult<Bytes> Session::seal(std::span<const std::uint8_t> body) {
  return seal_kind(kKindData, body);
}

CryptoResult<Bytes> Session::rekey() {
  if (side_ != Side::host) {
    return fail("role");
  }
  if (recv_max_ == 255) {
    return fail("exhausted");
  }
  ++recv_max_;
  auto frame = seal_kind(kKindRekey, {});
  if (!frame) {
    return frame;
  }
  if (auto r = next(send_); !r) {
    return std::unexpected(r.error());
  }
  return frame;
}

CryptoResult<Session::Opened> Session::open(std::span<const std::uint8_t> frame) {
  if (frame.size() < kHeaderLen + kTagLen || frame[0] != kVersion || frame[1] > kKindRekey) {
    return fail("bad_frame");
  }
  const std::uint8_t kind = frame[1];
  const int epoch = frame[2];
  std::uint64_t counter = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    counter = (counter << 8U) | frame[3 + i];
  }
  if (kind == kKindRekey && side_ == Side::host) {
    return fail("role");
  }
  Direction d = recv_;
  if (epoch > d.epoch && epoch <= recv_max_) {
    while (d.epoch < epoch) {
      if (auto r = next(d); !r) {
        return std::unexpected(r.error());
      }
    }
  }
  if (epoch != d.epoch || counter != d.counter) {
    sodium_memzero(d.key.data(), d.key.size());
    return fail("order");
  }
  const auto nonce = nonce_of(d.dir, d.epoch, d.counter);
  Bytes body(frame.size() - kHeaderLen - kTagLen);
  unsigned long long n = 0;  // NOLINT(google-runtime-int) — libsodium's type
  const auto ct = frame.subspan(kHeaderLen);
  if (crypto_aead_chacha20poly1305_ietf_decrypt(body.data(), &n, nullptr, ct.data(), ct.size(), frame.data(),
                                                kHeaderLen, nonce.data(), d.key.data()) != 0) {
    sodium_memzero(d.key.data(), d.key.size());
    return fail("bad_frame");
  }
  ++d.counter;
  sodium_memzero(recv_.key.data(), recv_.key.size());
  recv_ = d;
  if (kind == kKindRekey) {
    if (!body.empty()) {
      return fail("bad_frame");
    }
    if (auto r = next(recv_); !r) {
      return std::unexpected(r.error());
    }
    if (auto r = next(send_); !r) {
      return std::unexpected(r.error());
    }
    recv_max_ = recv_.epoch;
    return Opened{true, {}};
  }
  return Opened{false, std::move(body)};
}

Key Session::exporter(std::string_view label) const {
  return derive(resume_, label);
}

// ---------------------------------------------------------------------------------------------------- handshakes
struct JoinerHandshake::State {
  Bytes pw;
  Bytes id_a;
  Bytes id_b;
  std::optional<Spake> st;
  std::optional<CryptoError> start_error;
  std::optional<Agreed> ag;
  ~State() {
    sodium_memzero(pw.data(), pw.size());
    if (st) {
      sodium_memzero(st->x.data(), st->x.size());
      sodium_memzero(st->w.data(), st->w.size());
    }
    if (ag) {
      sodium_memzero(ag->k.data(), ag->k.size());
    }
  }
  State() = default;
  State(const State&) = delete;
  State& operator=(const State&) = delete;
  State(State&&) = delete;
  State& operator=(State&&) = delete;
};

JoinerHandshake::JoinerHandshake(std::span<const std::uint8_t> secret, std::string_view context,
                                 std::optional<std::array<std::uint8_t, 64>> rng)
    : st_(std::make_unique<State>()) {
  (void)crypto_ready();
  st_->pw.assign(secret.begin(), secret.end());
  auto [a, b] = identities(context);
  st_->id_a = std::move(a);
  st_->id_b = std::move(b);
  auto s = spake_start('A', st_->pw, rng);
  if (s) {
    st_->st = std::move(*s);
  } else {
    st_->start_error = s.error();
  }
}
JoinerHandshake::~JoinerHandshake() = default;
JoinerHandshake::JoinerHandshake(JoinerHandshake&&) noexcept = default;
JoinerHandshake& JoinerHandshake::operator=(JoinerHandshake&&) noexcept = default;

CryptoResult<Bytes> JoinerHandshake::message() const {
  if (st_->start_error) {
    return std::unexpected(*st_->start_error);
  }
  if (!st_->st) {
    return fail("state");
  }
  return st_->st->msg;
}

CryptoResult<Bytes> JoinerHandshake::receive(std::span<const std::uint8_t> host_msg) {
  if (!st_->st || st_->ag) {
    return fail("state");
  }
  auto k = spake_finish('A', *st_->st, st_->pw, st_->id_a, st_->id_b, host_msg);
  if (!k) {
    return std::unexpected(k.error());
  }
  st_->ag = Agreed{st_->st->msg, Bytes(host_msg.begin(), host_msg.end()), *k};
  const Key c = st_->ag->confirm(true);
  return Bytes(c.begin(), c.end());
}

CryptoResult<Session> JoinerHandshake::finish(std::span<const std::uint8_t> host_confirm) {
  if (!st_->ag) {
    return fail("state");
  }
  if (!same(host_confirm, st_->ag->confirm(false))) {
    return fail("wrong_code");
  }
  Session s = st_->ag->session(Session::Side::joiner);
  st_->st.reset();
  st_->ag.reset();
  return s;
}

struct HostHandshake::State {
  Bytes pw;
  Bytes id_a;
  Bytes id_b;
  std::optional<Agreed> ag;
  bool done = false;
  ~State() {
    sodium_memzero(pw.data(), pw.size());
    if (ag) {
      sodium_memzero(ag->k.data(), ag->k.size());
    }
  }
  State() = default;
  State(const State&) = delete;
  State& operator=(const State&) = delete;
  State(State&&) = delete;
  State& operator=(State&&) = delete;
};

HostHandshake::HostHandshake(std::span<const std::uint8_t> secret, std::string_view context)
    : st_(std::make_unique<State>()) {
  (void)crypto_ready();
  st_->pw.assign(secret.begin(), secret.end());
  auto [a, b] = identities(context);
  st_->id_a = std::move(a);
  st_->id_b = std::move(b);
}
HostHandshake::~HostHandshake() = default;
HostHandshake::HostHandshake(HostHandshake&&) noexcept = default;
HostHandshake& HostHandshake::operator=(HostHandshake&&) noexcept = default;

CryptoResult<Bytes> HostHandshake::receive(std::span<const std::uint8_t> joiner_msg,
                                           std::optional<std::array<std::uint8_t, 64>> rng) {
  if (st_->ag || st_->done) {
    return fail("state");
  }
  if (joiner_msg.size() != kPakeLen || joiner_msg[0] != 0x41) {
    return fail("bad_message");
  }
  auto st = spake_start('B', st_->pw, rng);
  if (!st) {
    return std::unexpected(st.error());
  }
  auto k = spake_finish('B', *st, st_->pw, st_->id_a, st_->id_b, joiner_msg);
  sodium_memzero(st->x.data(), st->x.size());
  sodium_memzero(st->w.data(), st->w.size());
  if (!k) {
    return std::unexpected(k.error());
  }
  st_->ag = Agreed{Bytes(joiner_msg.begin(), joiner_msg.end()), st->msg, *k};
  return st->msg;
}

CryptoResult<std::pair<Bytes, Session>> HostHandshake::finish(std::span<const std::uint8_t> joiner_confirm) {
  if (!st_->ag || st_->done) {
    return fail("state");
  }
  st_->done = true;
  if (!same(joiner_confirm, st_->ag->confirm(true))) {
    return fail("wrong_code");
  }
  const Key c = st_->ag->confirm(false);
  return std::pair<Bytes, Session>{Bytes(c.begin(), c.end()), st_->ag->session(Session::Side::host)};
}

// ---------------------------------------------------------------------------------------------------- codes, links
std::optional<Code> parse_code(std::string_view code) {
  std::string s;
  // String(code).toUpperCase(), character by character: ASCII is all that can pass the alphabet
  for (const char raw : code) {
    char ch = raw;
    if (ch >= 'a' && ch <= 'z') {
      ch = static_cast<char>(ch - 'a' + 'A');
    }
    if (ch == ' ' || ch == '-') {
      continue;
    }
    char c = ch;
    if (ch == 'O') {
      c = '0';
    } else if (ch == 'I' || ch == 'L') {
      c = '1';
    }
    if (!kCodeAlphabet.contains(c)) {
      return std::nullopt;
    }
    s += c;
  }
  if (s.size() != 7) {
    return std::nullopt;
  }
  return Code{s.substr(0, 2), s.substr(2), "lan:" + s.substr(0, 2)};
}

std::string new_code() {
  (void)crypto_ready();
  std::array<std::uint8_t, 7> r{};
  randombytes_buf(r.data(), r.size());
  std::string c;
  for (const std::uint8_t x : r) {
    c += kCodeAlphabet[x & 31U];
  }
  return c.substr(0, 2) + "-" + c.substr(2);
}

std::string room_tag(std::string_view room) {
  Bytes msg(kLabel.begin(), kLabel.end());
  append(msg, as_bytes(" room "));
  append(msg, as_bytes(room));
  const Key d = sha256(msg);
  std::uint64_t bits = 0;
  for (std::size_t i = 0; i < 5; ++i) {
    bits = (bits << 8U) | d[i];
  }
  std::string t;
  for (unsigned i = 0; i < 8; ++i) {
    t += kCodeAlphabet[(bits >> (35 - 5 * i)) & 31U];
  }
  return t;
}

namespace {
constexpr std::string_view kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::optional<Bytes> from_b64url(std::string_view s) {
  unsigned bits = 0;
  unsigned n = 0;
  Bytes out;
  for (const char ch : s) {
    const std::size_t v = kB64.find(ch);
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
  if (bits != 0 || s.size() % 4 == 1) {
    return std::nullopt;
  }
  return out;
}
}  // namespace

std::string to_b64url(std::span<const std::uint8_t> b) {
  std::string s;
  for (std::size_t i = 0; i < b.size(); i += 3) {
    const std::size_t len = std::min<std::size_t>(3, b.size() - i);
    const unsigned v = (static_cast<unsigned>(b[i]) << 16U) | (len > 1 ? static_cast<unsigned>(b[i + 1]) << 8U : 0U) |
                       (len > 2 ? static_cast<unsigned>(b[i + 2]) : 0U);
    for (std::size_t k = 0; k <= len; ++k) {
      s += kB64[(v >> (18 - 6 * k)) & 63U];
    }
  }
  return s;
}

std::optional<Bytes> parse_link_secret(std::string_view fragment) {
  if (!fragment.empty() && fragment[0] == '#') {
    fragment.remove_prefix(1);
  }
  auto out = from_b64url(fragment);
  if (!out || out->size() < 16 || out->size() > 32) {
    return std::nullopt;
  }
  return out;
}

std::string new_link_secret() {
  (void)crypto_ready();
  std::array<std::uint8_t, 16> r{};
  randombytes_buf(r.data(), r.size());
  return to_b64url(r);
}

// ---------------------------------------------------------------------------------------------------- assertions
std::span<const AssertionKey> assertion_keys() {
  static const std::array<AssertionKey, 1> kKeys = {{{"a1f5b9cc", "e2Bo1-R1Hnd9kDkp3I7LvT1kZWGMm1Ha2K_XRQVW6DY"}}};
  return kKeys;
}

Assertion verify_assertion(std::string_view token, std::string_view aud, double now, std::span<const AssertionKey> keys,
                           double skew) {
  (void)crypto_ready();
  const auto refuse = [](const char* why) { return Assertion{false, why, {}, {}, false, std::nullopt}; };
  // /^v1\.([A-Za-z0-9_-]{20,2000})\.([A-Za-z0-9_-]{86})$/
  const auto b64 = [](char c) { return kB64.contains(c); };
  if (!token.starts_with("v1.")) {
    return refuse("format");
  }
  const std::size_t dot = token.find('.', 3);
  if (dot == std::string_view::npos) {
    return refuse("format");
  }
  const std::string_view claims_part = token.substr(3, dot - 3);
  const std::string_view sig_part = token.substr(dot + 1);
  if (claims_part.size() < 20 || claims_part.size() > 2000 || sig_part.size() != 86 ||
      !std::ranges::all_of(claims_part, b64) || !std::ranges::all_of(sig_part, b64)) {
    return refuse("format");
  }
  const auto body = from_b64url(claims_part);
  const auto sig = from_b64url(sig_part);
  if (!body) {
    return refuse("format");  // JSON.parse of the TextDecoder text of nothing throws
  }
  const std::string text(body->begin(), body->end());
  auto parsed = base::parse_json(text, base::JsonLimits{std::size_t{1} << 16U, 64, std::size_t{1} << 16U});
  if (!parsed) {
    return refuse("format");
  }
  const base::Value& claims = *parsed;
  if (!sig || !claims.is_object() || !claims["v"].is_number() || claims["v"].as_number() != 1 ||
      !claims["name"].is_string()) {
    return refuse("format");
  }
  const base::Value& kid = claims["kid"];
  const AssertionKey* k = nullptr;
  for (const AssertionKey& x : keys) {
    if (kid.is_string() && x.kid == kid.as_string()) {
      k = &x;
      break;
    }
  }
  const auto pub = k != nullptr ? from_b64url(k->key) : std::nullopt;
  if (!pub || pub->size() != 32) {
    return refuse("key");
  }
  Bytes msg;
  append(msg, as_bytes("cadcraft-live-assertion/1."));
  append(msg, as_bytes(claims_part));
  if (sig->size() != crypto_sign_BYTES ||
      crypto_sign_verify_detached(sig->data(), msg.data(), msg.size(), pub->data()) != 0) {
    return refuse("signature");
  }
  const double exp = claims["exp"].num_or(std::nan(""));
  const double iat = claims["iat"].num_or(std::nan(""));
  if (!(exp + skew > now) || !(iat - skew <= now)) {
    return refuse("expired");
  }
  if (!claims["aud"].is_string() || claims["aud"].as_string() != aud) {
    return refuse("audience");
  }
  Assertion a;
  a.ok = true;
  a.name = claims["name"].as_string();
  if (claims["sub"].is_string()) {
    a.sub = claims["sub"].as_string();
  }
  a.verified = claims["verified"].truthy();
  if (claims["pic"].truthy() && claims["pic"].is_string()) {
    a.pic = claims["pic"].as_string();
  }
  return a;
}

std::string assertion_audience(const Session& s) {
  return to_b64url(s.exporter("assertion"));
}

bool picture_matches(std::span<const std::uint8_t> bytes, std::string_view pic) {
  if (bytes.size() > 16384) {
    return false;
  }
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string h;
  for (const std::uint8_t x : sha256(bytes)) {
    h += kHex[x >> 4U];
    h += kHex[x & 15U];
  }
  return h == pic;
}

}  // namespace cc::live
