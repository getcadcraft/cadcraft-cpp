// SPDX-License-Identifier: Apache-2.0
// cc::cmd — CadCraft 0.5's clipboard (clipboard.js) and blockcopy.js: what COPYCLIP / CUTCLIP / COPYBASE put away, and
// how PASTECLIP / PASTEORIG bring it back — the objects without their ids, the blocks they need (nested ones first)
// and their layers' settings. A block this drawing already has keeps this drawing's definition; anonymous blocks get a
// free name. The clipboard lives in the host, so a copy survives opening another drawing.
//
//   auto clip = make_clip(doc, ids, base, now_ms);          // nothing when there is nothing to copy
//   host.clipboard().put(*clip);
//   doc.change("PASTECLIP", [&] { r = paste_clip(doc, *clip, dx, dy, words); });
//
// Not ported yet (docs/gaps.md): sharing between app windows (0.5 keeps a copy in the browser's storage and reads it
// back through readClip).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/doc/doc.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::cmd {

/// makeClip(doc, ids, { base }): the clipboard contents (a plain object), or nothing. `skipped`: viewports left out.
[[nodiscard]] std::optional<base::Value> make_clip(const doc::Doc& doc, const std::vector<doc::Id>& ids,
                                                   const std::optional<std::pair<double, double>>& base_pt,
                                                   double now_ms);

/// The clipboard: this window's copy (0.5's Clipboard with no storage).
class Clipboard {
 public:
  void put(base::Value clip) { mem_ = std::move(clip); }
  [[nodiscard]] const std::optional<base::Value>& get() const noexcept { return mem_; }

 private:
  std::optional<base::Value> mem_;
};

/// basePoint(clip, mode): the COPYBASE point, else the lower left (or the centre) of the copy's extents.
[[nodiscard]] std::pair<double, double> clip_base_point(const base::Value& clip, std::string_view mode = "lower-left");
/// pastePreview(clip, max): what to draw while it follows the cursor, at the copy's own place.
[[nodiscard]] base::Array paste_preview(const base::Value& clip, std::size_t max = 20000);

struct PasteResult {
  std::vector<doc::Id> ids;
  std::vector<std::pair<std::string, std::string>> renamed;  // block old → new, in the order they were renamed
  std::vector<std::string> layers;                           // layers added
  std::string note;                                          // the units differ
  double constraints = 0;                                    // constraints added among the pasted objects
};
/// pasteClip(doc, clip, dx, dy): the copy added to the open space, moved by (dx, dy) — inside doc.change.
PasteResult paste_clip(doc::Doc& doc, const base::Value& clip, double dx, double dy, const i18n::Catalog& words);

// ---- blockcopy.js

struct CopyPlan {
  std::optional<std::string> root;  // the block to place (whole: a new one named after the file)
  bool whole = false;
  std::vector<std::string> needed;   // nested ones first
  std::vector<std::string> clashes;  // named blocks this drawing already has
  double scale = 1;
  std::string note;
  std::optional<std::pair<double, double>> base;
};
/// planCopy(doc, src, { block | whole | refs }): which definitions come along. `block` must exist in src.
[[nodiscard]] CopyPlan plan_copy(const doc::Doc& doc, const base::Value& src, const std::optional<std::string>& block,
                                 const std::optional<std::string>& whole, bool refs, const i18n::Catalog& words);
struct CopyResult {
  std::optional<std::string> name;
  std::size_t copied = 0;
  std::vector<std::pair<std::string, std::string>> renamed;
  std::vector<std::string> layers;
};
/// applyCopy(doc, src, plan, 'keep' | 'rename'): the definitions copied in (inside doc.change).
CopyResult apply_copy(doc::Doc& doc, const base::Value& src, const CopyPlan& plan, bool keep_on_clash);

}  // namespace cc::cmd
