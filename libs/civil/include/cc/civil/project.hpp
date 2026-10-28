// SPDX-License-Identifier: Apache-2.0
// CivilCraft 0.5's project.js: the project (survey points, surface rules, alignments with their profiles and corridors,
// pads, feature lines, parcels, the pipe network, GIS layers), one undo step per change, everything derived from it
// (surfaces, contours, profiles, the graded surface, sections, volumes, the network check), and the project file.
// The state is the JS's JSON object (base::Value) so its snapshots, the file and the views are the same text.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/civil/alignment.hpp"
#include "cc/civil/tin.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::civil {

[[nodiscard]] base::Value display_defaults();
[[nodiscard]] base::Value empty_state(const i18n::Catalog& words);

class Project {
 public:
  explicit Project(const i18n::Catalog& words);

  base::Value state;
  base::Value points = base::Value::array();
  std::string points_file;
  double points_version = 0, version = 0, saved = 0, gis_version = 0;
  base::Value gis_features = base::Value::object();  // layer id → [{g, p}], as a JS Map in insertion order
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> crs_grids;
  std::vector<std::pair<std::string, std::string>> gis_images;

  struct Step {
    std::string label;
    std::string json;
    std::optional<base::Value> pts;  // the points a point step replaced
    std::optional<base::Value> gis;
    bool tin_set = false;
    base::Value tin;
  };
  std::vector<Step> undo_stack, redo_stack;

  [[nodiscard]] bool dirty() const { return version != saved; }
  [[nodiscard]] std::string snapshot() const;
  void restore(const std::string& json);
  void set_display(const std::string& k, const base::Value& v);
  /// change(label, fn): fn edits `state`; one undo step when it changed anything
  bool change(const std::string& label, const std::function<void(base::Value&)>& fn);
  /// editPoints(label, fn): fn gets a copy of the points and the state, returns the new points or nullopt
  bool edit_points(const std::string& label,
                   const std::function<std::optional<base::Value>(base::Value, base::Value&)>& fn);
  bool set_tin(const std::string& label, const base::Value& tin, const std::function<void(base::Value&)>* fn);
  std::optional<std::string> undo();
  std::optional<std::string> redo();
  std::string next_id(const std::string& prefix);
  void set_points(const base::Value& pts, const std::string& file);
  [[nodiscard]] base::Value groups();
  bool set_coding(const base::Value& keys, const base::Value& line_codes, const std::string& label);
  [[nodiscard]] base::Value figures(const std::vector<std::string>* only);

  // ---- derived (memoised as the JS memoises, so the same objects come back)
  [[nodiscard]] base::Value eg_breaklines();
  [[nodiscard]] base::Value eg_points();
  struct Eg {
    std::shared_ptr<Surface> surface;
    base::Value report;
  };
  [[nodiscard]] const Eg& eg();
  [[nodiscard]] base::Value contours();
  [[nodiscard]] std::shared_ptr<Alignment> alignment(const base::Value& a);
  [[nodiscard]] base::Value eg_profile(const base::Value& a);
  [[nodiscard]] std::shared_ptr<Profile> design_profile(const base::Value& a);
  struct Graded {
    std::shared_ptr<Surface> surface;
    base::Value report, regions, items;  // items: [{id, kind, name, problems, region, …info}]
  };
  [[nodiscard]] const Graded& graded();
  [[nodiscard]] std::shared_ptr<base::Value> sections(const base::Value& a);
  [[nodiscard]] base::Value volumes(const base::Value& cell);
  [[nodiscard]] base::Value network_check();

  // ---- the project file
  [[nodiscard]] base::Value to_json() const;
  struct Loaded {
    std::shared_ptr<Project> project;
    base::Value notes;
    std::optional<std::string> error;  // the JS's thrown message
  };
  /// Project.fromJSON(obj); a ScriptError-style message on refusal
  [[nodiscard]] static Loaded from_json(const base::Value& obj, const i18n::Catalog& words);

  [[nodiscard]] const i18n::Catalog& words() const { return *words_; }

 private:
  const i18n::Catalog* words_;
  void touch() { version++; }
  [[nodiscard]] std::string surface_deps() const;
  [[nodiscard]] std::string grade_deps() const;
  [[nodiscard]] base::Value default_use(const std::string& group) const;
  std::map<std::string, std::pair<std::string, std::shared_ptr<void>>> cache_;
  template <class T, class F>
  std::shared_ptr<T> memo(const std::string& key, const std::string& deps, F f);
};

/// The ops-civil applyOps: [{set | push | del: path, value}] on a JSON object
void apply_ops(base::Value& state, const base::Value& ops);

}  // namespace cc::civil
