// SPDX-License-Identifier: Apache-2.0
// Port of cc-dwg read.js: readDwg(bytes) → Model (ODA spec v5.4.1 ch. 9 header framing, 10 classes, 23 object map,
// 20 objects, 22 template / MEASUREMENT). The model's shape, field order and JS types follow the JS reference: the
// parity gate compares them. Spec gaps the JS found by experiment are kept with their notes.
#include "cc/dwg/read.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js_math.hpp"
#include "container.hpp"
#include "messages.hpp"
#include "parts.hpp"
#include "record.hpp"

namespace cc::dwg {

using base::Array;
using base::make_object;

namespace {

bool same_bytes(Bytes b, std::size_t at, std::span<const std::uint8_t> sig) {
  if (at + sig.size() > b.size()) {
    return false;
  }
  return std::equal(sig.begin(), sig.end(), b.begin() + static_cast<std::ptrdiff_t>(at));
}

std::uint32_t le16(Bytes b, std::size_t at) {
  if (at + 2 > b.size()) {
    throw DecodeError("Offset is outside the bounds of the DataView");
  }
  return b[at] | (static_cast<std::uint32_t>(b[at + 1]) << 8U);
}
std::uint32_t le32(Bytes b, std::size_t at) {
  if (at + 4 > b.size()) {
    throw DecodeError("Offset is outside the bounds of the DataView");
  }
  return b[at] | (static_cast<std::uint32_t>(b[at + 1]) << 8U) | (static_cast<std::uint32_t>(b[at + 2]) << 16U) |
         (static_cast<std::uint32_t>(b[at + 3]) << 24U);
}

constexpr std::array<int, 24> kLineweights = {0,  5,  9,  13, 15, 18,  20,  25,  30,  35,  40,  50,
                                              53, 60, 70, 80, 90, 100, 106, 120, 140, 158, 200, 211};
int lineweight_from_index(double i) {
  if (i == 29) {
    return -1;
  }
  if (i == 30) {
    return -2;
  }
  if (i == 31) {
    return -3;
  }
  // Spec gap: indexes 24–28 are not lineweights; the ODA File Converter reports them as ByLayer.
  if (i >= 0 && i < 24 && std::floor(i) == i) {
    return kLineweights[static_cast<std::size_t>(i)];
  }
  return -1;
}

Value transparency_from_word(std::uint32_t w) {
  const std::uint32_t type = w >> 24U;
  if (type == 0) {
    return make_object({{"byLayer", true}});
  }
  if (type == 1) {
    return make_object({{"byBlock", true}});
  }
  return make_object({{"alpha", w & 0xFFU}});
}

// ---------------------------------------------------------------- ch. 9 header section
Value read_header_section(const Section* secp, Ctx& ctx) {
  if (secp == nullptr) {
    throw DecodeError(msg("ccdwg.err.no_header"));
  }
  const Bytes sec = secp->view;
  if (!same_bytes(sec, 0, kSentinelHeader)) {
    ctx.notes->push_back(msg("ccdwg.read.header_sentinel"));
  }
  const std::uint32_t size = le32(sec, 16);
  std::size_t p = 20;
  const int ver = ctx.ver;
  // Spec gap: the extra RL is there for R2010/R2013 with maintenance > 3, and R2018+.
  if (ver >= 24 && (ctx.maint > 3 || ver >= 32)) {
    p += 4;
  }
  const std::size_t data_end = std::min<std::size_t>(sec.size(), p + size);
  if (crc8(0xC0C1, sec.subspan(16, data_end - std::min<std::size_t>(16, data_end))) != le16(sec, data_end)) {
    ctx.notes->push_back(msg("ccdwg.read.header_crc"));
  }
  if (ver < 21) {
    Bits r(sec, ver, &ctx.cp, p * 8, data_end * 8);
    HeaderResult hr = decode_header_vars(r, ver, nullptr);
    if (hr.error) {
      ctx.notes->push_back(msg("ccdwg.read.header_early", {{"reason", *hr.error}}));
    }
    return std::move(hr.vars);
  }
  // R2007+: RL size in bits (counted from that RL), then data; strings in a string stream; handles after it.
  const std::uint32_t bits = le32(sec, p);
  const std::uint64_t endbit = p * 8 + static_cast<std::uint64_t>(bits);
  Bits r(sec, ver, &ctx.cp, (p + 4) * 8, endbit);
  Bits s(sec, ver, &ctx.cp);
  if (const auto ss = string_stream_start(sec, endbit, (p + 4) * 8)) {
    s.set_pos(ss->start);
    s.set_end(ss->end);
    r.set_end(ss->start);
  } else {
    s.set_pos(0);
    s.set_end(0);
  }
  r.str = &s;
  Bits hr(sec, ver, &ctx.cp, endbit, data_end * 8);
  HeaderResult res = decode_header_vars(r, ver, &hr);
  r.str = nullptr;
  if (res.error) {
    ctx.notes->push_back(msg("ccdwg.read.header_early", {{"reason", *res.error}}));
  }
  return std::move(res.vars);
}

// ---------------------------------------------------------------- ch. 10 classes
std::vector<ClassInfo> read_classes_section(const Section* secp, Ctx& ctx) {
  if (secp == nullptr) {
    throw DecodeError(msg("ccdwg.err.no_classes"));
  }
  const Bytes sec = secp->view;
  if (!same_bytes(sec, 0, kSentinelClasses)) {
    ctx.notes->push_back(msg("ccdwg.read.classes_sentinel"));
  }
  const std::uint32_t size = le32(sec, 16);
  const int ver = ctx.ver;
  std::size_t p = 20;
  if (ver >= 24 && (ctx.maint > 3 || ver >= 32)) {
    p += 4;
  }
  const std::size_t data_end = std::min<std::size_t>(sec.size(), p + size);
  if (crc8(0xC0C1, sec.subspan(16, data_end - std::min<std::size_t>(16, data_end))) != le16(sec, data_end)) {
    ctx.notes->push_back(msg("ccdwg.read.classes_crc"));
  }
  Bits r;
  Bits s;
  if (ver >= 21) {
    const std::uint32_t bits = le32(sec, p);
    const std::uint64_t endbit = p * 8 + static_cast<std::uint64_t>(bits);
    r = Bits(sec, ver, &ctx.cp, (p + 4) * 8, endbit);
    s = Bits(sec, ver, &ctx.cp);
    if (const auto ss = string_stream_start(sec, endbit, (p + 4) * 8)) {
      s.set_pos(ss->start);
      s.set_end(ss->end);
      r.set_end(ss->start);
    }
    r.str = &s;
  } else {
    r = Bits(sec, ver, &ctx.cp, p * 8, data_end * 8);
  }
  std::int64_t max_num = std::numeric_limits<std::int64_t>::max();
  if (ver >= 18) {
    max_num = r.BS();
    (void)r.RC();
    (void)r.RC();
    (void)r.B();
  }
  std::vector<ClassInfo> out;
  while (r.left() > 40) {
    ClassInfo k;
    k.num = r.BS();
    k.proxy_flags = r.BS();
    k.app_name = r.TV();
    k.cpp_name = r.TV();
    k.dxf_name = r.TV();
    k.was_zombie = r.B() != 0;
    const std::int32_t item_class = r.BS();
    k.is_entity = item_class == 0x1F2;
    if (ver >= 18) {
      k.has_r18 = true;
      k.instance_count = r.BL();
      k.dwg_version = r.BS();
      k.maint_version = r.BS();
      (void)r.BL();
      (void)r.BL();
    }
    if (k.num < 500 || k.num > 5000) {
      ctx.notes->push_back(msg("ccdwg.read.class_number", {{"num", k.num}}));
      break;
    }
    out.push_back(std::move(k));
    if (out.back().num >= max_num) {
      break;
    }
  }
  r.str = nullptr;
  return out;
}

// ---------------------------------------------------------------- ch. 23 object map
std::vector<std::pair<std::int64_t, std::int64_t>> read_object_map(const Section* secp, Ctx& ctx) {
  if (secp == nullptr) {
    throw DecodeError(msg("ccdwg.err.no_handles"));
  }
  const Bytes sec = secp->view;
  // A JS Map: insertion order, a repeated handle keeps its first place and takes the last offset.
  std::vector<std::pair<std::int64_t, std::int64_t>> map;
  std::unordered_map<std::int64_t, std::size_t> at;
  std::size_t p = 0;
  int bad = 0;
  // Spec gap: each page starts again from 0 (handles and offsets are deltas within the page).
  while (p + 2 <= sec.size()) {
    const std::size_t size = (static_cast<std::size_t>(sec[p]) << 8U) | sec[p + 1];
    if (size <= 2) {
      break;
    }
    const std::size_t end = p + size;
    if (end + 2 > sec.size()) {
      ctx.notes->push_back(msg("ccdwg.read.map_short"));
      break;
    }
    const std::uint32_t crc = (static_cast<std::uint32_t>(sec[end]) << 8U) | sec[end + 1];
    if (crc8(0xC0C1, sec.subspan(p, end - p)) != crc) {
      ++bad;
    }
    std::size_t q = p + 2;
    double hd = 0;  // JS numbers: deltas never overflow, they lose precision past 2^53 as in the reference
    double locd = 0;
    while (q < end) {
      hd += read_mcu(sec, q);
      locd += read_mc(sec, q);
      const auto h = base::clamp_cast<std::int64_t>(hd);
      const auto loc = base::clamp_cast<std::int64_t>(locd);
      if (auto it = at.find(h); it != at.end()) {
        map[it->second].second = loc;
      } else {
        at[h] = map.size();
        map.emplace_back(h, loc);
      }
    }
    p = end + 2;
  }
  if (bad != 0) {
    ctx.notes->push_back(msg("ccdwg.read.map_crc", {{"n", bad}}));
  }
  return map;
}

// ---------------------------------------------------------------- model assembly
constexpr std::array<std::string_view, 8> kSkipKeyList = {"handle",    "typeNum", "type",    "isEntity",
                                                          "className", "color",   "ltscale", "invisible"};
constexpr std::array<std::string_view, 9> kSubTypeList = {
    "VERTEX_2D", "VERTEX_3D", "VERTEX_MESH", "VERTEX_PFACE", "VERTEX_PFACE_FACE",
    "SEQEND",    "ATTRIB",    "BLOCK",       "ENDBLK"};
bool in_list(std::span<const std::string_view> list, std::string_view x) {
  return std::ranges::find(list, x) != list.end();
}
bool skip_key(std::string_view k) {
  return in_list(kSkipKeyList, k);
}
bool sub_type(std::string_view t) {
  return in_list(kSubTypeList, t);
}
int dim_base(std::string_view t) {
  if (t == "DIMENSION_LINEAR") {
    return 0;
  }
  if (t == "DIMENSION_ALIGNED") {
    return 1;
  }
  if (t == "DIMENSION_ANG2LN") {
    return 2;
  }
  if (t == "DIMENSION_DIAMETER") {
    return 3;
  }
  if (t == "DIMENSION_RADIUS") {
    return 4;
  }
  if (t == "DIMENSION_ANG3PT") {
    return 5;
  }
  if (t == "DIMENSION_ORDINATE") {
    return 6;
  }
  return -1;
}
std::string_view table_of(std::string_view t) {
  if (t == "LAYER") {
    return "layers";
  }
  if (t == "LTYPE") {
    return "linetypes";
  }
  if (t == "STYLE") {
    return "styles";
  }
  if (t == "DIMSTYLE") {
    return "dimstyles";
  }
  if (t == "VIEW") {
    return "views";
  }
  if (t == "UCS") {
    return "ucs";
  }
  if (t == "VPORT") {
    return "vports";
  }
  if (t == "APPID") {
    return "appids";
  }
  if (t == "BLOCK_HEADER") {
    return "blockRecords";
  }
  return {};
}

/// The public fields of a record (no _private keys, no skipped ones; byte arrays as base64).
Value plain(const Rec& rec) {
  Value o = Value::object();
  for (const base::Member& m : rec.f.as_object()) {
    if (m.key.empty() || m.key[0] == '_' || skip_key(m.key)) {
      continue;
    }
    if (m.value.is_bytes()) {
      o.set(m.key, base::base64(m.value.as_bytes()));
    } else {
      o.set(m.key, m.value);
    }
  }
  return o;
}

/// plain() that moves the public fields out of the record instead of copying them (the record keeps its private
/// "_" fields). Only for records nothing reads public fields of after they are converted.
Value plain_take(Rec& rec) {
  Value o = base::object_with_capacity(rec.f.as_object().size());
  for (base::Member& m : rec.f.as_object()) {
    if (m.key.empty() || m.key[0] == '_' || skip_key(m.key)) {
      continue;
    }
    if (m.value.is_bytes()) {
      o.as_object().push_back(base::Member{m.key, Value(base::base64(m.value.as_bytes()))});
    } else {
      o.as_object().push_back(base::Member{m.key, std::move(m.value)});
      m.value = Value();
    }
  }
  return o;
}

/// Types whose model builders read the record's own public fields after plain(): those are copied, not moved.
bool reads_own_fields(std::string_view t) {
  return t == "MULTILEADER" || t == "MLEADER" || t == "ACAD_TABLE" || t == "REGION" || t == "3DSOLID" || t == "BODY" ||
         t.ends_with("SURFACE");
}

bool truthy(const Value& v) {
  return v.truthy();
}
double numv(const Value& v) {
  return v.num_or(0);
}

/// fitSplineControl (read.js): the control points of a spline stored by fit points only.
std::optional<std::pair<Array, Array>> fit_spline_control(const Array& fit, double degree, const Value& t0,
                                                          const Value& t1, double knot_param) {
  const std::size_t n = fit.size();
  const double p = degree != 0 ? degree : 3;
  if (p != 3 || n < 2 || n > 2000) {
    return std::nullopt;
  }
  auto X = [&](std::size_t i) { return fit[i].get("x").num_or(0); };
  auto Y = [&](std::size_t i) { return fit[i].get("y").num_or(0); };
  auto Z = [&](std::size_t i) { return fit[i].get("z").truthy() ? fit[i].get("z").num_or(0) : 0.0; };
  std::vector<double> u{0};
  for (std::size_t i = 1; i < n; ++i) {
    double d = base::js_hypot(X(i) - X(i - 1), Y(i) - Y(i - 1), Z(i) - Z(i - 1));
    if (knot_param == 1) {
      d = std::sqrt(d);
    } else if (knot_param == 2) {
      d = 1;
    }
    u.push_back(u[i - 1] + d);
  }
  const double L = u[n - 1];
  if (!(L > 0)) {
    return std::nullopt;
  }
  std::vector<double> knots{0, 0, 0, 0};
  for (std::size_t i = 1; i + 1 < n; ++i) {
    knots.push_back(u[i]);
  }
  for (int i = 0; i < 4; ++i) {
    knots.push_back(L);
  }
  const std::size_t m = n + 2;
  auto basis = [&](double t, std::size_t& span, std::array<double, 4>& N) {
    span = 3;
    while (span < m - 1 && t >= knots[span + 1]) {
      ++span;
    }
    N = {1, 0, 0, 0};
    std::array<double, 4> left{};
    std::array<double, 4> right{};
    for (std::size_t j = 1; j <= 3; ++j) {
      left[j] = t - knots[span + 1 - j];
      right[j] = knots[span + j] - t;
      double saved = 0;
      for (std::size_t r = 0; r < j; ++r) {
        const double tmp = N[r] / (right[r + 1] + left[j - r]);
        N[r] = saved + right[r + 1] * tmp;
        saved = left[j - r] * tmp;
      }
      N[j] = saved;
    }
  };
  std::vector<std::vector<double>> A(m, std::vector<double>(m, 0));
  std::vector<std::array<double, 3>> B(m, {0, 0, 0});
  auto has = [](const Value& t) {
    return t.is_object() && (t.get("x").truthy() || t.get("y").truthy() || t.get("z").truthy());
  };
  auto tz = [](const Value& t) { return t.get("z").truthy() ? t.get("z").num_or(0) : 0.0; };
  A[0][0] = 1;
  B[0] = {X(0), Y(0), Z(0)};
  const double u4 = knots[4];
  const double u5 = knots[5];
  if (has(t0)) {
    A[1][0] = -1;
    A[1][1] = 1;
    B[1] = {t0.get("x").num_or(0) * u4 / 3, t0.get("y").num_or(0) * u4 / 3, tz(t0) * u4 / 3};
  } else {
    const double d = (u5 - knots[1]) != 0 ? (u5 - knots[1]) : u5;
    A[1][0] = 1 / u4;
    A[1][1] = -1 / u4 - 1 / d;
    A[1][2] = 1 / d;
  }
  for (std::size_t k = 1; k + 1 < n; ++k) {
    std::size_t span = 0;
    std::array<double, 4> N{};
    basis(u[k], span, N);
    for (std::size_t j = 0; j <= 3; ++j) {
      A[k + 1][span - 3 + j] = N[j];
    }
    B[k + 1] = {X(k), Y(k), Z(k)};
  }
  const double ua = knots[m - 1];
  const double ub = knots[m - 2];
  const double e1 = L - ua;
  const double e2 = L - ub;
  if (has(t1)) {
    A[m - 2][m - 2] = -1;
    A[m - 2][m - 1] = 1;
    B[m - 2] = {t1.get("x").num_or(0) * e1 / 3, t1.get("y").num_or(0) * e1 / 3, tz(t1) * e1 / 3};
  } else {
    A[m - 2][m - 1] = 1 / e1;
    A[m - 2][m - 2] = -1 / e1 - 1 / e2;
    A[m - 2][m - 3] = 1 / e2;
  }
  A[m - 1][m - 1] = 1;
  B[m - 1] = {X(n - 1), Y(n - 1), Z(n - 1)};
  for (std::size_t c = 0; c < m; ++c) {
    std::size_t piv = c;
    for (std::size_t r = c + 1; r < std::min(m, c + 5); ++r) {
      if (std::abs(A[r][c]) > std::abs(A[piv][c])) {
        piv = r;
      }
    }
    if (A[piv][c] == 0) {
      return std::nullopt;
    }
    if (piv != c) {
      std::swap(A[piv], A[c]);
      std::swap(B[piv], B[c]);
    }
    for (std::size_t r = c + 1; r < std::min(m, c + 5); ++r) {
      const double f = A[r][c] / A[c][c];
      if (f == 0) {
        continue;
      }
      for (std::size_t k = c; k < std::min(m, c + 6); ++k) {
        A[r][k] -= f * A[c][k];
      }
      for (std::size_t k = 0; k < 3; ++k) {
        B[r][k] -= f * B[c][k];
      }
    }
  }
  std::vector<std::array<double, 3>> P(m);
  for (std::size_t r = m; r-- > 0;) {
    std::array<double, 3> v = B[r];
    for (std::size_t k = r + 1; k < std::min(m, r + 6); ++k) {
      if (A[r][k] != 0) {
        for (std::size_t q = 0; q < 3; ++q) {
          v[q] -= A[r][k] * P[k][q];
        }
      }
    }
    for (std::size_t q = 0; q < 3; ++q) {
      P[r][q] = v[q] / A[r][r];
    }
  }
  Array kn;
  for (const double k : knots) {
    kn.emplace_back(k);
  }
  Array ctrl;
  for (const auto& q : P) {
    ctrl.push_back(base::pt3(q[0], q[1], q[2]));
  }
  return std::make_pair(std::move(kn), std::move(ctrl));
}

/// R13/R14 viewports keep their view in ACAD xdata "MVIEW" (read.js mviewXdata).
void mview_xdata(const Eed* x, Value& f) {
  if (x == nullptr) {
    return;
  }
  const Array& it = x->items;
  std::size_t k = it.size();
  for (std::size_t i = 0; i < it.size(); ++i) {
    const Array& g = it[i].as_array();
    if (g.at(0).num_or(-1) == 1000 && g.at(1).is_string() && g.at(1).as_string() == "MVIEW") {
      k = i;
      break;
    }
  }
  if (k == it.size()) {
    return;
  }
  std::vector<Value> v;
  for (std::size_t i = k + 3; i < it.size(); ++i) {
    v.push_back(it[i].as_array().at(1));
  }
  auto at = [&](std::size_t i) -> Value { return i < v.size() ? v[i] : Value(); };
  auto pt = [&](std::size_t i) -> Value {
    Value p = at(i);
    return p.is_object() ? p : base::pt3(0, 0, 0);
  };
  f.set("viewTarget", pt(0));
  f.set("viewDirection", pt(1));
  f.set("twist", at(2));
  f.set("viewHeight", at(3));
  f.set("viewCenter", make_object({{"x", at(4)}, {"y", at(5)}}));
  f.set("lensLength", at(6));
  f.set("frontClip", at(7));
  f.set("backClip", at(8));
  f.set("viewMode", at(9));
  f.set("circleZoom", at(10));
  f.set("snapAngle", at(17));
  f.set("snapBase", make_object({{"x", at(18)}, {"y", at(19)}}));
  f.set("snapSpacing", make_object({{"x", at(20)}, {"y", at(21)}}));
  f.set("gridSpacing", make_object({{"x", at(22)}, {"y", at(23)}}));
}

int block_flags(const Rec& r) {
  return (truthy(r.get("anonymous")) ? 1 : 0) | (truthy(r.get("hasAttribs")) ? 2 : 0) |
         (truthy(r.get("isXref")) ? 4 : 0) | (truthy(r.get("isOverlaid")) ? 8 : 0) | (truthy(r.get("_xdep")) ? 16 : 0) |
         (truthy(r.get("_flag64")) ? 64 : 0);
}

std::string count_type(const Value& e) {
  const Value& t = e["type"];
  if (t.is_string() && t.as_string() == "UNKNOWN") {
    const Value& d = e["dxfName"];
    return d.is_string() ? d.as_string() : std::string("undefined");
  }
  return t.is_string() ? t.as_string() : std::string("undefined");
}

void bump(Value& counts, const std::string& key) {
  Value* v = counts.find(key);
  if (v == nullptr) {
    counts.set(key, 1);
  } else {
    *v = v->num_or(0) + 1;
  }
}

Array hex_list(const std::vector<std::int64_t>& hs) {
  Array a;
  a.reserve(hs.size());
  for (const std::int64_t h : hs) {
    a.emplace_back(handle_hex(h));
  }
  return a;
}

/// The value of name `n` among `names` (Array of strings), or -1.
std::ptrdiff_t index_of(const Value& names, std::string_view n) {
  if (!names.is_array()) {
    return -1;
  }
  const Array& a = names.as_array();
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].is_string() && a[i].as_string() == n) {
      return static_cast<std::ptrdiff_t>(i);
    }
  }
  return -1;
}

class Builder {
 public:
  Builder(Value& model, Records& records, Ctx& ctx) : model_(&model), records_(&records), ctx_(&ctx) {
    res_.records = records_;
  }

  void run();

 private:
  Rec* get(std::int64_t h) { return records_->get(h); }
  [[nodiscard]] std::optional<Value> name_of(std::int64_t h) const { return res_.name_of(h); }
  [[nodiscard]] Value name_or_hex(std::int64_t h) const { return res_.name_or_hex(h); }
  [[nodiscard]] std::string app_name(std::int64_t h) const { return res_.app_name(h); }

  [[nodiscard]] Value xdata_of(const Rec& rec) const {
    if (!rec.has_eed) {
      return {};
    }
    Value x = Value::object();
    for (const Eed& e : rec.eed) {
      const std::string app = app_name(e.app);
      Value* list = x.find(app);
      if (list == nullptr) {
        list = &x.set(app, Array{});
      }
      for (const Value& it : e.items) {
        list->push(it);
      }
    }
    return x;
  }
  [[nodiscard]] Value common_obj(const Rec& rec) const {
    Value o = make_object({{"handle", handle_hex(rec.handle)}});
    if (const std::int64_t ow = rec.ref("owner"); ow != 0) {
      o.set("owner", handle_hex(ow));
    }
    Value xd = xdata_of(rec);
    if (!xd.is_undefined()) {
      o.set("xdata", std::move(xd));
    }
    if (const std::int64_t xd2 = rec.ref("xdic"); xd2 != 0) {
      o.set("extDict", handle_hex(xd2));
    }
    if (!rec.refs("reactors").empty()) {
      o.set("reactors", hex_list(rec.refs("reactors")));
    }
    return o;
  }
  [[nodiscard]] Value layer_name(std::int64_t h) const {
    const Rec* r = records_->get(h);
    return r != nullptr ? r->get("name") : Value("0");
  }
  [[nodiscard]] std::int64_t owner_of(const Rec& rec) const {
    if (const std::int64_t o = rec.ref("owner"); o != 0) {
      return o;
    }
    if (rec.entmode == 2) {
      return ms_rec_;
    }
    if (rec.entmode == 1) {
      return ps_rec_;
    }
    return 0;
  }

  std::vector<std::int64_t> chain(std::int64_t first, std::int64_t last) {
    std::vector<std::int64_t> out;
    std::unordered_set<std::int64_t> seen;
    std::int64_t h = first;
    while (h != 0 && !seen.contains(h) && out.size() <= records_->size()) {
      seen.insert(h);
      const Rec* r = get(h);
      out.push_back(h);
      if (h == last || r == nullptr) {
        break;
      }
      const std::int64_t next = r->nolinks != 0 ? h + 1 : r->ref("next");
      if (next == 0 || next == h) {
        break;
      }
      h = next;
    }
    return out;
  }

  [[nodiscard]] const Array* mlatt_of(const Rec& rec) const {
    const Rec* d = records_->get(rec.ref("xdic"));
    if (d == nullptr || d->type != "DICTIONARY" || !d->get("names").is_array()) {
      return nullptr;
    }
    const std::ptrdiff_t k = index_of(d->get("names"), "ACAD_MLATT");
    const auto& items = d->refs("items");
    const Rec* x = k >= 0 && static_cast<std::size_t>(k) < items.size()
                       ? records_->get(items[static_cast<std::size_t>(k)])
                       : nullptr;
    return x != nullptr && x->type == "XRECORD" && x->get("data").is_array() ? &x->get("data").as_array() : nullptr;
  }
  void apply_mlatt(const Rec& rec, Value& f) const;
  Value convert(Rec& rec, std::int64_t owner_handle);
  Value type_fields(Rec& rec);
  Value object_fields(const Rec& r);

  Value* model_;      // non-owning: the model being built
  Records* records_;  // non-owning
  Ctx* ctx_;          // non-owning
  Resolver res_;
  std::int64_t ms_rec_ = 0;
  std::int64_t ps_rec_ = 0;
  std::unordered_set<std::int64_t> used_;
  std::unordered_set<std::int64_t> mlatt_fold_;
  int build_errors_ = 0;
};

void Builder::apply_mlatt(const Rec& rec, Value& f) const {
  const Array* data = mlatt_of(rec);
  if (data == nullptr) {
    return;
  }
  auto code_of = [](const Value& x) { return x.as_array().at(0).num_or(-1); };
  std::size_t ti = data->size();
  for (std::size_t i = 0; i < data->size(); ++i) {
    if (code_of((*data)[i]) == 2) {
      ti = i;
      break;
    }
  }
  if (ti == data->size()) {
    return;
  }
  auto g = [&](double code, int nth) -> Value {
    for (const Value& x : *data) {
      if (code_of(x) == code && nth-- == 0) {
        return x.as_array().at(1);
      }
    }
    return {};
  };
  auto a = [&](double code, int nth) -> Value {
    for (std::size_t i = ti + 1; i < data->size(); ++i) {
      if (code_of((*data)[i]) == code && nth-- == 0) {
        return (*data)[i].as_array().at(1);
      }
    }
    return {};
  };
  f.set("attType", g(70, 0));
  f.set("storedTag", f["tag"]);
  f.set("tag", (*data)[ti].as_array().at(1));
  Value mt = Value::object();
  mt.set("insert", a(10, 0));
  mt.set("height", a(40, 0));
  mt.set("width", a(41, 0));
  mt.set("rectHeight", a(46, 0));
  mt.set("attach", a(71, 0));
  mt.set("direction", a(72, 0));
  mt.set("value", a(1, 1));
  mt.set("xAxis", a(11, 0));
  mt.set("lineSpacingStyle", a(73, 0));
  mt.set("lineSpacing", a(44, 0));
  const Value value = mt["value"];
  f.set("mtext", std::move(mt));
  if (!value.is_undefined()) {
    f.set("storedValue", f["value"]);
    f.set("value", value);
  }
}

// Recursion: an INSERT converts its ATTRIBs (convert → type_fields → convert), one level, never deeper (an ATTRIB
// has no attributes of its own).
// NOLINTNEXTLINE(misc-no-recursion)
Value Builder::convert(Rec& rec, std::int64_t owner_handle) {
  const int ver = ctx_->ver;
  used_.insert(rec.handle);
  Value e = base::object_with_capacity(rec.f.as_object().size() + 18);
  e.append("handle", handle_hex(rec.handle));
  e.append("type", rec.type);
  e.append("owner", handle_hex(owner_handle != 0 ? owner_handle : owner_of(rec)));
  // up to "colorBook" every key is new to `e`: appended, not searched
  e.append("layer", layer_name(rec.ref("layer")));
  const auto lt_name = [&]() -> Value { return name_or_hex(rec.ref("ltype")); };
  if (ver < 15) {
    e.append("linetype", rec.bylayerlt != 0 ? Value("ByLayer") : lt_name());
  } else {
    switch (rec.ltflags.value_or(0xFFFFFFFFU)) {
      case 0: e.append("linetype", "ByLayer"); break;
      case 1: e.append("linetype", "ByBlock"); break;
      case 2: e.append("linetype", "Continuous"); break;
      default: e.append("linetype", lt_name()); break;
    }
  }
  e.append("color", rec.color && rec.color->truthy() ? *rec.color : make_object({{"index", 256}}));
  e.append("lineweight", rec.lw ? lineweight_from_index(*rec.lw) : -1);
  e.append("ltscale", rec.ltscale ? Value(*rec.ltscale) : Value());
  e.append("invisible", rec.invisible ? Value(*rec.invisible) : Value());
  if (rec.transparency) {
    e.append("transparency", transparency_from_word(*rec.transparency));
  }
  if (const std::int64_t m = rec.ref("material"); m != 0) {
    e.append("material", handle_hex(m));
  }
  if (const std::int64_t p = rec.ref("plotstyle"); p != 0) {
    e.append("plotStyle", handle_hex(p));
  }
  if (const std::int64_t b = rec.ref("colorBook"); b != 0) {
    e.append("colorBook", handle_hex(b));
  }
  Value xd = xdata_of(rec);
  if (!xd.is_undefined()) {
    // Spec gap: Revit's DWG export puts the real lineweight in ACAD xdata [1000 "RTLwPs"] [1070 128 + index].
    if (const Value* acad = xd.find("ACAD")) {
      const Array& a = acad->as_array();
      for (std::size_t k = 0; k < a.size(); ++k) {
        const Array& it = a[k].as_array();
        if (it.at(0).num_or(-1) == 1000 && it.at(1).is_string() && it.at(1).as_string() == "RTLwPs") {
          if (k + 1 < a.size()) {
            const Array& nx = a[k + 1].as_array();
            const auto v = base::clamp_cast<std::int64_t>(nx.at(1).num_or(0));
            if (nx.at(0).num_or(-1) == 1070 && (v & 128) != 0) {
              e.set("lineweight", lineweight_from_index(static_cast<double>(v & 0x7F)));
            }
          }
          break;
        }
      }
    }
    e.set("xdata", std::move(xd));
  }
  if (const std::int64_t x = rec.ref("xdic"); x != 0) {
    e.set("extDict", handle_hex(x));
  }
  if (!rec.refs("reactors").empty()) {
    e.set("reactors", hex_list(rec.refs("reactors")));
  }
  if (rec.unknown) {
    e.set("type", "UNKNOWN");
    e.set("typeNum", rec.type_num);
    e.set("dxfName", rec.type);
    e.set("className", rec.class_name.value_or(""));
    if (auto raw = capture_raw(rec, ver)) {
      e.set("raw", std::move(*raw));
    }
    e.set("bits",
          static_cast<double>(static_cast<std::int64_t>(rec.data_end) - static_cast<std::int64_t>(rec.data_start)));
    bump(*model_->find("report")->find("unknown"), rec.type);
    return e;
  }
  Value f = type_fields(rec);
  // Object.assign(e, f): f (the record's fields) can repeat only the common keys convert() set above.
  static constexpr std::array<std::string_view, 17> kCommon = {
      "handle",       "type",     "owner",     "layer",     "linetype", "color",   "lineweight", "ltscale", "invisible",
      "transparency", "material", "plotStyle", "colorBook", "xdata",    "extDict", "reactors",   "space"};
  e.assign_from(std::move(f), kCommon);
  if (rec.error) {
    e.set("damaged", *rec.error);
  }
  return e;
}

// NOLINTNEXTLINE(misc-no-recursion) — see convert()
Value Builder::type_fields(Rec& rec) {
  const int ver = ctx_->ver;
  // Entities are converted once and nothing reads their public fields afterwards: move them (below, reads of the
  // record's own public fields go through `f`).
  Value f = reads_own_fields(rec.type) ? plain(rec) : plain_take(rec);
  const std::string& t = rec.type;
  if (t == "SHAPE" || t == "MLINE") {
    f.set("style", name_or_hex(rec.ref("style")));
  } else if (t == "TEXT" || t == "ATTRIB" || t == "ATTDEF") {
    f.set("style", name_or_hex(rec.ref("style")));
    if (t != "TEXT" && rec.ref("xdic") != 0) {
      apply_mlatt(rec, f);
    }
  } else if (t == "MTEXT") {
    f.set("style", name_or_hex(rec.ref("style")));
    // R13/R14: line spacing kept in ACAD xdata between "MTEXTBEGIN" and "MTEXTEND". Spec gap: found by experiment.
    if (ver < 15) {
      for (const Eed& x : rec.eed) {
        if (app_name(x.app) != "ACAD") {
          continue;
        }
        const Array& it = x.items;
        std::size_t b0 = it.size();
        for (std::size_t i = 0; i < it.size(); ++i) {
          const Value& v = it[i].as_array().at(1);
          if (v.is_string() && v.as_string() == "MTEXTBEGIN") {
            b0 = i;
            break;
          }
        }
        if (b0 < it.size()) {
          for (std::size_t k = b0 + 1; k + 1 < it.size(); k += 2) {
            const Value& kv = it[k].as_array().at(1);
            if (kv.is_string() && kv.as_string() == "MTEXTEND") {
              break;
            }
            if (it[k].as_array().at(0).num_or(-1) != 1070) {
              break;
            }
            const double code = kv.num_or(0);
            const Value& v = it[k + 1].as_array().at(1);
            if (code == 73) {
              f.set("lineSpacingStyle", v);
            } else if (code == 44) {
              f.set("lineSpacing", v);
            } else if (code == 46) {
              f.set("rectHeight", v);
            }
          }
        }
        break;
      }
    }
    // Saved-down files keep the newer MTEXT in XRECORD "ACAD_MTEXT_RT" (group-1 chunks). Spec gap.
    const Rec* dict = records_->get(rec.ref("xdic"));
    if (dict != nullptr && dict->type == "DICTIONARY" && dict->get("names").is_array()) {
      const std::ptrdiff_t k = index_of(dict->get("names"), "ACAD_MTEXT_RT");
      const auto& items = dict->refs("items");
      const Rec* xr = k >= 0 && static_cast<std::size_t>(k) < items.size()
                          ? records_->get(items[static_cast<std::size_t>(k)])
                          : nullptr;
      if (xr != nullptr && xr->type == "XRECORD" && xr->get("data").is_array()) {
        std::string text;
        for (const Value& d : xr->get("data").as_array()) {
          if (d.as_array().at(0).num_or(-1) == 1) {
            const Value& v = d.as_array().at(1);
            if (v.is_string()) {
              text += v.as_string();
            }
          }
        }
        if (!text.empty()) {
          f.set("storedValue", f["value"]);
          f.set("value", text);
        }
      }
    }
  } else if (t == "INSERT" || t == "MINSERT") {
    f.set("block", name_or_hex(rec.ref("block")));
    f.set("blockRecord", handle_hex(rec.ref("block")));
    Array atts;
    std::vector<std::int64_t> ah;
    if (rec.has_h("attribs")) {
      ah = rec.refs("attribs");
    } else if (rec.ref("firstAttrib") != 0) {
      ah = chain(rec.ref("firstAttrib"), rec.ref("lastAttrib"));
    }
    for (const std::int64_t h : ah) {
      Rec* a = get(h);
      if (a != nullptr && a->type == "ATTRIB") {
        if (mlatt_fold_.contains(h)) {
          used_.insert(h);
        } else {
          atts.push_back(convert(*a, rec.handle));
        }
      }
    }
    f.set("attribs", std::move(atts));
    if (const std::int64_t s = rec.ref("seqend"); s != 0) {
      used_.insert(s);
    }
  } else if (t == "POLYLINE_2D" || t == "POLYLINE_3D" || t == "POLYLINE_PFACE" || t == "POLYLINE_MESH") {
    std::vector<std::int64_t> vh;
    if (rec.has_h("vertices")) {
      vh = rec.refs("vertices");
    } else if (rec.ref("firstVertex") != 0) {
      vh = chain(rec.ref("firstVertex"), rec.ref("lastVertex"));
    }
    Array verts;
    Array faces;
    for (const std::int64_t h : vh) {
      const Rec* v = get(h);
      if (v == nullptr || !v->type.starts_with("VERTEX")) {
        continue;
      }
      used_.insert(h);
      if (v->type == "VERTEX_PFACE_FACE") {
        if (truthy(v->get("indices"))) {
          faces.push_back(v->get("indices"));
        }
        continue;
      }
      const Value& pt = v->get("point");
      if (!truthy(pt)) {
        continue;
      }
      Value o = make_object({{"x", pt.get("x")}, {"y", pt.get("y")}, {"z", pt.get("z")}, {"flags", v->get("flags")}});
      if (v->type == "VERTEX_2D") {
        o.set("z", f["elevation"]);
        o.set("startWidth", v->get("startWidth"));
        o.set("endWidth", v->get("endWidth"));
        o.set("bulge", v->get("bulge"));
        o.set("tangent", v->get("tangent"));
        if (!v->get("id").is_undefined()) {
          o.set("id", v->get("id"));
        }
      }
      verts.push_back(std::move(o));
    }
    if (const std::int64_t s = rec.ref("seqend"); s != 0) {
      used_.insert(s);
    }
    f.set("vertices", std::move(verts));
    if (t == "POLYLINE_PFACE") {
      f.set("faces", std::move(faces));
    }
    if (t == "POLYLINE_3D") {
      const auto cl = static_cast<std::int64_t>(numv(rec.get("_closedFlags")));
      const auto sp = static_cast<std::int64_t>(numv(rec.get("_splineFlags")));
      f.set("flags", 8 | (cl & 1) | ((sp & 3) != 0 ? 4 : 0));
      int curve = 0;
      if ((sp & 1) != 0) {
        curve = 5;
      } else if ((sp & 2) != 0) {
        curve = 6;
      }
      f.set("curveType", curve);
    }
    if (t == "POLYLINE_PFACE") {
      f.set("flags", 64);
    }
    if (const Value* flags_v = f.find("flags"); flags_v != nullptr && !flags_v->is_undefined()) {
      f.set("closed", (static_cast<std::int64_t>(flags_v->num_or(0)) & 1) != 0);
    }
  } else if (t == "LEADER") {
    f.set("annotation", handle_hex(rec.ref("annotation")));
    f.set("style", name_or_hex(rec.ref("dimstyle")));
  } else if (t == "TOLERANCE") {
    f.set("style", name_or_hex(rec.ref("dimstyle")));
  } else if (t == "IMAGE" || t == "WIPEOUT") {
    f.set("def", handle_hex(rec.ref("def")));
    f.set("defReactor", handle_hex(rec.ref("defReactor")));
  } else if (t == "VIEWPORT") {
    if (ver < 15) {
      const Eed* acad = nullptr;
      for (const Eed& x : rec.eed) {
        if (app_name(x.app) == "ACAD") {
          acad = &x;
          break;
        }
      }
      mview_xdata(acad, f);
    }
    f.set("frozenLayers", hex_list(rec.refs("frozenLayers")));
    if (const std::int64_t c = rec.ref("clipBoundary"); c != 0) {
      f.set("clipBoundary", handle_hex(c));
    }
  } else if (t == "HATCH") {
    if (Value* loops = f.find("loops"); loops != nullptr && loops->is_array()) {
      for (Value& l : loops->as_array()) {
        Array b;
        if (const Value* lb = l.find("_boundary"); lb != nullptr && lb->is_array()) {
          for (const Value& x : lb->as_array()) {
            b.emplace_back(handle_hex(base::clamp_cast<std::int64_t>(x.num_or(0))));
          }
        }
        l.set("boundary", std::move(b));
        l.erase("_boundary");
        l.erase("_nbound");
      }
    }
  } else if (t == "SPLINE") {
    const Value& fit = f["fit"];
    const Value& knots = f["knots"];
    const bool no_knots = !knots.is_array() || knots.as_array().empty();
    if (f["scenario"].num_or(-1) == 2 && fit.is_array() && fit.as_array().size() >= 2 && no_knots &&
        !(f["fitTolerance"].num_or(0) > 1e-9)) {
      const double kp = f["knotParam"].is_undefined() || f["knotParam"].is_null() ? 0 : f["knotParam"].num_or(0);
      if (auto c = fit_spline_control(fit.as_array(), f["degree"].num_or(0), f["startTangent"], f["endTangent"], kp)) {
        f.set("knots", std::move(c->first));
        f.set("control", std::move(c->second));
        f.set("controlComputed", true);
      }
    }
  } else if (t == "MULTILEADER" || t == "MLEADER") {
    f.set("style", name_or_hex(rec.ref("style")));
    const Value& ctxv = rec.get("context");
    if (ctxv.is_object() && truthy(ctxv["block"])) {
      f.set("contentBlock", name_or_hex(base::clamp_cast<std::int64_t>(ctxv["block"]["_block"].num_or(0))));
    }
    if (ctxv.is_object() && truthy(ctxv["text"])) {
      auto n = name_of(base::clamp_cast<std::int64_t>(ctxv["text"]["_style"].num_or(0)));
      f.set("textStyle", n ? *n : Value());
    }
    std::optional<Value> m;
    try {
      m = read_mleader(rec, *ctx_, res_);
    } catch (const std::exception&) {
      m.reset();
    }
    if (m) {
      f.erase("context");
      f.erase("arrowheads");
      f.erase("blockLabels");
      f.erase("mlVersion");
      for (base::Member& x : m->as_object()) {
        f.set(x.key, std::move(x.value));
      }
    }
  } else if (t == "ACAD_PROXY_ENTITY") {
    if (auto raw = capture_raw(rec, ver)) {
      f.set("raw", std::move(*raw));
    }
    f.set("typeNum", rec.type_num);
  } else if (t == "ACAD_TABLE") {
    table_entity_fields(rec, f, res_);
  } else if (t == "REGION" || t == "3DSOLID" || t == "BODY") {
    solid_fields(rec, f, res_);
  } else if (t == "SURFACE" || t == "PLANESURFACE" || t == "EXTRUDEDSURFACE" || t == "LOFTEDSURFACE" ||
             t == "REVOLVEDSURFACE" || t == "SWEPTSURFACE" || t == "NURBSURFACE") {
    surface_fields(rec, f, res_);
  } else if (dim_base(t) >= 0 || t == "ARC_DIMENSION" || t == "LARGE_RADIAL_DIMENSION") {
    f.set("block", name_or_hex(rec.ref("block")));
    f.set("style", name_or_hex(rec.ref("dimstyle")));
    const int db = dim_base(t);
    const double base = db >= 0 ? db : f["dimType"].num_or(0);
    const auto f1 = static_cast<std::int64_t>(numv(rec.get("_flags1")));
    const auto f2 = static_cast<std::int64_t>(numv(rec.get("_flags2")));
    f.set("flags",
          static_cast<double>(static_cast<std::int64_t>(base) | ((f1 & 1) != 0 ? 0 : 128) | ((f1 & 2) != 0 ? 32 : 0) |
                              (t == "DIMENSION_ORDINATE" && (f2 & 1) != 0 ? 64 : 0)));
  }
  return f;
}

Value Builder::object_fields(const Rec& r) {
  Value f = plain(r);
  const std::string& t = r.type;
  if (t == "DICTIONARY" || t == "ACDBDICTIONARYWDFLT" || t == "DICTIONARYWDFLT") {
    Value entries = Value::object();
    const Value& names = r.get("names");
    const auto& items = r.refs("items");
    if (names.is_array()) {
      const Array& a = names.as_array();
      for (std::size_t i = 0; i < a.size(); ++i) {
        std::string key;
        if (a[i].is_string()) {
          key = a[i].as_string();
        } else {
          key = "undefined";
        }
        entries.set(key, handle_hex(i < items.size() ? items[i] : 0));
      }
    }
    f.set("entries", std::move(entries));
    f.erase("names");
    if (const std::int64_t d = r.ref("defaultEntry"); d != 0) {
      f.set("defaultEntry", handle_hex(d));
    }
  } else if (t == "XRECORD") {
    f.set("objids", hex_list(r.refs("objids")));
  } else if (t == "GROUP") {
    f.set("entities", hex_list(r.refs("entities")));
  } else if (t == "LAYOUT") {
    f.set("block", handle_hex(r.ref("block")));
    f.set("viewports", hex_list(r.refs("viewports")));
  } else if (t == "SORTENTSTABLE") {
    f.set("ownerBlock", handle_hex(r.ref("ownerBlock")));
    f.set("entities", hex_list(r.refs("ents")));
    Array sh;
    if (r.get("_sort").is_array()) {
      for (const Value& x : r.get("_sort").as_array()) {
        sh.emplace_back(handle_hex(base::clamp_cast<std::int64_t>(x.num_or(0))));
      }
    }
    f.set("sortHandles", std::move(sh));
  } else if (t == "IDBUFFER") {
    f.set("ids", hex_list(r.refs("ids")));
  } else if (t == "FIELDLIST") {
    f.set("fields", hex_list(r.refs("fields")));
  } else if (t == "TABLESTYLE" || t == "CELLSTYLEMAP" || t == "TABLECONTENT" || t == "TABLEGEOMETRY" || t == "FIELD") {
    table_object_fields(r, f, res_);
  } else if (t == "LAYER_INDEX") {
    Array out;
    const auto& hs = r.refs("entries");
    if (r.get("entries").is_array()) {
      const Array& es = r.get("entries").as_array();
      for (std::size_t i = 0; i < es.size(); ++i) {
        Value e = es[i];
        e.set("handle", handle_hex(i < hs.size() ? hs[i] : 0));
        out.push_back(std::move(e));
      }
    }
    f.set("entries", std::move(out));
  } else if (t == "MLINESTYLE") {
    if (Value* els = f.find("elements"); els != nullptr && els->is_array()) {
      for (Value& e : els->as_array()) {
        if (const Value* lt = e.find("_ltype"); lt != nullptr && !lt->is_undefined()) {
          const std::string hex = handle_hex(static_cast<std::int64_t>(lt->num_or(0)));
          e.set("linetype", hex);
          e.erase("_ltype");
        }
      }
    }
  } else if (t == "MLEADERSTYLE") {
    for (const std::string_view k : {"leaderLtype", "arrowhead", "textStyle", "block"}) {
      if (r.has_h(k)) {
        f.set(k, handle_hex(r.ref(k)));
      }
    }
    std::optional<Value> m;
    try {
      m = read_mleader_style(r, *ctx_, res_);
    } catch (const std::exception&) {
      m.reset();
    }
    if (m) {
      f.erase("leaderLtype");
      for (base::Member& x : m->as_object()) {
        f.set(x.key, std::move(x.value));
      }
    }
  }
  return f;
}

void Builder::run() {
  const int ver = ctx_->ver;
  Value& H = *model_->find("header");
  Value& rep = *model_->find("report");
  auto hnum = [&](std::string_view k) -> std::int64_t {
    const Value& v = H[k];
    if (!v.is_string() || v.as_string().empty()) {
      return 0;
    }
    return static_cast<std::int64_t>(std::stoull(v.as_string(), nullptr, 16));
  };

  // ---- tables (control objects → entries)
  const std::array<std::pair<std::string_view, std::string_view>, 9> controls = {{
      {"blockRecords", "BLOCK_CONTROL_OBJECT"},
      {"layers", "LAYER_CONTROL_OBJECT"},
      {"styles", "STYLE_CONTROL_OBJECT"},
      {"linetypes", "LINETYPE_CONTROL_OBJECT"},
      {"views", "VIEW_CONTROL_OBJECT"},
      {"ucs", "UCS_CONTROL_OBJECT"},
      {"vports", "VPORT_CONTROL_OBJECT"},
      {"appids", "APPID_CONTROL_OBJECT"},
      {"dimstyles", "DIMSTYLE_CONTROL_OBJECT"},
  }};
  std::unordered_map<std::string_view, std::vector<std::int64_t>> table_recs;
  for (const auto& [tname, ch] : controls) {
    const Rec* c = get(hnum(ch));
    std::unordered_set<std::int64_t> seen;
    std::vector<std::int64_t> list;
    if (c != nullptr && c->has_h("entries")) {
      for (const std::int64_t h : c->refs("entries")) {
        const Rec* r = get(h);
        if (r != nullptr && !seen.contains(h) && table_of(r->type) == tname) {
          seen.insert(h);
          list.push_back(h);
        }
      }
    }
    // entries the control does not list (damaged control objects): add them in record order
    for (const Rec& r : records_->all()) {
      if (table_of(r.type) == tname && !seen.contains(r.handle)) {
        seen.insert(r.handle);
        list.push_back(r.handle);
      }
    }
    table_recs[tname] = std::move(list);
  }

  // ACAD_XREC_ROUNDTRIP: the long table record names of files saved down to R13/R14. Spec gap.
  for (Rec& r : records_->all()) {
    if (table_of(r.type).empty() || r.ref("xdic") == 0) {
      continue;
    }
    const Rec* d = get(r.ref("xdic"));
    if (d == nullptr || d->type != "DICTIONARY" || !d->get("names").is_array()) {
      continue;
    }
    const std::ptrdiff_t k = index_of(d->get("names"), "ACAD_XREC_ROUNDTRIP");
    const auto& items = d->refs("items");
    const Rec* x =
        k >= 0 && static_cast<std::size_t>(k) < items.size() ? get(items[static_cast<std::size_t>(k)]) : nullptr;
    if (x == nullptr || x->type != "XRECORD" || !x->get("data").is_array()) {
      continue;
    }
    const Array& data = x->get("data").as_array();
    auto g = [&](std::size_t i) -> const Array* { return i < data.size() ? &data[i].as_array() : nullptr; };
    for (std::size_t i = 0; i < data.size(); ++i) {
      const Array& gi = data[i].as_array();
      if (gi.at(0).num_or(-1) == 102 && gi.at(1).is_string() && gi.at(1).as_string() == "EXTNAMES") {
        const Array* g2 = g(i + 2);
        const Array* g1 = g(i + 1);
        if (g2 != nullptr && g2->at(0).num_or(-1) == 2 && g1 != nullptr && g1->at(1) == r.get("name")) {
          r.set("_shortName", r.get("name"));
          r.set("name", g2->at(1));
        }
        break;
      }
    }
    if (r.type == "LAYER" && ver < 15) {
      for (std::size_t i = 0; i < data.size(); ++i) {
        const Array& gi = data[i].as_array();
        if (gi.at(0).num_or(-1) == 102 && gi.at(1).is_string() && gi.at(1).as_string() == "LINEWEIGHT") {
          if (const Array* g1 = g(i + 1)) {
            r.set("_lwIndex", static_cast<double>(base::clamp_cast<std::int64_t>(g1->at(1).num_or(0)) & 0x1F));
          }
          break;
        }
      }
      for (std::size_t i = 0; i < data.size(); ++i) {
        const Array& gi = data[i].as_array();
        if (gi.at(0).num_or(-1) == 102 && gi.at(1).is_string() && gi.at(1).as_string() == "PLOTBIT") {
          if (const Array* g1 = g(i + 1)) {
            r.set("plot", g1->at(1).truthy());
          }
          break;
        }
      }
    }
  }
  // R13/R14: the R2000+ header variables kept in ACDBHEADERROUNDTRIPXREC. Spec gap.
  if (ver < 15) {
    const Rec* nod = get(hnum("DICTIONARY_NAMED_OBJECTS"));
    const std::ptrdiff_t k = nod != nullptr ? index_of(nod->get("names"), "ACDBHEADERROUNDTRIPXREC") : -1;
    const Rec* d = nullptr;
    if (k >= 0 && static_cast<std::size_t>(k) < nod->refs("items").size()) {
      d = get(nod->refs("items")[static_cast<std::size_t>(k)]);
    }
    if (d != nullptr && d->get("names").is_array()) {
      const Array& names = d->get("names").as_array();
      for (std::size_t j = 0; j < names.size(); ++j) {
        const Rec* x = j < d->refs("items").size() ? get(d->refs("items")[j]) : nullptr;
        if (x == nullptr || x->type != "XRECORD" || !x->get("data").is_array() ||
            x->get("data").as_array().size() != 1 || !names[j].is_string() ||
            (H.has(names[j].as_string()) && !H[names[j].as_string()].is_undefined())) {
          continue;
        }
        H.set(names[j].as_string(), x->get("data").as_array()[0].as_array().at(1));
      }
    }
  }
  // Dynamic block true names (AcDbDynamicBlockTrueName[2] xdata). Spec gap.
  auto owns_graph = [&](const Rec& r) {
    const Rec* d = r.ref("xdic") != 0 ? get(r.ref("xdic")) : nullptr;
    return d != nullptr && index_of(d->get("names"), "ACAD_ENHANCEDBLOCK") >= 0;
  };
  std::unordered_set<std::string> used_names;
  for (const Rec& r : records_->all()) {
    if (r.type == "BLOCK_HEADER" && r.get("name").truthy() && r.get("name").is_string() &&
        !r.get("name").as_string().starts_with("*")) {
      used_names.insert(base::to_upper(r.get("name").as_string()));
    }
  }
  for (Rec& r : records_->all()) {
    if (r.type != "BLOCK_HEADER" || !r.has_eed || !r.get("name").is_string()) {
      continue;
    }
    const std::string& nm = r.get("name").as_string();
    if (nm.size() < 2 || nm[0] != '*' || (nm[1] != 'U' && nm[1] != 'u')) {
      continue;
    }
    for (const Eed& x : r.eed) {
      const std::string app = app_name(x.app);
      if ((app == "AcDbDynamicBlockTrueName2" || app == "AcDbDynamicBlockTrueName") && !x.items.empty() &&
          x.items[0].as_array().at(0).num_or(-1) == 1000) {
        const Value tn = x.items[0].as_array().at(1);
        r.set("_trueName", tn);
        const bool rename =
            app == "AcDbDynamicBlockTrueName2" ||
            (owns_graph(r) && tn.truthy() && tn.is_string() && !used_names.contains(base::to_upper(tn.as_string())));
        if (rename) {
          r.set("_storedName", r.get("name"));
          r.set("name", tn);
          if (tn.is_string()) {
            used_names.insert(base::to_upper(tn.as_string()));
          }
        }
      }
    }
  }
  // Anonymous block names numbered by their position in the BLOCK_CONTROL list (null entries skipped). Spec gap.
  {
    const Rec* c = get(hnum("BLOCK_CONTROL_OBJECT"));
    const std::int64_t ps = hnum("BLOCK_RECORD_PAPER_SPACE");
    const std::int64_t ms = hnum("BLOCK_RECORD_MODEL_SPACE");
    const std::vector<std::int64_t> list = c != nullptr ? c->refs("entries") : std::vector<std::int64_t>{};
    int i = -1;
    for (const std::int64_t h : list) {
      if (h == 0) {
        continue;
      }
      ++i;
      Rec* r = get(h);
      if (r == nullptr || r->type != "BLOCK_HEADER" || h == ps || h == ms || !r->get("name").is_string()) {
        continue;
      }
      const std::string nm = r->get("name").as_string();
      const bool one_letter = nm.size() == 2 && nm[0] == '*' && std::isalpha(static_cast<unsigned char>(nm[1])) != 0;
      if (one_letter || base::to_upper(nm) == "*PAPER_SPACE") {
        r->set("_storedName", nm);
        r->set("name", nm + std::to_string(i));
      }
    }
  }

  Value& T = *model_->find("tables");
  auto tlist = [&](std::string_view n) -> Array& { return T.find(n)->as_array(); };
  for (const std::int64_t h : table_recs["layers"]) {
    const Rec& r = *get(h);
    const Rec* lt = get(r.ref("ltype"));
    Value o = common_obj(r);
    o.set("name", r.get("name"));
    o.set("flags", (truthy(r.get("frozen")) ? 1 : 0) | (truthy(r.get("frozenNew")) ? 2 : 0) |
                       (truthy(r.get("locked")) ? 4 : 0) | (truthy(r.get("_xdep")) ? 16 : 0) |
                       (truthy(r.get("_flag64")) ? 64 : 0));
    o.set("color", r.get("color"));
    o.set("linetype", handle_hex(r.ref("ltype")));
    o.set("linetypeName", lt != nullptr ? lt->get("name") : Value());
    o.set("lineweight", !r.get("_lwIndex").is_undefined() ? lineweight_from_index(r.num("_lwIndex")) : -3);
    o.set("plot", r.get("plot"));
    o.set("off", r.get("on").is_undefined() ? Value(true) : Value(!truthy(r.get("on"))));
    o.set("frozen", r.get("frozen"));
    o.set("locked", r.get("locked"));
    o.set("frozenInNewViewports", r.get("frozenNew"));
    o.set("plotStyle", r.ref("plotstyle") != 0 ? Value(handle_hex(r.ref("plotstyle"))) : Value());
    o.set("material", r.ref("material") != 0 ? Value(handle_hex(r.ref("material"))) : Value());
    tlist("layers").push_back(std::move(o));
  }
  for (const std::int64_t h : table_recs["linetypes"]) {
    const Rec& r = *get(h);
    Value o = common_obj(r);
    o.set("name", r.get("name"));
    o.set("flags", (truthy(r.get("_xdep")) ? 16 : 0) | (truthy(r.get("_flag64")) ? 64 : 0));
    o.set("description", r.get("description"));
    o.set("patternLength", r.get("patternLength"));
    Array els;
    if (r.get("elements").is_array()) {
      const auto& shapes = r.refs("shapes");
      const Array& src = r.get("elements").as_array();
      for (std::size_t i = 0; i < src.size(); ++i) {
        const Value& e = src[i];
        Value x = make_object({{"length", e["length"]}});
        if (truthy(e["flags"])) {
          x.set("flags", e["flags"]);
          x.set("shape", e["shape"]);
          x.set("style", handle_hex(i < shapes.size() ? shapes[i] : 0));
          x.set("offset", e["offset"]);
          x.set("scale", e["scale"]);
          x.set("rotation", e["rotation"]);
          if (!e["text"].is_undefined()) {
            x.set("text", e["text"]);
          }
        }
        els.push_back(std::move(x));
      }
    }
    o.set("elements", std::move(els));
    tlist("linetypes").push_back(std::move(o));
  }
  for (const std::int64_t h : table_recs["styles"]) {
    const Rec& r = *get(h);
    Value o = common_obj(r);
    o.set("name", r.get("name"));
    o.set("flags", (truthy(r.get("isShapeFile")) ? 1 : 0) | (truthy(r.get("vertical")) ? 4 : 0) |
                       (truthy(r.get("_xdep")) ? 16 : 0) | (truthy(r.get("_flag64")) ? 64 : 0));
    for (const std::string_view k :
         {"height", "widthFactor", "oblique", "generation", "lastHeight", "font", "bigFont"}) {
      o.set(k, r.get(k));
    }
    o.set("shapeFile", r.get("isShapeFile"));
    tlist("styles").push_back(std::move(o));
  }
  for (const std::int64_t h : table_recs["dimstyles"]) {
    const Rec& r = *get(h);
    Value vars = r.get("vars").is_object() ? r.get("vars") : Value::object();
    for (const std::string_view k :
         {"DIMTXSTY", "DIMLDRBLK", "DIMBLK", "DIMBLK1", "DIMBLK2", "DIMLTYPE", "DIMLTEX1", "DIMLTEX2"}) {
      if (r.has_h(k)) {
        vars.set(k, handle_hex(r.ref(k)));
        if (auto n = name_of(r.ref(k))) {
          vars.set(std::string(k) + "_NAME", *n);
        }
      }
    }
    Value o = common_obj(r);
    o.set("name", r.get("name"));
    o.set("flags", (truthy(r.get("_xdep")) ? 16 : 0) | (truthy(r.get("_flag64")) ? 64 : 0));
    o.set("vars", std::move(vars));
    tlist("dimstyles").push_back(std::move(o));
  }
  for (const std::string_view t : {"views", "ucs", "vports", "appids"}) {
    for (const std::int64_t h : table_recs[t]) {
      const Rec& r = *get(h);
      Value o = common_obj(r);
      Value p = plain(r);
      for (base::Member& m : p.as_object()) {
        o.set(m.key, std::move(m.value));
      }
      o.set("flags", (truthy(r.get("_xdep")) ? 16 : 0) | (truthy(r.get("_flag64")) ? 64 : 0));
      tlist(t).push_back(std::move(o));
    }
  }
  ms_rec_ = hnum("BLOCK_RECORD_MODEL_SPACE");
  ps_rec_ = hnum("BLOCK_RECORD_PAPER_SPACE");
  for (const std::int64_t h : table_recs["blockRecords"]) {
    const Rec& r = *get(h);
    Value o = common_obj(r);
    o.set("name", r.get("name"));
    o.set("flags", block_flags(r));
    o.set("layout", handle_hex(r.ref("layout")));
    o.set("base", r.get("base"));
    o.set("xrefPath", r.get("xrefPath"));
    o.set("description", r.get("description"));
    if (truthy(r.get("_trueName"))) {
      o.set("trueName", r.get("_trueName"));
    }
    if (!r.get("insertUnits").is_undefined()) {
      o.set("insertUnits", r.get("insertUnits"));
      o.set("explodable", r.get("explodable"));
      o.set("scalable", r.get("scalable"));
    }
    if (r.get("preview").is_bytes()) {
      o.set("preview", base::base64(r.get("preview").as_bytes()));
    }
    tlist("blockRecords").push_back(std::move(o));
  }

  // ---- entities
  std::unordered_map<std::int64_t, std::string> rec_space;  // block record handle → model | paper
  std::unordered_set<std::int64_t> layout_blocks;
  for (const Rec& r : records_->all()) {
    if (r.type == "LAYOUT" && r.ref("block") != 0) {
      layout_blocks.insert(r.ref("block"));
    }
  }
  for (const std::int64_t h : table_recs["blockRecords"]) {
    const Rec& r = *get(h);
    const std::string nm = r.get("name").is_string() ? base::to_upper(r.get("name").as_string()) : std::string();
    if (r.handle == ms_rec_ || nm == "*MODEL_SPACE") {
      rec_space[r.handle] = "model";
    } else if (r.handle == ps_rec_ || nm.starts_with("*PAPER_SPACE") ||
               (layout_blocks.contains(r.handle) && r.get("name").is_string() &&
                r.get("name").as_string().starts_with("*"))) {
      rec_space[r.handle] = "paper";
    }
  }
  // Add countsBySpace before holding a reference into `rep` (a new member may reallocate its members).
  rep.set("countsBySpace",
          make_object({{"model", Value::object()}, {"paper", Value::object()}, {"block", Value::object()}}));
  Value& counts = *rep.find("counts");

  for (const Rec& r : records_->all()) {
    if ((r.type != "ATTRIB" && r.type != "ATTDEF") || r.ref("xdic") == 0) {
      continue;
    }
    if (const Array* data = mlatt_of(r)) {
      for (const Value& g : *data) {
        const Array& ga = g.as_array();
        if (ga.at(0).num_or(-1) == 340 && ga.at(1).is_string()) {
          mlatt_fold_.insert(static_cast<std::int64_t>(std::stoll(ga.at(1).as_string(), nullptr, 16)));
        }
      }
    }
  }

  // ---- blocks
  std::vector<std::int64_t> recs_in_order;
  const auto& brs = table_recs["blockRecords"];
  const bool has_ms = std::ranges::find(brs, ms_rec_) != brs.end();
  const bool has_ps = std::ranges::find(brs, ps_rec_) != brs.end();
  if (has_ms) {
    recs_in_order.push_back(ms_rec_);
  }
  if (has_ps && ps_rec_ != ms_rec_) {
    recs_in_order.push_back(ps_rec_);
  }
  for (const std::int64_t h : brs) {
    if ((h != ms_rec_ || !has_ms) && (h != ps_rec_ || !has_ps)) {
      recs_in_order.push_back(h);
    }
  }
  Array& blocks = model_->find("blocks")->as_array();
  std::unordered_map<std::string, std::size_t> block_by_record;
  for (const std::int64_t bh : recs_in_order) {
    const Rec& br = *get(bh);
    const Rec* blk = get(br.ref("block"));
    Value b = Value::object();
    b.set("name", br.get("name"));
    b.set("handle", handle_hex(br.ref("block")));
    b.set("record", handle_hex(br.handle));
    b.set("base", br.get("base"));
    b.set("flags", block_flags(br));
    b.set("xrefPath", br.get("xrefPath"));
    b.set("description", truthy(br.get("description")) ? br.get("description") : Value(""));
    b.set("entities", Array{});
    b.set("endHandle", handle_hex(br.ref("endblk")));
    if (truthy(br.get("_trueName"))) {
      b.set("trueName", br.get("_trueName"));
    }
    if (blk != nullptr) {
      used_.insert(blk->handle);
      b.set("layer", layer_name(blk->ref("layer")));
      Value xd = xdata_of(*blk);
      if (!xd.is_undefined()) {
        b.set("xdata", std::move(xd));
      }
    }
    if (const std::int64_t eb = br.ref("endblk"); eb != 0) {
      used_.insert(eb);
    }
    std::vector<std::int64_t> eh;
    if (br.has_h("entities")) {
      eh = br.refs("entities");
    } else if (br.ref("firstEntity") != 0) {
      eh = chain(br.ref("firstEntity"), br.ref("lastEntity"));
    }
    const auto sp = rec_space.find(br.handle);
    const std::string space = sp != rec_space.end() ? sp->second : std::string();
    Value& s = *rep.find("countsBySpace")->find(space.empty() ? "block" : space);
    Array ents;
    for (const std::int64_t h : eh) {
      Rec* r = get(h);
      if (r == nullptr || !r->is_entity || sub_type(r->type)) {
        continue;
      }
      if (mlatt_fold_.contains(h)) {
        used_.insert(h);
        continue;
      }
      Value e = convert(*r, br.handle);
      if (!space.empty()) {
        e.set("space", space);
      }
      bump(counts, count_type(e));
      bump(s, count_type(e));
      if (Value* atts = e.find("attribs"); atts != nullptr && atts->is_array()) {
        for (Value& x : atts->as_array()) {
          if (!space.empty()) {
            x.set("space", space);
          }
          bump(counts, count_type(x));
          bump(s, count_type(x));
        }
      }
      ents.push_back(std::move(e));
    }
    b.set("entities", std::move(ents));
    block_by_record[handle_hex(br.handle)] = blocks.size();
    blocks.push_back(std::move(b));
  }

  // ---- entities nobody lists (damaged chains): keep them in their owner block, noted
  int orphans = 0;
  for (Rec& r : records_->all()) {
    if (!r.is_entity || used_.contains(r.handle) || sub_type(r.type)) {
      continue;
    }
    const std::int64_t owner = owner_of(r);
    const auto bi = block_by_record.find(handle_hex(owner));
    if (bi == block_by_record.end()) {
      continue;
    }
    Value e = convert(r, owner);
    const auto sp = rec_space.find(owner);
    if (sp != rec_space.end()) {
      e.set("space", sp->second);
    }
    bump(counts, count_type(e));
    blocks[bi->second].find("entities")->push(std::move(e));
    ++orphans;
  }
  if (orphans != 0) {
    rep.find("notes")->push(msg("ccdwg.read.orphans", {{"n", orphans}}));
  }

  // ---- layouts
  Array& layouts = model_->find("layouts")->as_array();
  for (const Rec& r : records_->all()) {
    if (r.type != "LAYOUT") {
      continue;
    }
    const Value p = r.get("plot").is_object() ? r.get("plot") : Value::object();
    const std::int64_t block_rec = r.ref("block");
    Array vps;
    if (r.has_h("viewports")) {
      vps = hex_list(r.refs("viewports"));
    } else if (const auto bi = block_by_record.find(handle_hex(block_rec)); bi != block_by_record.end()) {
      for (const Value& e : blocks[bi->second]["entities"].as_array()) {
        if (e["type"].is_string() && e["type"].as_string() == "VIEWPORT") {
          vps.push_back(e["handle"]);
        }
      }
    }
    Value o = common_obj(r);
    o.set("name", r.get("name"));
    o.set("tabOrder", r.get("tabOrder"));
    o.set("flags", r.get("flags"));
    o.set("block", handle_hex(block_rec));
    Value paper = Value::object();
    paper.set("name", p["paperSize"]);
    paper.set("w", p["paperWidth"]);
    paper.set("h", p["paperHeight"]);
    paper.set("units", p["paperUnits"]);
    paper.set("margins", p["margins"]);
    paper.set("rotation", p["rotation"]);
    paper.set("plotType", p["plotType"]);
    paper.set("scaleType", p["scaleType"]);
    paper.set("scaleFactor", p["scaleFactor"]);
    paper.set("plotScale", truthy(p["drawingUnits"])
                               ? Value(p["realWorldUnits"].num_or(std::nan("")) / p["drawingUnits"].as_number())
                               : Value());
    paper.set("realWorldUnits", p["realWorldUnits"]);
    paper.set("drawingUnits", p["drawingUnits"]);
    paper.set("origin", p["origin"]);
    paper.set("pageSetupName", p["pageSetupName"]);
    paper.set("printer", p["printer"]);
    paper.set("styleSheet", p["styleSheet"]);
    paper.set("flags", p["flags"]);
    paper.set("windowMin", p["windowMin"]);
    paper.set("windowMax", p["windowMax"]);
    o.set("paper", std::move(paper));
    o.set("limits", make_object({{"min", r.get("limMin")}, {"max", r.get("limMax")}}));
    o.set("insbase", r.get("insBase"));
    o.set("extents", make_object({{"min", r.get("extMin")}, {"max", r.get("extMax")}}));
    o.set("ucs",
          make_object({{"origin", r.get("ucsOrigin")}, {"xAxis", r.get("ucsXAxis")}, {"yAxis", r.get("ucsYAxis")}}));
    o.set("elevation", r.get("elevation"));
    o.set("lastActiveViewport", handle_hex(r.ref("lastViewport")));
    o.set("viewports", std::move(vps));
    layouts.push_back(std::move(o));
  }
  std::ranges::stable_sort(
      layouts, [](const Value& a, const Value& b) { return a["tabOrder"].num_or(0) < b["tabOrder"].num_or(0); });

  // ---- non-graphical objects
  Array& objects = model_->find("objects")->as_array();
  Value& unknown = *rep.find("unknown");
  for (const Rec& r : records_->all()) {
    const bool table_thing = !table_of(r.type).empty() || r.type.ends_with("_CONTROL") || r.type == "VPENT_HEADER";
    if (r.is_entity || table_thing) {
      continue;
    }
    Value o = common_obj(r);
    o.set("type", r.type);
    if (r.unknown) {
      o.set("undecoded", true);
      o.set("typeNum", r.type_num);
      if (r.class_name && !r.class_name->empty()) {
        o.set("className", *r.class_name);
      }
      bump(unknown, r.type);
    } else {
      Value f = object_fields(r);
      for (base::Member& m : f.as_object()) {
        o.set(m.key, std::move(m.value));
      }
    }
    if (r.error) {
      o.set("damaged", *r.error);
    }
    if (auto raw = capture_raw(r, ver)) {
      o.set("raw", std::move(*raw));
    }
    objects.push_back(std::move(o));
  }
  std::ranges::stable_sort(objects, [](const Value& a, const Value& b) {
    const auto ha = a["handle"].as_string().empty() ? 0ULL : std::stoull(a["handle"].as_string(), nullptr, 16);
    const auto hb = b["handle"].as_string().empty() ? 0ULL : std::stoull(b["handle"].as_string(), nullptr, 16);
    return ha < hb;
  });
  if (build_errors_ != 0) {
    rep.find("notes")->push(msg("ccdwg.read.kept_common", {{"n", build_errors_}}));
  }
}

Value new_model() {
  Value tables = make_object({{"layers", Array{}},
                              {"linetypes", Array{}},
                              {"styles", Array{}},
                              {"dimstyles", Array{}},
                              {"views", Array{}},
                              {"ucs", Array{}},
                              {"vports", Array{}},
                              {"appids", Array{}},
                              {"blockRecords", Array{}}});
  return make_object(
      {{"version", "AC1015"},
       {"maintenance", 0},
       {"codepage", "ANSI_1252"},
       {"header", Value::object()},
       {"classes", Array{}},
       {"tables", std::move(tables)},
       {"blocks", Array{}},
       {"layouts", Array{}},
       {"objects", Array{}},
       {"report", make_object({{"counts", Value::object()}, {"notes", Array{}}, {"unknown", Value::object()}})}});
}

}  // namespace

Value read_dwg(std::span<const std::byte> input, const ReadOptions& opts) {
  const auto t0 = std::chrono::steady_clock::now();
  const Bytes bytes = base::as_u8(input);
  Value model = new_model();
  Value& report = *model.find("report");
  std::vector<std::string> notes;
  auto flush_notes = [&]() {
    for (std::string& n : notes) {
      report.find("notes")->push(std::move(n));
    }
    notes.clear();
  };
  if (bytes.size() > opts.max_bytes) {
    report.set("error", "the file is larger than the reader's limit");
    report.find("notes")->push(msg("ccdwg.open.failed", {{"reason", "the file is larger than the reader's limit"}}));
    return model;
  }
  Container c;
  try {
    c = read_container(bytes);
  } catch (const std::exception& e) {
    std::string v;
    for (std::size_t i = 0; i < 6 && i < bytes.size(); ++i) {
      const auto ch = bytes[i];
      if (ch >= 0x20 && ch <= 0x7E) {
        v += static_cast<char>(ch);
      } else {
        // String.fromCharCode of a byte ≥ 0x80 is one UTF-16 unit: not printable ASCII → "?"
        v += '?';
      }
    }
    model.set("version", v);
    const std::string m = error_text(e);
    report.set("error", m);
    report.find("notes")->push(
        dynamic_cast<const Unsupported*>(&e) != nullptr ? m : msg("ccdwg.open.failed", {{"reason", m}}));
    return model;
  }
  model.set("version", c.version);
  model.set("maintenance", c.maintenance);
  for (std::string& n : c.notes) {
    notes.push_back(std::move(n));
  }
  Ctx ctx;
  ctx.cp = codepage_for(c.codepage_num);
  model.set("codepage", ctx.cp.name);
  if (!ctx.cp.known) {
    notes.push_back(msg("ccdwg.read.codepage", {{"num", c.codepage_num}, {"name", ctx.cp.name}}));
  }
  const int ver = c.ver;
  ctx.ver = ver;
  ctx.maint = c.maintenance;
  ctx.notes = &notes;

  // ---- header variables (ch. 9)
  try {
    model.set("header", read_header_section(c.section("AcDb:Header"), ctx));
  } catch (const std::exception& e) {
    notes.push_back(msg("ccdwg.read.header_failed", {{"reason", error_text(e)}}));
  }
  Value& H = *model.find("header");
  H.set("ACADVER", c.version);
  H.set("ACADMAINTVER", c.maintenance);
  H.set("DWGCODEPAGE", ctx.cp.name);
  if (const Value* v = H.find("OBSCUREDCOLOR"); v != nullptr && !v->is_undefined()) {
    H.set("OBSCOLOR", *v);
  }
  if (const Value* v = H.find("OBSCUREDLTYPE"); v != nullptr && !v->is_undefined()) {
    H.set("OBSLTYPE", *v);
  }
  if (const Value* v = H.find("FLAGS"); v != nullptr && !v->is_undefined()) {
    H.set("CELWEIGHT", lineweight_from_index(static_cast<double>(static_cast<std::int64_t>(v->num_or(0)) & 0x1F)));
  }
  // MEASUREMENT lives in AcDb:Template (ch. 22). Spec gap: from R2007 the description length counts UTF-16 units.
  if (const Section* tpl = c.section("AcDb:Template"); tpl != nullptr && tpl->view.size() >= 4) {
    const Bytes t = tpl->view;
    const std::size_t n = (t[0] | (static_cast<std::size_t>(t[1]) << 8U)) * (ver >= 21 ? 2U : 1U);
    if (2 + n + 2 <= t.size()) {
      H.set("MEASUREMENT", t[2 + n] | (static_cast<std::uint32_t>(t[3 + n]) << 8U));
      if (n != 0) {
        if (ver >= 21) {
          U16 u;
          for (std::size_t i = 0; i + 1 < n; i += 2) {
            u.push_back(static_cast<std::uint16_t>(t[2 + i] | (t[3 + i] << 8U)));
          }
          while (!u.empty() && u.back() == 0) {
            u.pop_back();
          }
          H.set("TEMPLATE_DESCRIPTION", utf8(u));
        } else {
          H.set("TEMPLATE_DESCRIPTION", decode_text(ctx.cp, t.subspan(2, n)));
        }
      }
    }
  }

  // ---- classes (ch. 10)
  try {
    std::vector<ClassInfo> classes = read_classes_section(c.section("AcDb:Classes"), ctx);
    Array& out = model.find("classes")->as_array();
    for (ClassInfo& k : classes) {
      Value o = make_object({{"num", k.num},
                             {"proxyFlags", k.proxy_flags},
                             {"appName", k.app_name},
                             {"cppName", k.cpp_name},
                             {"dxfName", k.dxf_name},
                             {"wasZombie", k.was_zombie},
                             {"isEntity", k.is_entity}});
      if (k.has_r18) {
        o.set("instanceCount", k.instance_count);
        o.set("dwgVersion", k.dwg_version);
        o.set("maintVersion", k.maint_version);
      }
      out.push_back(std::move(o));
      ctx.classes[k.num] = std::move(k);
    }
  } catch (const std::exception& e) {
    notes.push_back(msg("ccdwg.read.classes_failed", {{"reason", error_text(e)}}));
  }

  // ---- object map (ch. 23) and objects (ch. 20)
  std::vector<std::pair<std::int64_t, std::int64_t>> map;
  try {
    map = read_object_map(c.section("AcDb:Handles"), ctx);
  } catch (const std::exception& e) {
    notes.push_back(msg("ccdwg.read.map_failed", {{"reason", error_text(e)}}));
  }
  Records records;
  int damaged = 0;
  int crc_bad = 0;
  std::vector<std::pair<std::string, int>> damaged_types;
  if (const Section* objs = c.section("AcDb:AcDbObjects")) {
    // Decoding is independent per object (decoders read only the bytes and the immutable context): split the map
    // across threads, each result in its slot, then take them in map order exactly as one thread would.
    struct Decoded {
      std::optional<Rec> rec;
      std::string error;  // the exception's text when decoding threw
    };
    std::vector<Decoded> decoded(map.size());
    auto decode_range = [&](std::size_t from, std::size_t to) {
      for (std::size_t i = from; i < to; ++i) {
        const auto [handle, off] = map[i];
        try {
          if (off < 0) {
            throw DecodeError(msg("ccdwg.err.offset_outside"));
          }
          decoded[i].rec = decode_object(objs->view, static_cast<std::size_t>(off), ctx);
        } catch (const std::exception& e) {
          decoded[i].error = error_text(e);
        }
      }
    };
    unsigned threads =
        opts.threads != 0 ? opts.threads : std::min(8U, std::max(1U, std::thread::hardware_concurrency()));
    threads = static_cast<unsigned>(std::min<std::size_t>(threads, map.size() / 256 + 1));  // small files: one thread
    if (threads <= 1) {
      decode_range(0, map.size());
    } else {
      std::vector<std::jthread> pool;
      const std::size_t chunk = (map.size() + threads - 1) / threads;
      for (unsigned t = 1; t < threads; ++t) {
        const std::size_t from = std::min(map.size(), chunk * t);
        const std::size_t to = std::min(map.size(), from + chunk);
        // by value: `pool` is joined when the block ends, after the locals declared below it are gone
        pool.emplace_back([&decode_range, from, to] { decode_range(from, to); });
      }
      decode_range(0, std::min(map.size(), chunk));
    }  // the jthreads join here
    for (std::size_t i = 0; i < map.size(); ++i) {
      const std::int64_t handle = map[i].first;
      Decoded& d = decoded[i];
      if (!d.rec.has_value()) {
        ++damaged;
        if (damaged <= 20) {
          notes.push_back(
              msg("ccdwg.read.object_failed", {{"handle", handle_hex(handle)}, {"reason", decoded[i].error}}));
        }
        continue;
      }
      Rec rec = std::move(d.rec.value());
      d.rec.reset();
      if (rec.handle != handle) {
        ++damaged;
        if (damaged <= 20) {
          notes.push_back(
              msg("ccdwg.read.map_entry", {{"handle", handle_hex(handle)}, {"other", handle_hex(rec.handle)}}));
        }
        continue;
      }
      if (rec.crc_bad) {
        ++crc_bad;
      }
      if (rec.error) {
        auto it = std::ranges::find_if(damaged_types, [&](const auto& x) { return x.first == rec.type; });
        if (it == damaged_types.end()) {
          damaged_types.emplace_back(rec.type, 1);
        } else {
          ++it->second;
        }
      }
      if (records.get(handle) != nullptr) {
        continue;  // a JS Map set on an existing key keeps the first place; the object map never repeats a handle
      }
      records.add(std::move(rec));
    }
  } else {
    notes.push_back(msg("ccdwg.read.no_objects"));
  }
  if (damaged > 20) {
    notes.push_back(msg("ccdwg.read.more_failed", {{"n", damaged - 20}}));
  }
  if (crc_bad != 0) {
    notes.push_back(msg("ccdwg.read.crc_failed", {{"n", crc_bad}}));
  }
  for (const auto& [t, n] : damaged_types) {
    notes.push_back(msg("ccdwg.read.partial_type", {{"n", n}, {"type", t}}));
  }
  if (ver >= 27) {
    const Section* ds = c.section("AcDb:AcDsPrototype_1b");
    const Bytes dsv = ds != nullptr ? ds->view : Bytes{};
    attach_data_storage(records, ds != nullptr ? &dsv : nullptr, notes);
  }
  flush_notes();

  try {
    Builder b(model, records, ctx);
    b.run();
  } catch (const std::exception& e) {
    report.find("notes")->push(msg("ccdwg.read.assembled_part", {{"reason", error_text(e)}}));
  }
  flush_notes();
  try {
    read_dynamic(model);
  } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
    // As in read.js: dynamic-block data never stops a read; the blocks stay as they are drawn.
  }
  const auto t1 = std::chrono::steady_clock::now();
  report.set("ms", std::round(std::chrono::duration<double, std::milli>(t1 - t0).count()));
  report.set("objects", static_cast<double>(records.size()));
  return model;
}

}  // namespace cc::dwg
