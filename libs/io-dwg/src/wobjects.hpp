// SPDX-License-Identifier: Apache-2.0
// Port of cc-dwg objects-write.js — DWG object, header-variable and class encoders for R2000–R2018 (ODA spec v5.4.1
// ch. 2, 9, 10, 20, 28). Records are the JS-shaped values write.js prepares (numeric handles already resolved);
// records built by the other writer modules carry their own encoder.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wbits.hpp"
#include "wjs.hpp"

namespace cc::dwg::w {

inline constexpr double kSoftOwn = 2;
inline constexpr double kHardOwn = 3;
inline constexpr double kSoftPtr = 4;
inline constexpr double kHardPtr = 5;

/// A class the class table names (CLASSED in the JS).
struct ClassDef {
  std::string dxf, cpp, app;
  bool entity = false;
  int proxy = 0;
};

struct Ctx;
struct Rec;
using RecPtr = std::shared_ptr<Rec>;
/// A record's own encoder (nongraphic-write.js, mleader-write.js, passthrough.js …): its bytes, or nullopt.
using Encoder = std::function<std::optional<ByteBuf>(Rec& rec, Ctx& ctx)>;
/// The handles a multileader looks up when it is encoded (mleader-write.js rec.R).
struct MlRefs {
  std::function<double()> style, leader_ltype, arrowhead, style_text, style_block, text_style, content_block;
  std::function<double(const Value&)> h;
};
/// One record write.js prepares: its fields (`v`, JS-shaped), and for the other modules' records their encoder and
/// the state their closures share.
struct Rec {
  Value v;
  Encoder encode;
  std::shared_ptr<MlRefs> ml;  // a MULTILEADER's lookups (ensureMleaderStyle replaces `style`)
  std::shared_ptr<void> aux;   // a module's own state for the encoder
};

struct ClassEntry {
  ClassDef def;
  double num = 0;
  double count = 0;
};

/// One write's state the encoders share (write.js ctx + the class registry).
struct Ctx {
  std::string version;
  bool metric = false;
  double created_ms = 0;
  double handseed = 0;
  Value extents;
  std::function<std::string()> guid;
  int dwg_version = 0;
  int maint = 0;
  /// CLASSED: the known classes; the other modules add theirs (a write starts from the base set).
  std::map<std::string, ClassDef, std::less<>> classed;
  std::vector<ClassEntry> classes;
  std::map<std::string, double, std::less<>> class_nums;
  double class_num(std::string_view t);
};

/// The base CLASSED table of objects-write.js.
[[nodiscard]] std::map<std::string, ClassDef, std::less<>> base_classed();

/// objects-write.js ObjWriter: data bits here, handles in `hnd` (inline in the header before R2007), strings in the
/// string stream from R2007.
class ObjWriter : public BitWriter {
 public:
  explicit ObjWriter(std::string_view version, bool inline_handles = false);
  ObjWriter(const ObjWriter&) = delete;
  ObjWriter& operator=(const ObjWriter&) = delete;
  ObjWriter(ObjWriter&&) = delete;
  ObjWriter& operator=(ObjWriter&&) = delete;
  ~ObjWriter() = default;

  BitWriter hnd;
  BitWriter strs;
  bool inline_handles = false;
  std::uint64_t size_at = 0;

  /// A handle in the handle stream (the data stream when inline) — the JS ObjWriter overrides H the same way; nothing
  /// calls it through a BitWriter.
  void H(double code, const Value& v);  // NOLINT(bugprone-derived-method-shadowing-base-method)
  void H(double code, double v = 0);    // NOLINT(bugprone-derived-method-shadowing-base-method)
  /// A handle in the data stream.
  void Hd(double code, const Value& v) { BitWriter::H(code, v); }
  void Hd(double code, double v = 0) { BitWriter::H(code, v); }
  /// R2007+: the string stream, its size and the "present" bit (20.1). Returns endbit.
  std::uint64_t end_data();
};

void begin(ObjWriter& w, double type, const Value& rec);
[[nodiscard]] ByteBuf finish(ObjWriter& w);
void entity_common(ObjWriter& w, const Value& r);
void object_common(ObjWriter& w, const Value& r);
void table_entry(ObjWriter& w, const Value& name, int flags = 0);
[[nodiscard]] ByteBuf xrecord_bytes(const Value& items, bool r2007);
[[nodiscard]] ByteBuf eed_bytes(const Value& items, const BitWriter& w);

/// TYPE (the fixed object type numbers), -1 when not fixed.
[[nodiscard]] double fixed_type(std::string_view t);
/// Encode one record; nullopt for an unknown type.
[[nodiscard]] std::optional<ByteBuf> encode_object(Rec& rec, Ctx& ctx);
/// The class table's data (finishStreams applied).
[[nodiscard]] ByteBuf encode_classes(Ctx& ctx);
/// The AcDb:Header data (finishStreams applied). H: the fixed handles by the names write.js gives them.
[[nodiscard]] ByteBuf encode_header(const std::map<std::string, double, std::less<>>& H, const Value& hv, Ctx& ctx);

/// The ENC table: an encoder for a record type (the other modules register FIELD, TABLESTYLE, ACAD_TABLE).
using EncFn = void (*)(ObjWriter&, Value&);
void register_encoder(std::string_view t, EncFn f);

/// julian(ms since 1970) → { day, ms }.
[[nodiscard]] JulianPair julian(double t);

}  // namespace cc::dwg::w
