// SPDX-License-Identifier: Apache-2.0
// host.js / tools/mcp/core.mjs "mcp-core", ported. See dispatcher.hpp.
#include "cc/mcp/dispatcher.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <utility>

#include "cc/base/js.hpp"

namespace cc::mcp {

using base::Value;

namespace {

constexpr std::array<std::string_view, 4> kProtocols = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
constexpr std::array<std::string_view, 4> kGroups = {"read", "edit", "save", "export"};

/// The names an object inherits from Object.prototype: `props[k]` finds them on any schema's properties, and the
/// check then accepts the argument without looking (a function is not a schema).
constexpr std::array<std::string_view, 12> kInherited = {
    "constructor",      "toString",         "toLocaleString",       "valueOf",
    "hasOwnProperty",   "isPrototypeOf",    "propertyIsEnumerable", "__proto__",
    "__defineGetter__", "__defineSetter__", "__lookupGetter__",     "__lookupSetter__"};

bool inherited(std::string_view k) {
  return std::ranges::find(kInherited, k) != kInherited.end();
}

std::string js_type_of(const Value& v) {
  switch (v.kind()) {
    case Value::Kind::null: return "null";
    case Value::Kind::array: return "array";
    case Value::Kind::number: {
      const double d = v.as_number();
      return std::isfinite(d) && std::floor(d) == d ? "integer" : "number";
    }
    case Value::Kind::string: return "string";
    case Value::Kind::boolean: return "boolean";
    case Value::Kind::undefined: return "undefined";
    case Value::Kind::object:
    case Value::Kind::bytes: return "object";
  }
  return "object";
}

bool fits(std::string_view t, const Value& v) {
  const std::string k = js_type_of(v);
  return t == k || (t == "number" && k == "integer");
}

/// a === b for JSON values (objects and arrays are never === another).
bool strict_equal(const Value& a, const Value& b) {
  if (a.kind() != b.kind()) {
    return false;
  }
  switch (a.kind()) {
    case Value::Kind::null:
    case Value::Kind::undefined: return true;
    case Value::Kind::boolean: return a.as_bool() == b.as_bool();
    case Value::Kind::number: return a.as_number() == b.as_number();
    case Value::Kind::string: return a.as_string() == b.as_string();
    default: return false;
  }
}
/// Array#includes (SameValueZero: NaN finds NaN).
bool same_value_zero(const Value& a, const Value& b) {
  if (a.is_number() && b.is_number() && std::isnan(a.as_number()) && std::isnan(b.as_number())) {
    return true;
  }
  return strict_equal(a, b);
}

/// The first `n` UTF-16 code units of s (String#slice(0, n)).
std::string slice16(std::string_view s, std::size_t n) {
  const auto units = base::to_utf16(s);
  if (units.size() <= n) {
    return std::string(s);
  }
  std::string out;
  base::append_utf16(out, std::span(units).first(n));
  return out;
}

// NOLINTNEXTLINE(misc-no-recursion) — schemas are the tools' own, values capped by the JSON parser's depth
void check(const Value& schema, Value& value, const std::string& where, std::vector<std::string>& out) {
  if (!schema.is_object()) {
    return;
  }
  const Value* any = schema.find("anyOf");
  if (any == nullptr || !any->truthy()) {
    any = schema.find("oneOf");
  }
  if (any != nullptr && any->truthy()) {
    bool one = false;
    if (any->is_array()) {
      for (const Value& alt : any->as_array()) {
        std::vector<std::string> p;
        check(alt, value, where, p);
        if (p.empty()) {
          one = true;
          break;
        }
      }
    }
    if (!one) {
      out.push_back(where + " does not match any of the allowed shapes.");
    }
    return;
  }
  if (const Value& type = schema["type"]; type.truthy()) {
    std::vector<std::string> ts;
    if (type.is_array()) {
      for (const Value& t : type.as_array()) {
        ts.push_back(base::js_string(t));
      }
    } else {
      ts.push_back(base::js_string(type));
    }
    if (std::ranges::none_of(ts, [&value](const std::string& t) { return fits(t, value); })) {
      std::string list;
      for (std::size_t i = 0; i < ts.size(); ++i) {
        list += (i != 0 ? " or " : "") + ts[i];
      }
      out.push_back(where + " must be " + list + ".");
      return;
    }
  }
  if (const Value* c = schema.find("const"); c != nullptr && !c->is_undefined() && !strict_equal(value, *c)) {
    out.push_back(where + " must be " + stringify(*c) + ".");
  }
  if (const Value& e = schema["enum"]; e.truthy() && e.is_array()) {
    if (std::ranges::none_of(e.as_array(), [&value](const Value& x) { return same_value_zero(value, x); })) {
      std::string list;
      for (std::size_t i = 0; i < e.as_array().size(); ++i) {
        list += (i != 0 ? ", " : "") + stringify(e.as_array()[i]);
      }
      out.push_back(where + " must be one of " + list + ".");
    }
  }
  if (value.is_number()) {
    const double v = value.as_number();
    if (const Value& m = schema["minimum"]; !m.is_undefined() && v < base::js_to_number(m)) {
      out.push_back(where + " must be at least " + base::js_string(m) + ".");
    }
    if (const Value& m = schema["maximum"]; !m.is_undefined() && v > base::js_to_number(m)) {
      out.push_back(where + " must be at most " + base::js_string(m) + ".");
    }
  }
  if (value.is_string()) {
    const auto len = static_cast<double>(base::utf16_length(value.as_string()));
    if (const Value& m = schema["minLength"]; !m.is_undefined() && len < base::js_to_number(m)) {
      out.push_back(where + " must have at least " + base::js_string(m) + " characters.");
    }
    if (const Value& m = schema["maxLength"]; !m.is_undefined() && len > base::js_to_number(m)) {
      out.push_back(where + " must have at most " + base::js_string(m) + " characters.");
    }
    if (const Value& p = schema["pattern"]; p.truthy()) {
      const auto hit = pattern_matches(base::js_string(p), value.as_string());
      if (!hit || !*hit) {
        out.push_back(where + " is not in the expected form.");
      }
    }
  }
  if (value.is_array()) {
    const auto n = static_cast<double>(value.as_array().size());
    if (const Value& m = schema["minItems"]; !m.is_undefined() && n < base::js_to_number(m)) {
      out.push_back(where + " must have at least " + base::js_string(m) + " items.");
    }
    if (const Value& m = schema["maxItems"]; !m.is_undefined() && n > base::js_to_number(m)) {
      out.push_back(where + " must have at most " + base::js_string(m) + " items.");
    }
    if (const Value& items = schema["items"]; items.truthy()) {
      auto& arr = value.as_array();
      for (std::size_t i = 0; i < arr.size(); ++i) {
        check(items, arr[i], where + "[" + std::to_string(i) + "]", out);
      }
    }
  }
  if (value.is_object()) {
    static const Value kEmpty = Value::object();
    const Value& props_v = schema["properties"].truthy() ? schema["properties"] : kEmpty;
    if (const Value& req = schema["required"]; req.truthy() && req.is_array()) {
      for (const Value& r : req.as_array()) {
        const std::string k = base::js_string(r);
        const Value* got = value.find(k);
        if (got == nullptr || got->is_undefined()) {
          out.push_back(std::format("{}.{} is required.", where, k));
        }
      }
    }
    // Object.entries(value), in JS property order
    std::vector<std::string> keys;
    for (const base::Member* m : base::js_members(value.as_object())) {
      if (!m->value.is_undefined()) {
        keys.push_back(m->key);
      }
    }
    const Value& extra = schema["additionalProperties"];
    for (const std::string& k : keys) {
      Value* v = value.find(k);
      const Value* p = props_v.is_object() ? props_v.find(k) : nullptr;
      if (p != nullptr && p->truthy()) {
        check(*p, *v, std::format("{}.{}", where, k), out);
      } else if (inherited(k)) {
        // a function from Object.prototype: no schema, nothing checked
      } else if (extra.is_bool() && !extra.as_bool()) {
        out.push_back(std::format("{}.{} is not a known argument.", where, k));
      } else if (extra.truthy() && extra.is_object()) {
        check(extra, *v, std::format("{}.{}", where, k), out);
      }
    }
    if (props_v.is_object()) {
      for (const base::Member* m : base::js_members(props_v.as_object())) {
        const Value* got = value.find(m->key);
        const Value* def = m->value.is_object() ? m->value.find("default") : nullptr;
        if ((got == nullptr || got->is_undefined()) && m->value.truthy() && def != nullptr && !def->is_undefined()) {
          value.set(m->key, *def);
        }
      }
    }
  }
}

// NOLINTNEXTLINE(misc-no-recursion) — bounded by the value's depth
void write(const Value& v, int indent, int level, std::string& out) {
  const auto nl = [&](int l) {
    if (indent <= 0) {
      return;
    }
    out += '\n';
    out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(l), ' ');
  };
  switch (v.kind()) {
    case Value::Kind::undefined:
    case Value::Kind::null: out += "null"; return;
    case Value::Kind::boolean: out += v.as_bool() ? "true" : "false"; return;
    case Value::Kind::number: {
      const double d = v.as_number();
      if (!std::isfinite(d)) {
        out += "null";
      } else {
        base::append_number(out, d);
      }
      return;
    }
    case Value::Kind::string: base::append_json_string(out, v.as_string()); return;
    case Value::Kind::bytes: out += "{}"; return;
    case Value::Kind::array: {
      const auto& a = v.as_array();
      if (a.empty()) {
        out += "[]";
        return;
      }
      out += '[';
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (i != 0) {
          out += ',';
        }
        nl(level + 1);
        write(a[i], indent, level + 1, out);
      }
      nl(level);
      out += ']';
      return;
    }
    case Value::Kind::object: {
      const Value& copy = v;  // a compact point reads through its plain form
      const auto members = base::js_members(copy.as_object());
      bool any = false;
      out += '{';
      for (const base::Member* m : members) {
        if (m->value.is_undefined()) {
          continue;
        }
        if (any) {
          out += ',';
        }
        any = true;
        nl(level + 1);
        base::append_json_string(out, m->key);
        out += indent > 0 ? ": " : ":";
        write(m->value, indent, level + 1, out);
      }
      if (any) {
        nl(level);
      }
      out += '}';
      return;
    }
  }
}

/// The tool's name: ^[a-z][a-z0-9_]{0,63}$
bool good_name(std::string_view n) {
  if (n.empty() || n.size() > 64 || n[0] < 'a' || n[0] > 'z') {
    return false;
  }
  return std::ranges::all_of(n, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

}  // namespace

std::span<const std::string_view> protocols() {
  return kProtocols;
}
std::span<const std::string_view> tool_groups() {
  return kGroups;
}

Value group_hints(std::string_view group) {
  const auto hints = [](bool ro, bool destructive, bool idempotent) {
    Value h = Value::object();
    h.append("readOnlyHint", ro);
    h.append("destructiveHint", destructive);
    h.append("idempotentHint", idempotent);
    h.append("openWorldHint", false);
    return h;
  };
  if (group == "read") {
    return hints(true, false, true);
  }
  if (group == "edit") {
    return hints(false, false, false);
  }
  if (group == "save") {
    return hints(false, true, true);
  }
  if (group == "export") {
    return hints(false, false, true);
  }
  return {};
}

std::vector<std::string> check_schema(const Value& schema, Value& value, std::string_view where) {
  std::vector<std::string> out;
  check(schema, value, std::string(where), out);
  return out;
}

// NOLINTNEXTLINE(misc-no-recursion) — depth ≤ 2
Value log_args(const Value& args, int depth) {
  if (args.is_string()) {
    const std::string& s = args.as_string();
    return base::utf16_length(s) > 200 ? Value(slice16(s, 200) + "\xe2\x80\xa6") : args;
  }
  if (args.is_array()) {
    if (depth > 1) {
      return {"[" + std::to_string(args.as_array().size()) + " items]"};
    }
    base::Array out;
    const auto& a = args.as_array();
    for (std::size_t i = 0; i < a.size() && i < 20; ++i) {
      out.push_back(log_args(a[i], depth + 1));
    }
    return {std::move(out)};
  }
  if (args.is_object()) {
    if (depth > 1) {
      return {"{\xe2\x80\xa6}"};
    }
    const Value& copy = args;
    Value o = Value::object();
    std::size_t n = 0;
    for (const base::Member* m : base::js_members(copy.as_object())) {
      if (n++ >= 20) {
        break;
      }
      o.set(m->key, log_args(m->value, depth + 1));
    }
    return o;
  }
  return args;
}

std::string stringify(const Value& v, int indent) {
  std::string out;
  write(v, indent, 0, out);
  return out;
}

std::string iso_time(double ms) {
  const auto t = static_cast<std::int64_t>(std::floor(ms));
  std::int64_t days = t / 86'400'000;
  std::int64_t rem = t % 86'400'000;
  if (rem < 0) {
    rem += 86'400'000;
    --days;
  }
  // civil_from_days (H. Hinnant)
  const std::int64_t z = days + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  std::int64_t y = yoe + era * 400;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) {
    ++y;
  }
  return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", y, m, d, rem / 3'600'000, rem / 60'000 % 60,
                     rem / 1000 % 60, rem % 1000);
}

std::optional<bool> pattern_matches(std::string_view pattern, std::string_view text) {
  // the code points of the text ('u' flag)
  std::vector<std::uint32_t> cps;
  for (std::size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    std::size_t n = 4;
    if (c < 0x80) {
      n = 1;
    } else if (c < 0xe0) {
      n = 2;
    } else if (c < 0xf0) {
      n = 3;
    }
    n = std::min(n, text.size() - i);
    std::uint32_t cp = n == 1 ? c : c & (0xffU >> (n + 1));
    for (std::size_t k = 1; k < n; ++k) {
      cp = (cp << 6U) | (static_cast<unsigned char>(text[i + k]) & 0x3fU);
    }
    cps.push_back(cp);
    i += n;
  }
  struct Atom {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;  // empty with any = any character
    std::size_t min = 1;
    std::size_t max = 1;
  };
  std::vector<Atom> atoms;
  bool start = false;
  bool end = false;
  std::size_t i = 0;
  if (i < pattern.size() && pattern[i] == '^') {
    start = true;
    ++i;
  }
  while (i < pattern.size()) {
    const char c = pattern[i];
    if (c == '$' && i + 1 == pattern.size()) {
      end = true;
      ++i;
      break;
    }
    Atom a;
    if (c == '[') {
      ++i;
      if (i < pattern.size() && pattern[i] == '^') {
        return std::nullopt;  // negated classes: not in the subset
      }
      while (i < pattern.size() && pattern[i] != ']') {
        auto lo = static_cast<unsigned char>(pattern[i]);
        if (lo == '\\') {
          return std::nullopt;
        }
        std::uint32_t hi = lo;
        if (i + 2 < pattern.size() && pattern[i + 1] == '-' && pattern[i + 2] != ']') {
          hi = static_cast<unsigned char>(pattern[i + 2]);
          i += 3;
        } else {
          ++i;
        }
        a.ranges.emplace_back(lo, hi);
      }
      if (i >= pattern.size()) {
        return std::nullopt;
      }
      ++i;  // ]
    } else if (std::string_view("\\.()|*+?{}^$").contains(c)) {
      return std::nullopt;
    } else {
      a.ranges.emplace_back(static_cast<unsigned char>(c), static_cast<unsigned char>(c));
      ++i;
    }
    if (i < pattern.size()) {
      const char q = pattern[i];
      if (q == '*' || q == '+' || q == '?') {
        a.min = q == '+' ? 1 : 0;
        a.max = q == '?' ? 1 : SIZE_MAX;
        ++i;
      } else if (q == '{') {
        const std::size_t close = pattern.find('}', i);
        if (close == std::string_view::npos) {
          return std::nullopt;
        }
        const std::string_view body = pattern.substr(i + 1, close - i - 1);
        const std::size_t comma = body.find(',');
        const auto to_n = [](std::string_view s) -> std::optional<std::size_t> {
          if (s.empty() || !std::ranges::all_of(s, [](char d) { return d >= '0' && d <= '9'; })) {
            return std::nullopt;
          }
          std::size_t v = 0;
          for (const char d : s) {
            v = v * 10 + static_cast<std::size_t>(d - '0');
          }
          return v;
        };
        const auto lo = to_n(comma == std::string_view::npos ? body : body.substr(0, comma));
        const auto hi = comma == std::string_view::npos ? lo : to_n(body.substr(comma + 1));
        if (!lo || !hi || *hi < *lo) {
          return std::nullopt;
        }
        a.min = *lo;
        a.max = *hi;
        i = close + 1;
      }
    }
    atoms.push_back(std::move(a));
  }
  if (i != pattern.size()) {
    return std::nullopt;
  }
  // backtracking over atoms (greedy): the patterns are short and anchored
  const auto in = [](const Atom& a, std::uint32_t cp) {
    return std::ranges::any_of(a.ranges, [cp](const auto& r) { return cp >= r.first && cp <= r.second; });
  };
  // match(ai, pos): does atoms[ai..] match cps[pos..] (to the end when `end`)?
  std::vector<std::vector<std::int8_t>> memo(atoms.size() + 1, std::vector<std::int8_t>(cps.size() + 1, -1));
  // NOLINTNEXTLINE(misc-no-recursion) one level per atom, and patterns are the tools' own
  const auto match = [&](auto&& self, std::size_t ai, std::size_t pos) -> bool {
    if (ai == atoms.size()) {
      return !end || pos == cps.size();
    }
    std::int8_t& m = memo[ai][pos];
    if (m >= 0) {
      return m != 0;
    }
    const Atom& a = atoms[ai];
    std::size_t k = 0;
    while (k < a.max && pos + k < cps.size() && in(a, cps[pos + k])) {
      ++k;
    }
    bool ok = false;
    for (std::size_t take = k + 1; take-- > a.min;) {
      if (self(self, ai + 1, pos + take)) {
        ok = true;
        break;
      }
      if (take == 0) {
        break;
      }
    }
    if (k < a.min) {
      ok = false;
    }
    m = ok ? 1 : 0;
    return ok;
  };
  if (start) {
    return match(match, 0, 0);
  }
  for (std::size_t p = 0; p <= cps.size(); ++p) {
    for (auto& row : memo) {
      std::ranges::fill(row, -1);
    }
    if (match(match, 0, p)) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------------------------------- Dispatcher
Dispatcher::Dispatcher(Options opts) : opts_(std::move(opts)) {}

double Dispatcher::now() const {
  if (opts_.now) {
    return opts_.now();
  }
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::vector<std::string> Dispatcher::allowed() const {
  if (opts_.allowed) {
    return opts_.allowed();
  }
  return {"read"};
}

std::expected<std::uint64_t, std::string> Dispatcher::tool(Tool t) {
  if (!good_name(t.name)) {
    return std::unexpected("A tool name is lower case letters, digits and _: " + t.name);
  }
  if (std::ranges::find(kGroups, t.group) == kGroups.end()) {
    return std::unexpected("Tool " + t.name + ": group must be one of read, edit, save, export");
  }
  if (!t.run) {
    return std::unexpected("Tool " + t.name + " has no run().");
  }
  if (t.title.empty()) {
    t.title = t.name;
  }
  if (!t.input_schema) {
    Value s = Value::object();
    s.append("type", "object");
    s.append("properties", Value::object());
    s.append("additionalProperties", false);
    t.input_schema = std::move(s);
  }
  const std::uint64_t id = next_id_++;
  const auto it = std::ranges::find_if(tools_, [&t](const Entry& e) { return e.tool.name == t.name; });
  if (it != tools_.end()) {
    *it = Entry{std::move(t), id};
  } else {
    tools_.push_back(Entry{std::move(t), id});
  }
  changed();
  return id;
}

void Dispatcher::remove(std::string_view name, std::uint64_t id) {
  const auto it = std::ranges::find_if(tools_, [name](const Entry& e) { return e.tool.name == name; });
  if (it != tools_.end() && it->id == id) {
    tools_.erase(it);
    changed();
  }
}

bool Dispatcher::has(std::string_view name) const {
  return get(name) != nullptr;
}

const Tool* Dispatcher::get(std::string_view name) const {
  const auto it = std::ranges::find_if(tools_, [name](const Entry& e) { return e.tool.name == name; });
  return it == tools_.end() ? nullptr : &it->tool;
}

Value Dispatcher::definition(const Tool& t) const {
  Value d = Value::object();
  d.append("name", t.name);
  d.append("title", t.title);
  d.append("description", t.description + " (Permission: " + t.group + ".)");
  d.append("inputSchema", t.input_schema ? *t.input_schema : Value::object());
  Value ann = Value::object();
  ann.append("title", t.title);
  Value hints = group_hints(t.group);
  for (const auto& m : hints.as_object()) {
    ann.append(m.key, m.value);
  }
  d.append("annotations", std::move(ann));
  if (t.output_schema) {
    d.append("outputSchema", *t.output_schema);
  }
  return d;
}

Value Dispatcher::list() const {
  const auto a = allowed();
  base::Array out;
  for (const Entry& e : tools_) {
    if (std::ranges::find(a, e.tool.group) != a.end()) {
      out.push_back(definition(e.tool));
    }
  }
  return {std::move(out)};
}

void Dispatcher::on_notify(std::function<void(const Value&)> fn) {
  notifiers_.push_back(std::move(fn));
}

void Dispatcher::changed() {
  Value note = Value::object();
  note.append("jsonrpc", "2.0");
  note.append("method", "notifications/tools/list_changed");
  for (const auto& fn : notifiers_) {
    try {
      fn(note);
    } catch (...) {  // NOLINT(bugprone-empty-catch) — a transport's trouble never stops a registration
    }
  }
}

void Dispatcher::method(std::string name, Method fn) {
  methods_.insert_or_assign(std::move(name), std::move(fn));
}

std::expected<Value, RpcError> Dispatcher::call(std::string_view name, const Value& args, Session& s) {
  const double t0 = now();
  const Tool* t = get(name);
  std::optional<std::string> client;
  if (s.client && !s.client->name.empty()) {
    client = s.client->name;
  } else if (s.client_name && !s.client_name->empty()) {
    client = s.client_name;
  }
  Value entry = Value::object();
  entry.append("time", iso_time(t0));
  entry.append("client", client ? Value(*client) : Value(nullptr));
  entry.append("tool", std::string(name));
  entry.append("group", t != nullptr ? Value(t->group) : Value(nullptr));
  entry.append("args", log_args(args.truthy() ? args : Value::object()));
  entry.append("outcome", "ok");
  entry.append("undo", nullptr);
  entry.append("ms", 0);
  const auto finish = [&](Value res, std::string_view outcome, std::optional<std::string> message) {
    entry.set("outcome", std::string(outcome));
    entry.set("ms", now() - t0);
    if (message) {
      entry.set("message", slice16(*message, 300));
    }
    if (opts_.log) {
      try {
        opts_.log(entry);
      } catch (...) {  // NOLINT(bugprone-empty-catch) — the log never breaks a call
      }
    }
    return res;
  };
  const auto fail = [&](const std::string& msg, std::string_view outcome = "error") {
    Value item = Value::object();
    item.append("type", "text");
    item.append("text", msg);
    Value res = Value::object();
    res.append("content", base::Array{std::move(item)});
    res.append("isError", true);
    return finish(std::move(res), outcome, msg);
  };
  if (t == nullptr) {
    return std::unexpected(RpcError{-32602, "Unknown tool: " + std::string(name), std::nullopt});
  }
  const auto a_list = allowed();
  if (std::ranges::find(a_list, t->group) == a_list.end()) {
    return fail(t->name + " needs the " + t->group + " permission, which is not allowed. " +
                    (opts_.how_to_allow ? opts_.how_to_allow(t->group) : std::string()),
                "refused");
  }
  const Value a = args.is_undefined() || args.is_null() ? Value::object() : args;
  if (!a.is_object()) {
    return fail("arguments must be an object.", "refused");
  }
  Value copy = a;
  const auto problems = check_schema(t->input_schema ? *t->input_schema : Value::object(), copy);
  if (!problems.empty()) {
    std::string joined;
    for (std::size_t i = 0; i < problems.size() && i < 8; ++i) {
      joined += (i != 0 ? " " : "") + problems[i];
    }
    return fail("Invalid arguments: " + joined, "refused");
  }
  CallCtx ctx{t->group, client, s, t->name, entry};
  ToolAnswer r;
  try {
    r = t->run(copy, ctx);
  } catch (const std::exception& e) {
    return fail(e.what());
  }
  if (!r) {
    return fail(r.error().message, r.error().refused ? "refused" : "error");
  }
  if (auto* raw = std::get_if<ToolResult>(&*r)) {
    Value res = Value::object();
    res.append("content", raw->content.truthy() ? raw->content : Value::array());
    if (raw->structured && !raw->structured->is_undefined()) {
      res.append("structuredContent", *raw->structured);
      if (raw->structured->truthy() && raw->structured->is_object() && raw->structured->get("undo").truthy()) {
        entry.set("undo", raw->structured->get("undo"));
      }
    }
    return finish(std::move(res), "ok", std::nullopt);
  }
  const Value& plain = std::get<Value>(*r);
  Value structured;
  if (plain.is_undefined() || plain.is_null()) {
    structured = Value::object();
    structured.append("ok", true);
  } else if (plain.is_object()) {
    structured = plain;
  } else {
    structured = Value::object();
    structured.append("result", plain);
  }
  if (structured.get("undo").truthy()) {
    entry.set("undo", structured.get("undo"));
  }
  Value item = Value::object();
  item.append("type", "text");
  item.append("text", stringify(structured, 1));
  Value res = Value::object();
  res.append("content", base::Array{std::move(item)});
  res.append("structuredContent", structured);
  if (structured.get("error").truthy() && structured.get("isError").truthy()) {
    res.append("isError", true);
    return finish(std::move(res), "error", base::js_string(structured.get("error")));
  }
  return finish(std::move(res), "ok", std::nullopt);
}

std::optional<Value> Dispatcher::handle(const Value& msg, Session& s) {
  const bool is_note = msg.is_object() && !msg.has("id");
  const auto reply = [&](Value result) -> std::optional<Value> {
    if (is_note) {
      return std::nullopt;
    }
    Value r = Value::object();
    r.append("jsonrpc", "2.0");
    r.append("id", msg.get("id"));
    r.append("result", std::move(result));
    return r;
  };
  const auto error = [&](int code, const std::string& message,
                         const std::optional<Value>& data) -> std::optional<Value> {
    if (is_note) {
      return std::nullopt;
    }
    Value e = Value::object();
    e.append("code", code);
    e.append("message", message);
    if (data) {
      e.append("data", *data);
    }
    Value r = Value::object();
    r.append("jsonrpc", "2.0");
    r.append("id", msg.is_object() && msg.has("id") ? msg.get("id") : Value(nullptr));
    r.append("error", std::move(e));
    return r;
  };
  const bool shaped = msg.is_object() && msg.get("jsonrpc").is_string() && msg.get("jsonrpc").as_string() == "2.0" &&
                      msg.get("method").is_string();
  if (!shaped) {
    if (msg.is_object() && msg.get("jsonrpc").is_string() && msg.get("jsonrpc").as_string() == "2.0" &&
        !msg.has("method") && (msg.has("result") || msg.has("error"))) {
      return std::nullopt;  // a response to us: nothing was asked
    }
    const Value id =
        msg.is_object() && (msg.get("id").is_string() || msg.get("id").is_number()) ? msg.get("id") : Value(nullptr);
    Value e = Value::object();
    e.append("code", -32600);
    e.append("message", "Invalid request");
    Value r = Value::object();
    r.append("jsonrpc", "2.0");
    r.append("id", id);
    r.append("error", std::move(e));
    return r;
  }
  const Value params = msg.get("params");
  const Value p = params.truthy() && (params.is_object() || params.is_array()) ? params : Value::object();
  const std::string& method = msg.get("method").as_string();
  const auto client_of = [](const Value& info, std::string_view fallback) {
    if (!(info.truthy() && (info.is_object() || info.is_array()))) {
      return Client{std::string(fallback), ""};
    }
    const Value n = info.get("name");
    const Value v = info.get("version");
    return Client{slice16(n.truthy() ? base::js_string(n) : "", 100),
                  slice16(v.truthy() ? base::js_string(v) : "", 40)};
  };
  try {
    if (method == "initialize") {
      const Value pv = p.get("protocolVersion");
      std::string v(kProtocols[0]);
      if (pv.is_string() && std::ranges::find(kProtocols, pv.as_string()) != kProtocols.end()) {
        v = pv.as_string();
      }
      s.protocol_version = v;
      s.client = client_of(p.get("clientInfo"), "unknown");
      s.initialized_at = now();
      if (opts_.on_event) {
        opts_.on_event("connect", s);
      }
      Value tools = Value::object();
      tools.append("listChanged", opts_.list_changed);
      Value caps = Value::object();
      caps.append("tools", std::move(tools));
      caps.append("logging", Value::object());
      Value info = Value::object();
      info.append("name", opts_.server.name);
      info.append("title", opts_.server.title);
      info.append("version", opts_.server.version);
      Value r = Value::object();
      r.append("protocolVersion", v);
      r.append("capabilities", std::move(caps));
      r.append("serverInfo", std::move(info));
      if (opts_.instructions) {
        if (auto text = opts_.instructions()) {
          r.append("instructions", *text);
        }
      }
      return reply(std::move(r));
    }
    if (method == "notifications/initialized") {
      s.ready = true;
      return std::nullopt;
    }
    if (method == "notifications/cancelled" || method == "notifications/progress" ||
        method == "notifications/roots/list_changed") {
      return std::nullopt;
    }
    if (method == "ping") {
      return reply(Value::object());
    }
    if (method == "logging/setLevel") {
      const Value l = p.get("level");
      s.log_level = l.truthy() ? base::js_string(l) : "info";
      return reply(Value::object());
    }
    if (method == "tools/list" || method == "tools.list") {
      Value r = Value::object();
      r.append("tools", list());
      return reply(std::move(r));
    }
    if (method == "tools/call" || method == "tools.call") {
      if (!p.get("name").is_string()) {
        return error(-32602, method == "tools/call" ? "tools/call needs a tool name." : "tools.call needs a tool name.",
                     std::nullopt);
      }
      auto r = call(p.get("name").as_string(), p.get("arguments"), s);
      if (!r) {
        return error(r.error().code, r.error().message, r.error().data);
      }
      return reply(std::move(*r));
    }
    if (method == "control.hello") {
      s.client = client_of(p.get("client"), "script");
      if (opts_.on_event) {
        opts_.on_event("connect", s);
      }
      Value r = Value::object();
      r.append("app", opts_.server.name);
      r.append("title", opts_.server.title);
      r.append("version", opts_.server.version);
      r.append("protocol", 1);
      r.append("tools", list().as_array().size());
      return reply(std::move(r));
    }
    if (method == "app.info") {
      if (opts_.info) {
        return reply(opts_.info());
      }
      Value r = Value::object();
      r.append("app", opts_.server.name);
      r.append("version", opts_.server.version);
      return reply(std::move(r));
    }
    if (const auto it = methods_.find(method); it != methods_.end()) {
      auto r = it->second(p, s);
      if (!r) {
        return error(r.error().code, r.error().message, r.error().data);
      }
      return reply(std::move(*r));
    }
    if (is_note) {
      return std::nullopt;
    }
    return error(-32601, "Method not found: " + method, std::nullopt);
  } catch (const std::exception& e) {
    return error(-32603, std::string("Internal error: ") + e.what(), std::nullopt);
  }
}

}  // namespace cc::mcp
