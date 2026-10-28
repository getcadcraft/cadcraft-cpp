// SPDX-License-Identifier: Apache-2.0
// cc::m3d — L2. The ModelCraft kernel: CadCraft 0.5's model.js (ModelCraft's copy, assets/apps/modelcraft/dev), ported
// function for function with the JS's arithmetic and the JS's iteration order, so the same edits give the same model
// JSON (tools/parity/m3d.mjs). CadCraft's 3D tab carries an older copy (its faceTris has no quad shortcut): the
// functions that differ take a `Kernel`.
//
// A model is a set of definitions (Def). The root definition is the model itself; a group is a definition with one
// instance, a component a definition with any number of instances. Each definition holds vertices (meters, Z up),
// edges between two vertices (soft, curve id, hidden), faces (loops[0] outer, counter-clockwise from the front; the
// rest holes; every loop segment is also an edge) and instances of other definitions (a 4×4 column-major matrix).
// Ids of vertices, edges, faces and instances come from one counter per definition (`next`).
//
// Every edit of a Model goes through `change(label, fn)` (one undo step) or a preview session. Errors the JS throws
// (plain Errors with words from the m3d.* catalog keys) come back as `Result` errors with the same words.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"
#include "cc/m3d/idmap.hpp"
#include "cc/m3d/vec.hpp"

namespace cc::i18n {
class Catalog;
}

namespace cc::m3d {

/// Which copy of model.js a call follows: ModelCraft's (the newest) or CadCraft 0.5's own m3d/model.js.
enum class Kernel : std::uint8_t { modelcraft, cadcraft };

/// The words the kernel's errors and names use (i18n.js t() with the app's catalog); unset: the key itself.
void set_words(const i18n::Catalog* words) noexcept;
[[nodiscard]] std::string tr(std::string_view key,
                             std::initializer_list<std::pair<std::string_view, std::string>> fills = {});

struct Edge {
  Id a = 0, b = 0;
  bool soft = false;
  std::string curve;  // 'arc:12' … ('' none)
  bool hidden = false;
  friend bool operator==(const Edge&, const Edge&) = default;
};
using Loop = std::vector<Id>;
struct Face {
  std::vector<Loop> loops;  // [0] outer (CCW from the front), the rest holes
  std::string color;        // paint ('' none)
  bool hidden = false;
  friend bool operator==(const Face&, const Face&) = default;
};
/// How a part follows when its component stretches (0.4 M26, dynamic.js): x, y, z rules and sx, sy, sz factors.
struct InstRule {
  std::optional<std::string> x, y, z;
  std::optional<double> sx, sy, sz;
  friend bool operator==(const InstRule&, const InstRule&) = default;
};
struct Inst {
  std::string def;
  Mat4 m = ident();
  std::string name;   // '' none
  std::string color;  // '' none
  bool hidden = false;
  std::string tag;  // '' untagged
  std::optional<InstRule> rule;
  friend bool operator==(const Inst&, const Inst&) = default;
};
struct Spline {
  std::vector<Vec3> pts;
  bool closed = false;
  int segs = 8;
  friend bool operator==(const Spline&, const Spline&) = default;
};
struct Dyn {
  std::string src;
  Vec3 size{};
  friend bool operator==(const Dyn&, const Dyn&) = default;
};

/// model.js Def.
struct Def {
  std::string id, kind = "root", name;
  IdMap<Vec3> verts;
  IdMap<Edge> edges;
  IdMap<Face> faces;
  IdMap<Inst> insts;
  OrderedMap<std::string, Spline> splines;  // curve id → the points clicked (0.4 M27)
  Id next = 1;
  std::string starter;     // 0.4 M25 ('' none)
  std::optional<Dyn> dyn;  // 0.4 M26

  Def() = default;
  Def(std::string id_, std::string kind_, std::string name_)
      : id(std::move(id_)), kind(std::move(kind_)), name(std::move(name_)) {}

  /// The edge between a and b, or nothing (edgeBetween).
  [[nodiscard]] std::optional<Id> edge_between(Id a, Id b) const;
  /// The vertex; a face or edge naming a missing one is a kernel bug (std::logic_error, caught by the edit guard).
  [[nodiscard]] const Vec3& vert(Id v) const;
  [[nodiscard]] std::vector<Vec3> points(const Loop& loop) const;
  /// Drops the holes deleted entries left in the tables (between operations only).
  void compact();
  /// Forget the edge index (it is rebuilt on the next lookup).
  void reset_index() { ek_.reset(); }
  /// The edge index, built on first use (def._ek).
  std::unordered_map<std::uint64_t, Id>& ek() const;

  friend bool operator==(const Def& a, const Def& b) {
    return a.id == b.id && a.kind == b.kind && a.name == b.name && a.next == b.next && a.starter == b.starter &&
           a.dyn == b.dyn && a.verts == b.verts && a.edges == b.edges && a.faces == b.faces && a.insts == b.insts &&
           a.splines == b.splines;
  }

 private:
  mutable std::optional<std::unordered_map<std::uint64_t, Id>> ek_;
};

/// key(a, b) as one number (the JS's "a|b" with the smaller first).
[[nodiscard]] constexpr std::uint64_t edge_key(Id a, Id b) noexcept {
  const auto lo = static_cast<std::uint64_t>(a < b ? a : b);
  const auto hi = static_cast<std::uint64_t>(a < b ? b : a);
  return (lo << 32U) | (hi & 0xffffffffU);
}

// ---- definitions: the low-level edits (model.js, same names)
Id v_add(Def& d, const Vec3& p);
[[nodiscard]] std::optional<Id> find_vert(const Def& d, const Vec3& p, double tol = kEps);
/// eAdd: the edge between a and b (made when missing); nothing when a == b.
std::optional<Id> e_add(Def& d, Id a, Id b, bool soft = false, std::string_view curve = {}, bool hidden = false);
void e_remove(Def& d, Id id);
/// fAdd: the face and its edges; `id` reuses an id.
Id f_add(Def& d, std::vector<Loop> loops, std::string_view color = {}, std::optional<Id> id = std::nullopt,
         bool hidden = false);
[[nodiscard]] std::vector<std::vector<Vec3>> f_pts(const Def& d, const Face& f);
[[nodiscard]] Vec3 face_normal(const Def& d, const Face& f);
[[nodiscard]] Plane face_plane(const Def& d, const Face& f);
[[nodiscard]] double face_area(const Def& d, const Face& f);
/// edgeFaces: edge key → the faces using it, in face order.
[[nodiscard]] std::unordered_map<std::uint64_t, std::vector<Id>> edge_faces(const Def& d);
/// vertEdges: vertex → the edges at it, in edge order.
[[nodiscard]] std::unordered_map<Id, std::vector<Id>> vert_edges(const Def& d);
/// faceTris: triangles as vertex-id triples, counter-clockwise from the front.
[[nodiscard]] std::vector<std::array<Id, 3>> face_tris(const Def& d, const Face& f, Kernel k = Kernel::modelcraft);

void split_edge(Def& d, Id eid, Id v);
/// The vertex at p: an existing one, or a new one (splitting the edge it lands on).
Id vertex_at(Def& d, const Vec3& p);
struct AddEdgeOpts {
  bool faces = true;
  std::optional<Vec3> hint;
};
/// Draw an edge from p to q: splits what it meets, re-faces the planes it lies on. → the edges along p–q.
std::vector<Id> add_edge(Def& d, const Vec3& p, const Vec3& q, const AddEdgeOpts& o = {});
/// Rebuild the faces of one plane from its edge arrangement.
bool reface(Def& d, const Plane& plane, const OrderedSet<Id>& new_edges = {}, std::optional<Vec3> hint = std::nullopt);
void prune_splines(Def& d);
/// Merge coincident vertices, drop zero-length edges and degenerate faces, delete loose vertices.
bool cleanup(Def& d);
void remove_loose_verts(Def& d);
void heal_collinear(Def& d, std::span<const Id> vids);
int soften_edges(Def& d, std::span<const Id> eids, double deg);
/// smoothNormals: "fid|v" → the unit normal at that corner.
[[nodiscard]] std::map<std::pair<Id, Id>, Vec3> smooth_normals(const Def& d);
void merge_coplanar(Def& d, std::span<const Id> eids);
[[nodiscard]] double depth_behind(const Def& d, Id fid);

struct PushPullResult {
  bool ok = false;
  std::string mode;  // 'move' | 'extrude'
  double distance = 0;
  std::optional<Id> face;
};
PushPullResult push_pull(Def& d, Id fid, double distance);

/// A selection item: {t: 'f' | 'e' | 'i' | 'g', id}.
struct Item {
  char t = 'f';
  Id id = 0;
  friend bool operator==(const Item&, const Item&) = default;
};
using Sel = std::vector<Item>;

void erase_edges(Def& d, std::span<const Id> eids);
[[nodiscard]] OrderedSet<Id> sel_verts(const Def& d, const Sel& sel);
void transform_sel(Def& d, const Sel& sel, const Mat4& m);
void transform_verts(Def& d, const OrderedSet<Id>& vs, const Mat4& m);
struct StretchOpts {
  const OrderedSet<Id>* only = nullptr;
  std::function<std::optional<Box>(const std::string&)> box_of;
};
base::Result<std::size_t> stretch(Def& d, const Vec3& dir, double cut, double delta, const StretchOpts& o = {});
Sel copy_sel(Def& d, const Sel& sel);
void erase_sel(Def& d, const Sel& sel);
void reverse_faces(Def& d, std::span<const Id> fids);
[[nodiscard]] Sel connected(const Def& d, const Sel& sel);
[[nodiscard]] OrderedSet<Id> connected_verts(const Def& d, Id v);
[[nodiscard]] Sel face_with_edges(const Def& d, Id fid);

std::vector<Id> add_polygon(Def& d, std::span<const Vec3> pts, std::optional<Vec3> hint = std::nullopt);
[[nodiscard]] std::vector<Vec3> rect_pts(const Vec3& p, const Vec3& u, const Vec3& v, double w, double h);
[[nodiscard]] std::vector<Vec3> ngon_pts(const Vec3& c, const Vec3& n, const Vec3& u, double r, int sides);

// ---- curves (0.4 M3, M27)
[[nodiscard]] bool is_curve_id(std::string_view s);
/// curveRemapper: gives copied edges fresh curve ids in d.
class CurveRemapper {
 public:
  explicit CurveRemapper(Def& d) : d_(&d) {}
  std::string operator()(const std::string& c);

 private:
  Def* d_;
  std::map<std::string, std::string> m_;
};
[[nodiscard]] std::vector<Vec3> arc_pts(const Vec3& c, const Vec3& n, const Vec3& u, double r, double sweep, int segs);
std::string tag_curve(Def& d, std::span<const Vec3> pts, std::string_view kind, bool closed = false);
[[nodiscard]] std::vector<Id> curve_edges(const Def& d, std::string_view cid);
struct CurveInfo {
  std::string id, kind;
  std::vector<Id> edges, verts;
  bool closed = false;
  double length = 0;
  std::size_t segs = 0;
  bool round = false;
  std::optional<Vec3> center, normal;
  double radius = 0;
  bool flat = false;
  double sweep = 0;
};
[[nodiscard]] std::optional<CurveInfo> curve_info(const Def& d, std::string_view cid);
base::Result<CurveInfo> set_curve_radius(Def& d, std::string_view cid, double r);
inline constexpr int kSplineSegs = 8;
[[nodiscard]] std::vector<Vec3> smooth_pts(std::span<const Vec3> P, bool closed = false, int segs = kSplineSegs);
base::Result<std::string> add_spline(Def& d, std::span<const Vec3> pts, bool closed = false, int segs = kSplineSegs,
                                     std::optional<Vec3> hint = std::nullopt);
base::Result<std::string> edit_spline(Def& d, const std::string& cid, std::span<const Vec3> pts,
                                      std::optional<bool> closed = std::nullopt,
                                      std::optional<int> segs = std::nullopt);

// ---- hide (M17), flip (M15), follow me (M10)
void hide_sel(Def& d, const Sel& sel, bool on = true);
int isolate_sel(Def& d, const Sel& sel);
[[nodiscard]] Mat4 mirror_matrix(const Vec3& c, const Vec3& n);
Sel flip_sel(Def& d, const Sel& sel, const Vec3& c, const Vec3& n, bool copy = false);
struct Chain {
  std::vector<Id> verts;
  bool closed = false;
};
base::Result<Chain> edge_chain(const Def& d, std::span<const Id> eids);
[[nodiscard]] std::vector<Id> chain_through(const Def& d, Id eid);
struct FollowResult {
  std::vector<Id> faces;
  bool closed = false;
};
base::Result<FollowResult> follow_me(Def& d, Id fid, std::span<const Id> path_eids);

// ---- paint (M8, RC.7, RC.11 b)
[[nodiscard]] bool is_color(std::string_view c);
[[nodiscard]] bool is_material_paint(std::string_view c);
[[nodiscard]] bool is_image_paint(std::string_view c);
[[nodiscard]] bool is_paint(std::string_view c);
/// The colour paint shows ('#rrggbb'): a colour itself; a material's colour from setMaterialColors; an image's own.
[[nodiscard]] std::string paint_hex(std::string_view c);
void set_material_colors(std::function<std::optional<std::string>(std::string_view)> fn);
[[nodiscard]] std::string image_id(std::string_view c);
[[nodiscard]] std::string to_base64(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string image_paint(std::string_view id, std::span<const double, 4> U, std::span<const double, 4> V,
                                      std::string_view avg = {});
[[nodiscard]] Vec2 image_uv(std::string_view c, const Vec3& p);
[[nodiscard]] std::optional<std::string> image_paint_fit(std::string_view id, std::span<const Vec3> pts,
                                                         std::span<const Vec2> uvs, const Vec3& n,
                                                         std::string_view avg = {});
base::Result<void> paint_faces(Def& d, std::span<const Id> fids, std::optional<std::string_view> c);
base::Result<void> paint_instance(Def& d, Id iid, std::optional<std::string_view> c);

// ---- offset (M4)
[[nodiscard]] std::optional<std::vector<Vec2>> offset2(std::span<const Vec2> P, double d, bool closed);
base::Result<std::vector<std::vector<Vec3>>> offset_face_pts(const Def& d, Id fid, double dist);
base::Result<std::optional<Id>> offset_face(Def& d, Id fid, double dist);
struct OffsetChain {
  std::vector<Vec3> pts;
  bool closed = false;
  std::vector<Id> chain;
};
base::Result<OffsetChain> offset_chain_pts(const Def& d, std::span<const Id> eids, double dist, const Vec3& n);

// ---- solids (M21)
struct SolidProblems {
  std::vector<Id> holes, extra, inner, stray, flipped;
};
[[nodiscard]] SolidProblems solid_problems(const Def& d);
struct FixSolid {
  std::size_t stray = 0, inner = 0, flipped = 0, holes = 0;
};
FixSolid fix_solid(Def& d);
struct SolidInfo {
  bool closed = false;
  std::optional<double> volume;
  double area = 0;
  std::size_t faces = 0, edges = 0, verts = 0;
};
[[nodiscard]] SolidInfo solid_info(const Def& d, Kernel k = Kernel::modelcraft);

// ---- the model

struct Guide {
  std::string kind;  // 'line' | 'point'
  Vec3 p{}, d{};
  friend bool operator==(const Guide&, const Guide&) = default;
};
/// A dimension end: its last known world point, and the vertex it sits on ({path, v}) when it is on one.
struct DimEnd {
  Vec3 p{};
  std::optional<std::vector<Id>> path;
  Id v = 0;
  friend bool operator==(const DimEnd&, const DimEnd&) = default;
};
/// The curve a radial dimension measures: its last known world centre, radius and normal, and {path, curve}.
struct DimCurve {
  Vec3 p{};
  double r = 0;
  Vec3 n{0, 0, 1};
  std::optional<std::vector<Id>> path;
  std::string curve;
  friend bool operator==(const DimCurve&, const DimCurve&) = default;
};
struct Dim {
  std::string kind;  // '' linear | 'rad' | 'dia' | 'ang'
  DimEnd a, b, o;
  DimCurve c;
  Vec3 off{};
  std::string move = "b";  // linear: 'a' | 'b' | 'both'
  friend bool operator==(const Dim&, const Dim&) = default;
};
struct Section {
  Vec3 p{}, n{};
  std::string name;
  friend bool operator==(const Section&, const Section&) = default;
};
struct Light {
  std::string kind;  // point | spot | area
  Vec3 p{}, dir{0, 0, -1};
  std::string color = "#ffe7c4";
  double lumens = 800, angle = 60;
  std::array<double, 2> size{0.06, 0.06};
  bool on = true;
  std::string name;
  friend bool operator==(const Light&, const Light&) = default;
};
struct Storey {
  std::string name;
  double elevation = 0;
  friend bool operator==(const Storey&, const Storey&) = default;
};
struct TagDef {
  std::string name;
  bool hidden = false;
  friend bool operator==(const TagDef&, const TagDef&) = default;
};
struct Image {
  std::string name, mime, data, avg;
  friend bool operator==(const Image&, const Image&) = default;
};

/// What `walk` hands its callback: the definition, its world matrix, the instance path and the nearest placement
/// colour above it ('' none). Return false to skip what is inside.
using WalkFn = std::function<bool(const Def&, const Mat4&, const std::vector<Id>&, const std::string&)>;

/// An undo snapshot (Snap): the model's head and each definition, as the JS's JSON strings compare them.
struct Snap {
  std::string head;
  std::vector<std::pair<std::string, std::string>> parts;  // def id → its JSON
  friend bool operator==(const Snap&, const Snap&) = default;
};
struct Step {
  std::string label;
  Snap b, a;  // before, after
};

class Model {
 public:
  Model();
  Model(const Model& o);
  Model& operator=(const Model& o);
  Model(Model&&) noexcept;
  Model& operator=(Model&&) noexcept;
  ~Model();

  void reset();
  [[nodiscard]] Def& root() { return *def_mut("root"); }
  [[nodiscard]] const Def& root() const { return *def("root"); }
  [[nodiscard]] const Def* def(std::string_view id) const;
  [[nodiscard]] Def* def_mut(std::string_view id);
  [[nodiscard]] std::vector<std::string> def_ids() const;
  [[nodiscard]] std::size_t def_count() const noexcept;
  Def& new_def(std::string_view kind, std::string_view name = {});
  [[nodiscard]] std::string unique_name(std::string_view base) const;
  Id new_guide_id() { return next_id++; }

  // ---- what a placement shows and where
  void walk(const WalkFn& cb, const Def* from = nullptr, const Mat4& m = ident(), const std::vector<Id>& path = {},
            int depth = 0, const std::string& color = {}) const;
  /// walk() without hidden placements and placements on hidden tags.
  void walk_shown(const WalkFn& cb) const;
  /// The old CadCraft callback (definition and world matrix).
  void walk(const std::function<void(const Def&, const Mat4&)>& cb) const;
  struct Context {
    Def* def = nullptr;
    Mat4 m = ident(), inv = ident();
  };
  [[nodiscard]] std::optional<Context> context(std::span<const Id> path);
  [[nodiscard]] std::optional<Box> bbox(const Def* d = nullptr, const Mat4& m = ident()) const;
  [[nodiscard]] std::map<std::string, std::size_t> instance_counts() const;
  [[nodiscard]] std::string path_color(std::span<const Id> path) const;
  [[nodiscard]] bool empty() const;

  // ---- groups and components
  std::optional<Id> make_group(Def& d, const Sel& sel, std::string_view kind = "group", std::string_view name = {});
  Sel explode(Def& d, Id iid);
  std::optional<std::string> make_unique(Def& d, Id iid);
  void purge();
  Id place(Def& in, const std::string& def_id, const Mat4& m);
  base::Result<Id> move_into(std::span<const Id> from, Id iid, std::span<const Id> to);

  // ---- clipboard
  /// copyClip: not const (it reads the model as its file form, which moves dimension ends where they are now).
  [[nodiscard]] std::optional<base::Value> copy_clip(std::span<const Id> path, const Sel& sel);
  base::Result<Sel> paste_clip(std::span<const Id> path, const base::Value& clip, std::span<const double> m);

  // ---- guides, dimensions
  Id add_guide_line(const Vec3& p, const Vec3& d);
  Id add_guide_point(const Vec3& p);
  [[nodiscard]] std::optional<DimEnd> anchor_at(const Vec3& p, std::span<const Id> prefer = {}) const;
  Id add_dim(const Vec3& a, const Vec3& b, const Vec3& off, std::span<const Id> prefer = {});
  Vec3 end_at(DimEnd& e);
  base::Result<Id> add_radial_dim(std::span<const Id> path, const std::string& cid, const Vec3& off,
                                  std::string_view kind = "rad");
  Id add_angle_dim(const Vec3& o, const Vec3& a, const Vec3& b, const Vec3& off, std::span<const Id> prefer = {});
  struct Radial {
    Vec3 c{};
    double r = 0;
    Vec3 n{};
    bool live = false, flat = false, arc = false;
  };
  std::optional<Radial> radial_at(DimCurve& c);
  base::Result<double> set_dim_length(Id id, double L);
  base::Result<double> resize_between(const DimEnd* A, const DimEnd* B, const Vec3& pa, const Vec3& pb, double L,
                                      std::string_view move = "b");
  base::Result<void> move_end(const DimEnd* E, const DimEnd* F, const Vec3& d, double delta);
  base::Result<double> set_edge_length(std::span<const Id> path, Id eid, double L);
  base::Result<void> set_def_size(const std::string& def_id, int axis, double size);

  // ---- tags (M6), sections (M7), axes (M20), lights (RC.11 d), scenes (M23)
  [[nodiscard]] const TagDef* tag(std::string_view name) const;
  std::string add_tag(std::string_view name = {});
  base::Result<std::string> rename_tag(std::string_view old, std::string_view name);
  int delete_tag(std::string_view name);
  void set_tag_hidden(std::string_view name, bool hidden);
  base::Result<void> set_tag(Def& d, std::span<const Id> iids, std::optional<std::string_view> name) const;
  [[nodiscard]] std::vector<std::pair<std::string, std::size_t>> tag_counts() const;
  void ensure_tags();
  Id add_section(const Vec3& p, const Vec3& n, std::string_view name = {});
  void delete_section(Id id);
  base::Result<void> set_axes(const Vec3& o, const Vec3& x, const Vec3& y);
  [[nodiscard]] Mat4 axes_frame(std::span<const Id> path = {});
  base::Result<Id> add_light(const base::Value& o);
  base::Result<bool> set_light(Id id, const base::Value& patch);
  bool delete_light(Id id) { return lights.erase(id); }
  [[nodiscard]] base::Value capture_vis() const;
  void apply_vis(const base::Value& vis);

  // ---- undo
  /// Run fn as one undo step labelled `label`; an error restores the model as it was.
  template <class F>
  auto change(std::string_view label, F&& fn) -> decltype(fn(*this));
  [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
  [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
  std::optional<std::string> undo();
  std::optional<std::string> redo();
  [[nodiscard]] std::vector<std::string> undo_labels() const;
  [[nodiscard]] std::vector<std::string> redo_labels() const;
  /// Live previews: begin → preview(fn)… → commit(label) | abort().
  void begin();
  [[nodiscard]] bool previewing() const noexcept { return preview_.has_value(); }
  template <class F>
  bool preview(F&& fn);
  void commit(std::string_view label);
  void abort();
  /// amendLast: do the newest step again from its own start; false (nothing changed) when it is not the newest.
  template <class F>
  bool amend_last(F&& fn);

  [[nodiscard]] Snap snap();
  void restore(const Snap& s);

  // ---- files
  /// toJSON(noDefs): the model in its file form. Not const: it moves dimension ends to where they are now, as the JS.
  [[nodiscard]] base::Value to_json(bool no_defs = false);
  [[nodiscard]] base::Value to_json() const;  // the old API: a copy's
  /// load(j): a parsed native file (untrusted). `keep_history` keeps undo; `made`: the definitions already made.
  base::Result<void> load(const base::Value& j, bool keep_history = false);

  // ---- images (RC.11 b)
  base::Result<std::string> add_image(std::string_view name, std::string_view mime, std::string_view data,
                                      std::string_view avg = {});
  [[nodiscard]] base::Value images_json() const;
  void set_images(const base::Value& list);

  std::string units = "ft-in";
  std::string uid;  // new on reset and on a load that does not keep the history (the clipboard's `source`)
  Id next_id = 1;
  std::uint64_t version = 0;
  bool dirty = false;
  base::Value saved_view;  // CadCraft's 3D tab: the view a file brought, or null

  IdMap<Guide> guides;
  IdMap<Light> lights;
  IdMap<Dim> dims;
  IdMap<Section> sections;
  std::optional<Id> active_section;
  bool view_planes = true, view_cuts = true;  // sectionView
  std::vector<Storey> storeys;
  std::optional<Mat4> axes_m;
  base::Value scenes;  // [cleanScene(…)…]
  std::vector<TagDef> tags;
  OrderedMap<std::string, Image> images;

 private:
  std::vector<std::pair<std::string, std::unique_ptr<Def>>> defs_;
  std::unordered_map<std::string, std::size_t> index_;
  std::vector<Step> undo_, redo_;
  std::optional<Snap> preview_;
  int editing_ = 0;
  static constexpr std::uint64_t kMaxWalk = 250000;
  mutable std::uint64_t walk_budget_ = kMaxWalk;
  void reindex();
  void erase_def(const std::string& id);
  void set_def(std::unique_ptr<Def> d);
  void push_step(std::string label, Snap before, Snap after);
  base::Result<void> load_impl(const base::Value& j, bool keep_history,
                               std::vector<std::pair<std::string, std::unique_ptr<Def>>>* made);
  friend struct ModelAccess;
};

/// meshDef(model, polys, name, kind, segs): a definition from imported polygons (a poly = its outer points, or
/// loops + colour), vertices welded at 1e-5 m; non-flat polygons as fans; coplanar neighbours' edges soft.
struct MeshPoly {
  std::vector<std::vector<Vec3>> loops;  // outer, then holes
  std::string color;
  bool object = false;  // given as {loops, color} (holes count only then)
};
Def& mesh_def(Model& model, std::span<const MeshPoly> polys, std::string_view name, std::string_view kind = "group",
              std::span<const std::pair<Vec3, Vec3>> segs = {}, Kernel k = Kernel::modelcraft);
/// The old API: plain outer loops.
Def& mesh_def(Model& model, const std::vector<std::vector<Vec3>>& polys, std::string_view name);
Def& edges_def(Model& model, std::span<const std::pair<Vec3, Vec3>> segs, std::string_view name);
struct WorldTri {
  std::array<Vec3, 3> p;
  Vec3 n{};
  std::string color;
};
[[nodiscard]] std::vector<WorldTri> world_triangles(const Model& model, const Def* from = nullptr,
                                                    const Mat4& m0 = ident());

/// defToJSON / defOf / defFromJSON (the native form of one definition).
[[nodiscard]] base::Value def_to_json(const Def& d);
base::Result<std::unique_ptr<Def>> def_of(const base::Value& dj);
base::Result<void> def_from_json(Def& d, const base::Value& j);
void copy_own_geometry(const Def& src, Def& dst);
[[nodiscard]] std::string clean_text(const base::Value& s, std::size_t max = 200);
[[nodiscard]] std::string clean_text(std::string_view s, std::size_t max = 200);
[[nodiscard]] base::Value clean_scene(const base::Value& s);
[[nodiscard]] std::vector<Storey> clean_storeys(const base::Value& list);
[[nodiscard]] std::optional<Light> clean_light(const base::Value& o);
[[nodiscard]] base::Value light_json(Id id, const Light& L);

inline constexpr std::string_view kFormat = "modelcraft";
inline constexpr int kVersion = 1;
inline constexpr std::size_t kMaxUndo = 200;

// ---- change / preview templates

template <class F>
auto Model::change(std::string_view label, F&& fn) -> decltype(fn(*this)) {
  using R = decltype(fn(*this));
  Snap before = snap();
  ++editing_;
  const auto finish = [&]() {
    ++version;
    Snap after = snap();
    if (!(after == before)) {
      push_step(std::string(label), std::move(before), std::move(after));
    }
  };
  try {
    if constexpr (std::is_void_v<R>) {
      std::forward<F>(fn)(*this);
      --editing_;
      finish();
    } else {
      R r = std::forward<F>(fn)(*this);
      --editing_;
      if constexpr (requires { r.has_value(); }) {
        if (!r.has_value()) {
          restore(before);
          return r;
        }
      }
      finish();
      return r;
    }
  } catch (...) {  // the edit guard: the model as it was, then the caller's guard reports
    --editing_;
    restore(before);
    throw;
  }
}

template <class F>
bool Model::preview(F&& fn) {
  if (!preview_) {
    return false;
  }
  restore(must(preview_));
  ++editing_;
  bool ok = true;
  try {
    auto r = std::forward<F>(fn)(*this);
    if constexpr (requires { r.has_value(); }) {
      ok = r.has_value();
    }
  } catch (...) {
    ok = false;
  }
  --editing_;
  if (!ok) {
    restore(must(preview_));
  }
  ++version;
  return ok;
}

template <class F>
bool Model::amend_last(F&& fn) {
  if (undo_.empty() || preview_) {
    return false;
  }
  restore(undo_.back().b);
  ++editing_;
  bool ok = true;
  try {
    auto r = std::forward<F>(fn)(*this);
    if constexpr (requires { r.has_value(); }) {
      ok = r.has_value();
    }
  } catch (...) {
    ok = false;
  }
  --editing_;
  if (!ok) {
    restore(undo_.back().a);
    ++version;
    return false;
  }
  ++version;
  undo_.back().a = snap();
  redo_.clear();
  dirty = true;
  return true;
}

/// intersectFaces (0.4 M19): the selection's faces cut against `others` ('model' | 'selection'); → edges gained.
std::size_t intersect_faces(Model& model, std::span<const Id> path, const Sel& sel, std::string_view others = "model");

}  // namespace cc::m3d
