// SPDX-License-Identifier: Apache-2.0
// A LISP parity session (script.hpp).
#include "cc/lisp/script.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "cc/base/value.hpp"
#include "cc/lisp/lisp.hpp"

namespace cc::lisp {

namespace {

std::string text_of(const base::Value& v, std::string_view key) {
  const base::Value x = v.get(key);
  return x.is_string() ? x.as_string() : std::string();
}

}  // namespace

base::Value run_session(const base::Value& session, const i18n::Catalog* words) {
  reset_counters();
  base::Value pieces = base::Value::array();
  Options o;
  o.words = words;
  // the fuzzer's limits, for replaying what it found (never in a parity script: 0.5 has none)
  if (const base::Value m = session.get("max_steps"); m.is_number() && m.as_number() >= 1) {
    o.max_steps = static_cast<std::uint64_t>(m.as_number());
  }
  if (const base::Value m = session.get("max_string"); m.is_number() && m.as_number() >= 1) {
    o.max_string = static_cast<std::size_t>(m.as_number());
  }
  o.out = [&pieces](const std::string& text, bool piece) {
    base::Value p = base::Value::array();
    p.push(base::Value(text));
    p.push(base::Value(piece));
    pieces.push(std::move(p));
  };
  Interp L(std::move(o));
  base::Value defuns = base::Value::array();
  L.on_defun = [&defuns](const std::string& name) { defuns.push(base::Value(name)); };
  base::Value steps = base::Value::array();
  const base::Value list_of_steps = session.get("steps");
  if (list_of_steps.is_array()) {
    for (const base::Value& s : list_of_steps.as_array()) {
      pieces = base::Value::array();
      std::optional<Result> r;
      if (s.get("eval").is_string()) {
        r = run_now(L.eval_text(s.get("eval").as_string()));
      } else if (s.get("load").is_object()) {
        const base::Value f = s.get("load");
        L.put_file(text_of(f, "name"), text_of(f, "text"));
        r = run_now(L.load_text(text_of(f, "text"), text_of(f, "name")));
      } else if (s.get("put").is_object()) {
        const base::Value f = s.get("put");
        L.put_file(text_of(f, "name"), text_of(f, "text"));
      } else if (s.get("call").is_string()) {
        r = run_now(L.call_command(s.get("call").as_string()));
      }
      base::Value step = base::Value::object();
      if (r) {
        step.set("ok", base::Value(r->ok));
        if (r->ok) {
          step.set("value", base::Value(show(r->value)));
        } else {
          step.set("error", base::Value(r->error));
        }
        step.set("cancel", base::Value(r->cancel));
      }
      step.set("out", std::move(pieces));
      steps.push(std::move(step));
    }
  }
  base::Value commands = base::Value::array();
  for (const std::string& c : L.commands()) {
    commands.push(base::Value(c));
  }
  base::Value out = base::Value::object();
  out.set("name", session.get("name"));
  out.set("steps", std::move(steps));
  out.set("commands", std::move(commands));
  out.set("defuns", std::move(defuns));
  return out;
}

}  // namespace cc::lisp
