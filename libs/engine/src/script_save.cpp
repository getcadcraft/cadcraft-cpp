// SPDX-License-Identifier: Apache-2.0
#include "cc/engine/script_save.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/reader.hpp"
#include "cc/cad/import.hpp"
#include "cc/cad/read_dxf.hpp"
#include "cc/dwg/read.hpp"
#include "cc/dxf/read.hpp"
#include "cc/save/dwg.hpp"
#include "cc/save/dxf.hpp"

namespace cc::engine {

namespace {

base::Value parametric_of(const doc::Doc& d) {
  return base::make_object({{"constraints", d.constraints}, {"params", d.params}});
}

}  // namespace

void script_save(const base::Value& save, const doc::Doc& doc, double now_ms, const i18n::Catalog& words,
                 base::Value& result) {
  if (!save.is_array()) {
    return;
  }
  base::Value out = base::Value::object();
  for (const base::Value& f : save.as_array()) {
    const std::string fmt = base::js_string(f);
    if (fmt == "dxf") {
      const save::DxfSaveResult r = save::write_dxf(doc, words);
      out.set("dxf", r.text);
      doc::Doc again;
      auto drawing = cad::read_dxf(r.text, words, [](std::string_view t) { return dxf::read_dxf_model(t); });
      if (drawing) {
        again.load(std::move(*drawing), words);
      }
      out.set("dxfReopened", parametric_of(again));
    } else if (fmt.starts_with("dwg")) {
      save::DwgSaveOptions o;
      o.version = fmt.substr(3);
      o.created_ms = now_ms;
      auto seed = std::make_shared<std::uint32_t>(1);  // the harness's crypto.getRandomValues
      o.random = [seed](std::span<std::uint8_t> b) {
        for (std::uint8_t& x : b) {
          *seed = *seed * 1103515245U + 12345U;
          x = static_cast<std::uint8_t>(*seed >> 24U);
        }
      };
      const save::DwgSaveResult r = save::save_dwg(doc, o, words);
      out.set(fmt, base::base64(r.bytes));
      std::vector<std::byte> bytes(r.bytes.size());
      for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(r.bytes[i]);
      }
      doc::Doc again;
      const base::Value model = dwg::read_dwg(bytes);
      if (model.get("blocks").is_array() && !model.get("blocks").as_array().empty()) {
        const auto head = std::span<const std::byte>(bytes).first(std::min<std::size_t>(6, bytes.size()));
        again.load(cad::import_dwg(model, base::as_text(head), words), words);
      }
      out.set(fmt + "Reopened", parametric_of(again));
    }
  }
  result.set("saved", out);
}

}  // namespace cc::engine
