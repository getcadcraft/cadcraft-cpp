// SPDX-License-Identifier: Apache-2.0
// cc::ifc model — cc-ifc's model.js: one open IFC file. The bytes, the instance index (scan_step), lines parsed on
// first read (kept in a cache of up to 250,000), edits kept beside the original bytes, and saving (untouched lines
// copied byte for byte, changed ones written anew).
//
//   IfcFile::open(bytes)             → the file, or a StepError (not Part 21)
//   f.type(id)                       "IFCWALL", "" for a complex instance, nullopt when there is no such line
//   f.params(id)                     the raw parameter list (shared, immutable), or null; throws ParseFailure when the
//                                    line does not parse (the JS throws there too; the geometry and the API catch it)
//   f.attr(id, "Name")               an attribute by name (AttrValue: undefined / a value kept alive with its line)
//   f.ids()  f.of_type(t, inherited)  f.types_present()  f.inverse(type, attr)  f.refs_to(id)
//   f.set(id, type, params)  f.add(type, params)  f.remove(id)  f.save(time_stamp)
//
// Ids are the JS numbers model.js keys its maps with: the index's Int32 ids, and the positive whole numbers set()
// takes. Not thread-safe (it caches as it reads, as the JS does): one file per thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cc/ifc/schema.hpp"
#include "cc/ifc/step.hpp"

namespace cc::ifc {

using Id = std::int64_t;
using Params = std::shared_ptr<const RawList>;

/// A line that does not parse, read where model.js throws: params, of_type, types_present, attr, refs_to and inverse
/// throw it (they parse lines on demand, as the JS does), and callers that mirror a JS try/catch catch it — the
/// geometry, the IfcAPI, BimCraft, the CLI (docs/architecture.md, Errors). Everything else returns a Result.
class ParseFailure : public std::exception {
 public:
  explicit ParseFailure(StepError e) : e_(std::move(e)) {}
  [[nodiscard]] const char* what() const noexcept override { return e_.message.c_str(); }
  [[nodiscard]] const StepError& error() const noexcept { return e_; }

 private:
  StepError e_;
};

/// An attribute's value: undefined (no such attribute or line), or the raw value, kept alive with its parameter list.
struct AttrValue {
  Params owner;
  const Raw* v = nullptr;
  [[nodiscard]] bool undefined() const noexcept { return v == nullptr; }
  [[nodiscard]] const Raw* operator->() const noexcept { return v; }
  [[nodiscard]] const Raw& operator*() const noexcept { return *v; }
};

class IfcFile {
 public:
  [[nodiscard]] static StepResult<std::unique_ptr<IfcFile>> open(std::vector<std::byte> bytes);
  IfcFile(std::vector<std::byte> bytes, StepIndex ix);

  [[nodiscard]] const Schema& schema() const noexcept { return *schema_; }
  [[nodiscard]] std::string_view schema_name() const noexcept { return schema_->name(); }
  [[nodiscard]] bool schema_guessed() const noexcept { return guessed_; }
  [[nodiscard]] const std::string& file_schema() const noexcept { return ix_.schema; }
  [[nodiscard]] const StepIndex& index() const noexcept { return ix_; }
  [[nodiscard]] double max_id() const noexcept { return max_id_; }
  [[nodiscard]] Bytes bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::uint64_t version() const noexcept { return version_; }

  // ---- reading
  [[nodiscard]] bool has(Id id) const { return type(id).has_value(); }
  [[nodiscard]] std::optional<std::string_view> type(Id id) const;
  [[nodiscard]] Params params(Id id);
  [[nodiscard]] std::optional<std::vector<ComplexPart>> complex(Id id) const;
  [[nodiscard]] std::vector<Id> ids() const;
  [[nodiscard]] std::vector<Id> new_ids() const;
  [[nodiscard]] std::size_t count() const { return ids().size(); }
  [[nodiscard]] std::vector<Id> of_type(std::string_view up, bool inherited = false);
  /// Every entity type present with its count, in the JS Map's order.
  [[nodiscard]] std::vector<std::pair<std::string, std::size_t>> types_present();
  [[nodiscard]] AttrValue attr(Id id, std::string_view name);

  // ---- inverse references (rebuilt after edits)
  [[nodiscard]] const std::vector<Id>& refs_to(Id id);
  /// Relations of one entity type keyed by the target of one attribute: element id → relation ids (file order).
  [[nodiscard]] const std::unordered_map<Id, std::vector<Id>>& inverse(std::string_view up, std::string_view attr,
                                                                       bool inherited = true);

  // ---- writing
  /// Writes a line (a new id or a replacement); a bad id → an Error worded as the JS.
  base::Result<Id> set(double id, std::string_view type, RawList params);
  base::Result<Id> add(std::string_view type, RawList params) { return set(max_id_ + 1, type, std::move(params)); }
  bool remove(Id id);
  [[nodiscard]] bool dirty() const noexcept { return !edits_.empty() || !gone_.empty(); }
  /// The whole file; `time_stamp` replaces FILE_NAME's (the JS default is the current UTC time, to the second).
  [[nodiscard]] base::Result<std::string> save(std::string_view time_stamp);
  /// "#id=TYPE(…);" for a written line.
  [[nodiscard]] base::Result<std::string> format_line(Id id, std::string_view type, const RawList& params) const;

 private:
  struct Edit {
    std::string type;
    Params params;
  };
  [[nodiscard]] std::ptrdiff_t rec(Id id) const;
  void index_types();
  [[nodiscard]] bool real_at(std::string_view type, std::size_t k) const;
  [[nodiscard]] base::Result<std::string> header_text(std::string_view time_stamp) const;

  std::vector<std::byte> bytes_;
  StepIndex ix_;
  const Schema* schema_ = nullptr;
  bool guessed_ = false;
  double max_id_ = 0;
  bool slots_ = true;  // model.js keeps an Int32Array of slots when ids are dense: negative ids then find nothing
  std::unordered_map<Id, std::size_t> rec_;  // record of an id (sparse ids)
  std::vector<std::uint32_t> slot_;          // record of an id (dense ids: model.js's Int32Array); kNoSlot = none
  std::vector<Id> edit_order_;               // Map insertion order
  std::unordered_map<Id, Edit> edits_;
  std::set<Id> gone_;
  std::vector<Params> cache_;  // parsed lines by record (original lines only); cleared past 250,000 as model.js
  std::size_t cached_ = 0;
  std::uint64_t version_ = 0;
  std::unordered_map<std::string_view, std::vector<Id>> by_type_;
  bool by_type_ready_ = false;
  std::optional<std::unordered_map<Id, std::vector<Id>>> refs_;
  std::uint64_t refs_version_ = 0;
  std::map<std::string, std::unordered_map<Id, std::vector<Id>>> inv_;
  std::uint64_t inv_version_ = 0;
};

// ---- Part 21 text (step.js writing)
/// A string as a Part 21 token: printable ASCII as it is, '' and \\ doubled, the rest as \X2\ (or \X4\).
[[nodiscard]] std::string format_string(std::string_view s);
/// A REAL token: always with a ".", exponent as E; a non-finite value → an Error ("A real value must be finite").
[[nodiscard]] base::Result<std::string> format_real(double x);
/// The Part 21 text of a raw value. real_at(path) says whether a bare number at that path is a REAL; real_type(name)
/// whether a typed value's type is.
[[nodiscard]] base::Result<std::string> format_value(const Raw& v, const std::function<bool(std::string_view)>& real_at,
                                                     const std::function<bool(std::string_view)>& real_type,
                                                     const std::string& path = "");

}  // namespace cc::ifc
