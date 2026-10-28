// SPDX-License-Identifier: Apache-2.0
// The add-on host against CadCraft 0.5's loader (tests/golden/js-0.5-addon-loader.json, tools/gen/addon-loader.mjs):
// checkManifest, apiParts and cleanAddonData case by case, then the same lifecycle run with built-in modules that
// follow the golden's scripts: list(), what the modules did and the storage after every step. Then what 0.5 has no
// golden for: installed packages refused as code, help, the shell's message once, the app's own API.
#include "cc/addons/loader.hpp"

#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/addons/trust.hpp"
#include "cc/base/js.hpp"
#include "cc/base/json.hpp"

using cc::addons::Api;
using cc::addons::Builtin;
using cc::addons::Loader;
using cc::addons::LoaderOptions;
using cc::addons::MemoryKeyValue;
using cc::addons::Module;
using cc::base::Value;

namespace {

Value golden() {
  std::ifstream in(CC_ADDON_LOADER_GOLDEN, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  auto v = cc::base::parse_json(ss.str(), {.max_bytes = std::size_t{64} << 20U});
  REQUIRE(v.has_value());
  return std::move(*v);
}

std::string canon(const Value& v) {
  return cc::addons::canonical(v);
}

// the run's trace (the modules write into it)
std::vector<std::string>* g_trace = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

class ScriptModule final : public Module {
 public:
  ScriptModule(std::string dir, Value script) : dir_(std::move(dir)), s_(std::move(script)) {}
  std::function<void()> activate(Api& api) override {
    for (const Value& op : s_["activate"].as_array()) {
      const std::string& what = op.as_array()[0].as_string();
      const std::string arg = op.as_array().size() > 1 ? op.as_array()[1].as_string() : std::string();
      if (what == "t") {
        const Value vars = op.as_array().size() > 2 ? op.as_array()[2] : Value();
        g_trace->push_back(dir_ + " t " + arg + " = " + api.t(arg, vars));
      } else if (what == "set") {
        const Value& v = op.as_array()[2];
        const bool ok = api.storage_set(arg, v == Value("$undefined") ? Value() : v);
        g_trace->push_back(dir_ + " set " + arg + " = " + (ok ? "true" : "false"));
      } else if (what == "get") {
        g_trace->push_back(dir_ + " get " + arg + " = " + cc::base::js_stringify(api.storage_get(arg)));
      } else if (what == "cleanup") {
        api.on_deactivate([arg] {  // NOLINT(bugprone-exception-escape) the "$throw" clean-up throws on purpose
          if (arg == "$throw") {
            throw std::runtime_error("x");
          }
          g_trace->push_back("cleanup " + arg);
        });
      } else if (what == "throw") {
        throw std::runtime_error(arg);
      }
    }
    g_trace->push_back(dir_ + " active " + api.version() + " " + api.app_slug() + " " + api.id() + " " +
                       api.addon_version());
    if (s_["returns"].is_string()) {
      const std::string r = s_["returns"].as_string();
      return [r] { g_trace->push_back("cleanup " + r); };  // NOLINT(bugprone-exception-escape) a test's trace
    }
    return nullptr;
  }
  [[nodiscard]] bool has_deactivate() const override { return s_["deactivate"] == Value(true); }
  void deactivate(Api& api) override { g_trace->push_back("deactivate " + api.id()); }

 private:
  std::string dir_;
  Value s_;
};

/// The index's folders as built-in add-ons (0.5's index filter /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/ first).
std::vector<Builtin> builtins_of(const Value& life) {
  std::vector<Builtin> out;
  for (const Value& n : life["index"].as_array()) {
    const std::string& dir = n.as_string();
    if (dir.empty() || dir.contains('/') || dir[0] == '.') {
      continue;
    }
    const Value& b = life["bundled"][dir];
    Builtin bi;
    bi.key = dir;
    bi.manifest = b["manifest"];
    if (b["lang"].is_object()) {
      for (const auto& m : b["lang"].as_object()) {
        bi.lang.emplace(m.key, m.value);
      }
    }
    const Value script = b["script"];
    bi.make = [dir, script]() -> std::unique_ptr<Module> {  // NOLINT(bugprone-exception-escape) test modules
      if (!script.is_object()) {
        return nullptr;
      }
      return std::make_unique<ScriptModule>(dir, script);
    };
    out.push_back(std::move(bi));
  }
  return out;
}

Value dump(const MemoryKeyValue& kv) {
  Value o = Value::object();
  for (const auto& [k, v] : kv.items) {
    o.set(k, v);
  }
  return o;
}

}  // namespace

TEST_CASE("add-on manifests, API versions and add-on data as 0.5 reads them", "[addons]") {
  const Value g = golden();
  std::size_t n = 0;
  for (const Value& c : g["checks"].as_array()) {
    const auto r = cc::addons::check_manifest(c["raw"], c["app"].as_string(), c["apiVersion"].as_string());
    Value got = Value::object();
    got.set("ok", r.ok);
    got.set("manifest", r.manifest);
    got.set("reason", r.reason);
    got.set("detail", r.detail);
    INFO(c["name"].as_string() << " in " << c["app"].as_string() << " " << c["apiVersion"].as_string());
    CHECK(canon(got) == canon(c["out"]));
    ++n;
  }
  CHECK(n == 292);
  for (const Value& c : g["api"].as_array()) {
    const auto r = cc::addons::api_parts(c["v"]);
    INFO(cc::base::js_stringify(c["v"]));
    if (c["out"].is_null()) {
      CHECK(!r.has_value());
    } else {
      REQUIRE(r.has_value());
      if (!r) {
        continue;
      }
      const auto& rv = *r;
      CHECK(rv.major == static_cast<int>(c["out"].as_array()[0].as_number()));
      CHECK(rv.minor == static_cast<int>(c["out"].as_array()[1].as_number()));
    }
  }
  for (const Value& c : g["clean"].as_array()) {
    const Value r = cc::addons::clean_addon_data(c["v"]);
    INFO(cc::base::js_stringify(c["v"]).substr(0, 200));
    if (c["out"] == Value("$undefined")) {
      CHECK(r.is_undefined());
    } else {
      CHECK(cc::base::js_stringify(r) == cc::base::js_stringify(c["out"]));
    }
  }
}

TEST_CASE("the add-on lifecycle as 0.5's loader runs it", "[addons]") {
  const Value g = golden();
  const Value& life = g["lifecycle"];
  const std::vector<Builtin> builtins = builtins_of(life);
  std::vector<std::string> trace;
  g_trace = &trace;
  const auto& steps = life["steps"].as_array();
  std::size_t at = 0;
  const auto snap = [&](const Loader& l, const MemoryKeyValue& kv) {
    REQUIRE(at < steps.size());
    const Value& s = steps[at++];
    INFO("step " << s["step"].as_string());
    Value rows = Value::array();
    for (const auto& r : l.list()) {
      rows.push(r.to_value());
    }
    CHECK(canon(rows) == canon(s["list"]));
    Value t = Value::array();
    for (const auto& x : trace) {
      t.push(x);
    }
    CHECK(canon(t) == canon(s["trace"]));
    CHECK(canon(dump(kv)) == canon(s["storage"]));
    trace.clear();
  };
  const auto make = [&](MemoryKeyValue& kv) {
    LoaderOptions o;
    o.app = "cadcraft";
    o.app_name = "CadCraft";
    o.api_version = "1.0";
    o.locale = "es-MX";
    o.builtins = builtins;
    o.storage = &kv;
    return std::make_unique<Loader>(std::move(o));
  };
  MemoryKeyValue st;
  st.items["cc-addons:cadcraft"] = R"({"x":1,"off":{"gamma":"user","beta":"failed"},"on":{}})";
  st.items["cc-addon:cadcraft:beta:k"] = R"("from before")";
  auto L = make(st);
  (void)L->start();
  snap(*L, st);
  CHECK(L->set_enabled("gamma", true));
  snap(*L, st);
  CHECK(L->set_enabled("beta", true));
  snap(*L, st);
  CHECK(L->set_enabled("alpha", false));
  snap(*L, st);
  CHECK(L->set_enabled("beta", false));
  snap(*L, st);
  CHECK(L->set_enabled("badcleanup", false));
  snap(*L, st);
  CHECK(!L->set_enabled("failing", true));
  snap(*L, st);
  CHECK(L->set_enabled("defoff", true));
  snap(*L, st);
  CHECK(!L->set_enabled("newer", true));
  snap(*L, st);
  CHECK(!L->set_enabled("nobody", true));
  snap(*L, st);
  CHECK(L->set_enabled("alpha", true));
  snap(*L, st);
  auto L2 = make(st);
  (void)L2->start();
  snap(*L2, st);
  MemoryKeyValue st3;
  st3.items["cc-addons:cadcraft"] = "[1,2]";
  auto L3 = make(st3);
  (void)L3->start();
  (void)L3->set_enabled("beta", false);
  snap(*L3, st3);
  MemoryKeyValue st4;
  st4.items["cc-addons:cadcraft"] = R"({"off": 5, "on": [1], "z": {"1": 2}, "2": 0})";
  auto L4 = make(st4);
  (void)L4->start();
  (void)L4->set_enabled("alpha", false);
  snap(*L4, st4);
  CHECK(at == steps.size());
  g_trace = nullptr;
}

namespace {
struct CadApi final : cc::addons::AppApi {
  std::vector<std::string> commands;
};
class UsesApp final : public Module {
 public:
  std::function<void()> activate(Api& api) override {
    auto* app = api.app_api_as<CadApi>();
    if (app == nullptr) {
      throw std::runtime_error("no CadCraft API");
    }
    app->commands.push_back(api.id() + ":wire");
    return [app] { app->commands.clear(); };
  }
};
}  // namespace

TEST_CASE("add-on host: packages are refused as code, help, the shell told once, the app's API", "[addons]") {
  const Value good = *cc::base::parse_json(
      R"({"id":"electrical","name":"Electrical","version":"1.0.0","apps":["cadcraft"],"api":"1.0","license":"Apache-2.0","entry":"./main.js","help":"./help/help.js"})");
  Builtin b;
  b.key = "electrical";
  b.manifest = good;
  b.help = *cc::base::parse_json(R"([{"id":"wires","title":"Wires"}])");
  b.make = [] { return std::make_unique<UsesApp>(); };
  Builtin bad;
  bad.key = "broken";
  bad.manifest = *cc::base::parse_json(
      R"({"id":"broken","name":"Broken","version":"1.0.0","apps":["cadcraft"],"api":"1.0","license":"MIT","entry":"main.js"})");
  bad.make = []() -> std::unique_ptr<Module> { throw std::runtime_error("cannot make"); };

  cc::addons::MemoryPackageStore pkgs;
  cc::addons::Package p;
  p.manifest = *cc::base::parse_json(
      R"({"id":"com.example.pipes","name":"Pipes","version":"2.0.0","apps":["cadcraft"],"api":"1","license":"MIT","entry":"main.js"})");
  pkgs.packages.push_back(p);
  cc::addons::Package other = p;
  other.manifest.set("id", "electrical");  // the bundled copy of an id wins
  pkgs.packages.push_back(other);

  MemoryKeyValue kv;
  kv.items["cc-addons-dev"] = "1";  // developer mode changes nothing: there is no engine to run a package
  std::vector<std::string> said;
  std::vector<std::string> helped;
  auto cad = std::make_shared<CadApi>();
  LoaderOptions o;
  o.app = "cadcraft";
  o.api_version = "1.0";
  o.builtins = {b, bad};
  o.storage = &kv;
  o.packages = &pkgs;
  o.make_api = [cad](Api&, const cc::addons::Entry&) { return cad; };
  o.say = [&](std::string_view key, const Value& vars) {
    said.push_back(std::string(key) + " " + vars["name"].as_string());
  };
  o.help = [&](std::string_view id, const Value& topics) {
    helped.push_back(std::string(id) + " " + cc::base::js_stringify(topics));
  };
  Loader L(std::move(o));
  int calls = 0;
  const auto off = L.on([&](const std::vector<cc::addons::Row>&) { ++calls; });
  const auto rows = L.start();
  REQUIRE(rows.size() == 3);
  CHECK(rows[0].state == "active");
  CHECK(cad->commands == std::vector<std::string>{"electrical:wire"});
  CHECK(helped == std::vector<std::string>{R"(electrical [{"id":"wires","title":"Wires"}])"});
  CHECK(rows[1].state == "failed");
  CHECK(rows[1].reason == "import");
  CHECK(rows[1].detail == "cannot make");
  CHECK(rows[2].id == "com.example.pipes");
  CHECK(rows[2].source == "installed");
  CHECK(rows[2].state == "refused");
  CHECK(rows[2].reason == "code");
  CHECK(rows[2].detail == "main.js");
  CHECK(said == std::vector<std::string>{"shell.addons.failed.say Broken"});
  CHECK(!L.set_enabled("broken", true));
  CHECK(said.size() == 1);  // once per add-on
  CHECK(!L.set_enabled("com.example.pipes", true));
  CHECK(calls > 0);
  off();
  const int before = calls;
  CHECK(L.set_enabled("electrical", false));
  CHECK(cad->commands.empty());
  CHECK(calls == before);

  const auto inst = L.install_package(p);
  CHECK(!inst.ok);
  CHECK(inst.reason == "code");
  cc::addons::Package wrong = p;
  wrong.manifest.set("api", "2");
  CHECK(L.install_package(wrong).reason == "api-newer");
  L.uninstall("com.example.pipes");
  CHECK(L.list().size() == 2);
  CHECK(pkgs.packages.size() == 1);
}

TEST_CASE("add-on host: storage that throws, no storage", "[addons]") {
  struct Blocked final : cc::addons::KeyValue {
    std::optional<std::string> get_item(std::string_view /*key*/) override { throw std::runtime_error("blocked"); }
    void set_item(std::string_view /*key*/, std::string /*value*/) override { throw std::runtime_error("blocked"); }
    void remove_item(std::string_view /*key*/) override { throw std::runtime_error("blocked"); }
  } blocked;
  struct Keeps final : Module {
    std::function<void()> activate(Api& api) override {
      (void)api.storage_set("k", 1);
      CHECK(api.storage_get("k").is_null());
      api.storage_remove("k");
      return nullptr;
    }
  };
  Builtin b;
  b.key = "keeps";
  b.manifest = *cc::base::parse_json(
      R"({"id":"keeps","name":"Keeps","version":"1.0.0","apps":["cadcraft"],"api":"1.0","license":"MIT","entry":"main.js"})");
  b.make = [] { return std::make_unique<Keeps>(); };
  for (cc::addons::KeyValue* kv :
       {static_cast<cc::addons::KeyValue*>(&blocked), static_cast<cc::addons::KeyValue*>(nullptr)}) {
    LoaderOptions o;
    o.app = "cadcraft";
    o.api_version = "1.0";
    o.builtins = {b};
    o.storage = kv;
    Loader L(std::move(o));
    CHECK(L.start().at(0).state == "active");
    CHECK(!L.dev_mode());
    CHECK(L.set_enabled("keeps", false));
    CHECK(L.list().at(0).state == "off");
  }
}
