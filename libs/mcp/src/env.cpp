// SPDX-License-Identifier: Apache-2.0
// tools/cli/headless/paths.mjs and tools.mjs (createEnv, readFile, keep, get, write, files_list, doc_close), ported.
#include "cc/mcp/env.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <system_error>

#include "cc/base/file.hpp"
#include "cc/base/js.hpp"

namespace cc::mcp {

namespace fs = std::filesystem;
using base::Value;

namespace {

#if defined(_WIN32)
constexpr char kSep = '\\';
#else
constexpr char kSep = '/';
#endif

std::string home() {
  if (auto h = base::env_var("HOME"); h && !h->empty()) {
    return *h;
  }
  if (auto h = base::env_var("USERPROFILE"); h && !h->empty()) {
    return *h;
  }
  return "/";
}

/// path.resolve: absolute, `.` and `..` gone, no trailing separator (except the root).
std::string resolve(const fs::path& p) {
  std::error_code ec;
  fs::path a = p.is_absolute() ? p : fs::current_path(ec) / p;
  a = a.lexically_normal();
  std::string s = a.string();
  while (s.size() > 1 && s.back() == kSep && (s.size() != 3 || s[1] != ':')) {
    s.pop_back();
  }
  return s;
}

std::string expand(std::string_view p) {
  if (p == "~") {
    return home();
  }
  if (p.starts_with("~/") || p.starts_with("~\\")) {
    return (fs::path(home()) / std::string(p.substr(2))).string();
  }
  return std::string(p);
}

bool inside(std::string_view child, std::string_view parent) {
  if (child == parent) {
    return true;
  }
  std::string pre(parent);
  if (pre.empty() || pre.back() != kSep) {
    pre += kSep;
  }
  return child.starts_with(pre);
}

std::optional<std::string> real_path(const std::string& p) {
  std::error_code ec;
  const fs::path r = fs::canonical(p, ec);
  if (ec) {
    return std::nullopt;
  }
  return r.string();
}

std::string join_list(const std::vector<std::string>& l) {
  std::string s;
  for (std::size_t i = 0; i < l.size(); ++i) {
    s += (i != 0 ? ", " : "") + l[i];
  }
  return s;
}

PathError err(std::string message, std::string code = "outside") {
  return PathError{std::move(message), std::move(code)};
}

std::string iso_mtime(const fs::path& p) {
  std::error_code ec;
  const auto t = fs::last_write_time(p, ec);
  if (ec) {
    return {};
  }
  const auto sys = std::chrono::file_clock::to_sys(t);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
  return iso_time(static_cast<double>(ms));
}

}  // namespace

PathResult<Roots> Roots::make(const std::vector<std::string>& list) {
  Roots r;
  for (const std::string& item : list) {
    const std::string abs = resolve(expand(item));
    const auto real = real_path(abs);
    if (!real) {
      return std::unexpected(err("The folder " + abs + " does not exist.", "missing"));
    }
    std::error_code ec;
    if (!fs::is_directory(*real, ec)) {
      return std::unexpected(err(abs + " is not a folder.", "missing"));
    }
    if (std::ranges::find(r.list_, *real) == r.list_.end()) {
      r.list_.push_back(*real);
    }
    if (std::ranges::find(r.raw_, abs) == r.raw_.end()) {
      r.raw_.push_back(abs);
    }
  }
  return r;
}

std::optional<std::string> Roots::root_of(std::string_view real) const {
  for (const std::string& r : list_) {
    if (inside(real, r)) {
      return r;
    }
  }
  return std::nullopt;
}

PathResult<std::string> Roots::abs(std::string_view p) const {
  if (base::js_trim(p).empty()) {
    return std::unexpected(err("A file path is needed.", "invalid"));
  }
  if (p.contains('\0')) {
    return std::unexpected(err("That path is not allowed.", "invalid"));
  }
  if (p.size() >= 4 && p[0] == '\\' && p[1] == '\\' && (p[2] == '.' || p[2] == '?') && p[3] == '\\') {
    return std::unexpected(err("Device paths are not allowed.", "invalid"));
  }
  if (!any()) {
    return std::unexpected(err("No folders are open to the assistant. Start it with --root <folder>.", "noroots"));
  }
  const std::string e = expand(p);
  const fs::path ep(e);
  return ep.is_absolute() ? resolve(ep) : resolve(fs::path(list_[0]) / ep);
}

std::string Roots::outside(const std::string& shown) const {
  return shown + " is outside the folders the assistant may use (" + join_list(list_) + ").";
}

PathResult<std::string> Roots::lexical(const std::string& a) const {
  const bool ok = std::ranges::any_of(list_, [&a](const std::string& r) { return inside(a, r); }) ||
                  std::ranges::any_of(raw_, [&a](const std::string& r) { return inside(a, r); });
  if (!ok) {
    return std::unexpected(err(outside(a)));
  }
  return a;
}

PathResult<std::string> Roots::check(const std::string& real, const std::string& shown) const {
  if (!root_of(real)) {
    return std::unexpected(err(outside(shown)));
  }
  return real;
}

PathResult<std::string> Roots::read(std::string_view p) const {
  auto a0 = abs(p);
  if (!a0) {
    return a0;
  }
  auto a = lexical(*a0);
  if (!a) {
    return a;
  }
  const auto real = real_path(*a);
  if (!real) {
    return std::unexpected(err("There is no file " + *a + ".", "missing"));
  }
  if (auto c = check(*real, *a); !c) {
    return c;
  }
  std::error_code ec;
  const auto st = fs::status(*real, ec);
  if (ec) {
    return std::unexpected(err("There is no file " + *a + ".", "missing"));
  }
  if (!fs::is_regular_file(st)) {
    return std::unexpected(err(*a + " is not a file.", "invalid"));
  }
  return *real;
}

PathResult<std::string> Roots::folder(std::string_view p) const {
  if (!any()) {
    return std::unexpected(err("No folders are open to the assistant. Start it with --root <folder>.", "noroots"));
  }
  std::string a = list_[0];
  if (!p.empty()) {
    auto a0 = abs(p);
    if (!a0) {
      return a0;
    }
    auto l = lexical(*a0);
    if (!l) {
      return l;
    }
    a = *l;
  }
  const auto real = real_path(a);
  if (!real) {
    return std::unexpected(err("There is no folder " + a + ".", "missing"));
  }
  if (auto c = check(*real, a); !c) {
    return c;
  }
  std::error_code ec;
  if (!fs::is_directory(*real, ec)) {
    return std::unexpected(err(a + " is not a folder.", "invalid"));
  }
  return *real;
}

PathResult<std::string> Roots::write(std::string_view p, bool overwrite, const std::set<std::string>& mine) const {
  auto a0 = abs(p);
  if (!a0) {
    return a0;
  }
  auto a = lexical(*a0);
  if (!a) {
    return a;
  }
  const fs::path ap(*a);
  const std::string dir = ap.parent_path().string();
  const auto real_dir = real_path(dir);
  if (!real_dir) {
    return std::unexpected(err("The folder " + dir + " does not exist.", "missing"));
  }
  std::string real = (fs::path(*real_dir) / ap.filename()).string();
  if (auto c = check(real, *a); !c) {
    return c;
  }
  std::error_code ec;
  const auto st = fs::symlink_status(real, ec);
  if (!ec && st.type() != fs::file_type::not_found) {
    if (fs::is_symlink(st)) {
      return std::unexpected(err(*a + " is a link; write to the file it points to instead.", "invalid"));
    }
    if (!fs::is_regular_file(st)) {
      return std::unexpected(err(*a + " is not a file.", "invalid"));
    }
    if (!overwrite && !mine.contains(real)) {
      return std::unexpected(err(*a + " already exists. Pass overwrite: true to replace it.", "exists"));
    }
  }
  return real;
}

// ---------------------------------------------------------------------------------------------------- Env
std::string ext_of(std::string_view p) {
  const std::string name = fs::path(std::string(p)).filename().string();
  const std::size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0) {
    return {};
  }
  return base::js_to_lower(name.substr(dot + 1));
}

std::expected<ReadFile, ToolError> read_file(Env& env, std::string_view p) {
  auto real = env.roots.read(p);
  if (!real) {
    return std::unexpected(ToolError{real.error().message});
  }
  std::error_code ec;
  const auto size = fs::file_size(*real, ec);
  if (ec) {
    return std::unexpected(ToolError{"There is no file " + *real + "."});
  }
  if (size > kMaxFile) {
    return std::unexpected(
        ToolError{fs::path(*real).filename().string() + " is " +
                  std::to_string(static_cast<long long>(std::round(static_cast<double>(size) / 1048576.0))) +
                  " MB; the limit is 500 MB."});
  }
  auto bytes = base::read_file(*real, kMaxFile);
  if (!bytes) {
    return std::unexpected(ToolError{bytes.error().message});
  }
  return ReadFile{*real, std::move(*bytes)};
}

std::expected<std::string, ToolError> keep(Env& env, std::string kind, std::shared_ptr<DocSession> s,
                                           std::optional<std::string> real) {
  if (env.docs.size() >= kMaxOpen) {
    return std::unexpected(ToolError{"16 files are open. Close one with doc_close first."});
  }
  std::string id = "d" + std::to_string(++env.seq);
  if (real) {
    env.mine.insert(*real);
  }
  env.docs[id] = OpenDoc{std::move(kind), std::move(s), std::move(real)};
  return id;
}

namespace {
std::string_view kind_words(std::string_view kind) {
  if (kind == "cad") {
    return "a drawing";
  }
  if (kind == "ifc") {
    return "an IFC model";
  }
  if (kind == "takeoff") {
    return "a takeoff";
  }
  return "undefined";
}
}  // namespace

std::expected<OpenDoc*, ToolError> get(Env& env, std::string_view id, std::string_view kind) {
  const auto it = env.docs.find(std::string(id));
  if (it == env.docs.end()) {
    std::string_view open = "takeoff_open";
    if (kind == "cad") {
      open = "drawing_open";
    }
    if (kind == "ifc") {
      open = "ifc_open";
    }
    return std::unexpected(
        ToolError{"No open file " + std::string(id) + ". Open one first (" + std::string(open) + ")."});
  }
  if (!kind.empty() && it->second.kind != kind) {
    return std::unexpected(ToolError{std::string(id) + " is " + std::string(kind_words(it->second.kind)) + ", not " +
                                     std::string(kind_words(kind)) + "."});
  }
  return &it->second;
}

std::expected<std::string, ToolError> write_file(Env& env, std::string_view p, std::span<const std::byte> bytes,
                                                 bool overwrite, std::string_view ext) {
  auto real = env.roots.write(p, overwrite, env.mine);
  if (!real) {
    return std::unexpected(ToolError{real.error().message});
  }
  if (!ext.empty() && ext_of(*real) != ext) {
    return std::unexpected(ToolError{"The file name must end in ." + std::string(ext) + "."});
  }
  if (auto ok = base::write_file_atomic(*real, bytes); !ok) {
    return std::unexpected(ToolError{ok.error().message});
  }
  env.mine.insert(*real);
  return *real;
}

// ---------------------------------------------------------------------------------------------------- schemas
Value schema_doc() {
  Value v = Value::object();
  v.append("type", "string");
  v.append("pattern", "^d[0-9]{1,6}$");
  v.append("description", "A handle from drawing_open, ifc_open or takeoff_open.");
  return v;
}
Value schema_path() {
  Value v = Value::object();
  v.append("type", "string");
  v.append("minLength", 1);
  v.append("maxLength", 4096);
  v.append("description", "A file inside the folders the assistant may use (relative = inside the first one).");
  return v;
}
Value schema_offset() {
  Value v = Value::object();
  v.append("type", "integer");
  v.append("minimum", 0);
  v.append("default", 0);
  return v;
}
Value schema_overwrite() {
  Value v = Value::object();
  v.append("type", "boolean");
  v.append("default", false);
  v.append("description", "Replace a file that already exists (and was not opened or written in this session).");
  return v;
}
Value schema_object(Value properties, std::vector<std::string> required) {
  Value v = Value::object();
  v.append("type", "object");
  v.append("properties", std::move(properties));
  base::Array req;
  for (std::string& r : required) {
    req.emplace_back(std::move(r));
  }
  v.append("required", std::move(req));
  v.append("additionalProperties", false);
  return v;
}

// ---------------------------------------------------------------------------------------------------- tools
Tool files_list_tool(Env& env) {
  Value props = Value::object();
  Value folder = schema_path();
  folder.set("description", "A folder inside the allowed ones; default the first allowed folder.");
  props.append("folder", std::move(folder));
  Value items = Value::object();
  items.append("type", "string");
  items.append("maxLength", 12);
  Value ext_schema = Value::object();
  ext_schema.append("type", "array");
  ext_schema.append("items", std::move(items));
  ext_schema.append("maxItems", 20);
  ext_schema.append("description", R"(e.g. ["dwg", "dxf"])");
  props.append("extensions", std::move(ext_schema));
  Value rec = Value::object();
  rec.append("type", "boolean");
  rec.append("default", false);
  props.append("recursive", std::move(rec));
  Tool t;
  t.name = "files_list";
  t.group = "read";
  t.title = "List files";
  t.description =
      "Files in a folder the assistant may use, newest first (up to 2000). Hidden files and links are skipped.";
  t.input_schema = schema_object(std::move(props));
  t.run = [&env](Value& a, CallCtx&) -> ToolAnswer {
    const Value f = a.get("folder");
    auto base_dir = env.roots.folder(f.is_string() ? f.as_string() : std::string());
    if (!base_dir) {
      return std::unexpected(ToolError{base_dir.error().message});
    }
    std::optional<std::set<std::string>> exts;
    if (const Value ev = a.get("extensions"); ev.is_array() && !ev.as_array().empty()) {
      exts.emplace();
      for (const Value& x : ev.as_array()) {
        std::string s = base::js_string(x);
        if (!s.empty() && s[0] == '.') {
          s.erase(0, 1);
        }
        exts->insert("." + base::js_to_lower(s));
      }
    }
    const bool recursive = a.get("recursive").truthy();
    struct Item {
      std::string path;
      std::uintmax_t size;
      std::string modified;
    };
    std::vector<Item> out;
    bool truncated = false;
    constexpr std::size_t kMax = 2000;
    // NOLINTNEXTLINE(misc-no-recursion) at most a few levels deep (depth is counted)
    const auto walk = [&](const fs::path& dir, int depth, auto&& self) -> void {
      std::error_code ec;
      fs::directory_iterator it(dir, ec);
      if (ec) {
        return;
      }
      for (const auto& d : it) {
        if (out.size() >= kMax) {
          truncated = true;
          return;
        }
        const std::string name = d.path().filename().string();
        if (!name.empty() && name[0] == '.') {
          continue;
        }
        const auto st = d.symlink_status(ec);
        if (ec) {
          continue;
        }
        if (fs::is_directory(st)) {
          if (recursive && depth < 12) {
            self(d.path(), depth + 1, self);
          }
          continue;
        }
        if (!fs::is_regular_file(st)) {
          continue;  // links are not followed here
        }
        if (exts) {
          std::string e = d.path().extension().string();
          if (!exts->contains(base::js_to_lower(e))) {
            continue;
          }
        }
        const auto size = fs::file_size(d.path(), ec);
        if (ec) {
          continue;
        }
        out.push_back(Item{d.path().string(), size, iso_mtime(d.path())});
        if (truncated) {
          return;
        }
      }
    };
    walk(fs::path(*base_dir), 0, walk);
    std::ranges::stable_sort(out, [](const Item& x, const Item& y) { return x.modified > y.modified; });
    base::Array files;
    for (const Item& i : out) {
      Value v = Value::object();
      v.append("path", i.path);
      v.append("size", static_cast<double>(i.size));
      v.append("modified", i.modified);
      files.push_back(std::move(v));
    }
    base::Array folders;
    for (const std::string& r : env.roots.list()) {
      folders.emplace_back(r);
    }
    Value r = Value::object();
    r.append("folder", *base_dir);
    r.append("folders", std::move(folders));
    r.append("files", std::move(files));
    r.append("truncated", truncated);
    return r;
  };
  return t;
}

Tool doc_close_tool(Env& env) {
  Value props = Value::object();
  props.append("doc", schema_doc());
  Tool t;
  t.name = "doc_close";
  t.group = "read";
  t.title = "Close a file";
  t.description = "Forget an open drawing, model or takeoff (unsaved changes are dropped).";
  t.input_schema = schema_object(std::move(props), {"doc"});
  t.run = [&env](Value& a, CallCtx&) -> ToolAnswer {
    const std::string id = a.get("doc").as_string();
    if (auto d = get(env, id); !d) {
      return std::unexpected(d.error());
    }
    env.docs.erase(id);
    Value r = Value::object();
    r.append("closed", id);
    return r;
  };
  return t;
}

}  // namespace cc::mcp
