# Building a C++ desktop + web creative app

Drop this file into any empty repo (as `AGENTS.md` or `CLAUDE.md`) and tell the agent what to build.
It needs no other context. It defines the stack, the architecture, the rules, the UI approach, the design
language and the working loop. The *product* comes from the person's request, their reference images and their
reference app.

It is the C++ twin of the Rust guide (`docs/forks/RUST-AGENTS.md`): same architecture, same commands, same
control channel, same tests. What differs is the language's cost: **C++ does not protect memory, so the safety rules
here are heavier, and none of them is optional.**

If a rule here conflicts with the request, follow the request and say so in one line. **Exception:** the safety rules
(section 4, section 9 "Security") and the project rules in section 15 are never relaxed by a request; say what
conflicts and ask.

> Last reviewed 2026-10-15 · Change: first version, translated from the hardened Rust guide for the CadCraft C++ fork.

---

## 1. What you are building

A native Windows / macOS / Linux application **and** a browser build (WebAssembly via Emscripten), from **one C++ codebase**:

- **C++23, no handwritten JS/TS** beyond the glue Emscripten generates.
- **GUI in C++** with Dear ImGui (docking branch) on SDL3, rendered through WebGPU (Dawn on desktop, the browser's WebGPU on web). No webview, no Electron, no Qt.
- **Headless-capable.** Every feature works without a window, from a CLI, a script, or an AI agent.
- **Never loses user work** (section 4), on desktop and web.
- **Fast.** Multithreaded where it pays, cached, render on demand.

---

## 2. How to start a task

1. **Read the request and any reference material first.** Measure reference images into `docs/ui-notes.md` (pixel sizes, regions, hex values) before coding UI. Use a reference app black-box only (section 4, Clean-room) and list its features in `docs/gaps.md`, ranked.
2. **If the request is large, write a short plan** (`docs/roadmap.md`: milestones, each ending in something runnable). Ask only for decisions that are genuinely the user's (names, licences, paid services, destructive actions, anything published or released).
3. **Build vertically.** Model + command + test first, then UI. Done = works headless, has a test, can be seen in a screenshot.
4. **Run the quality gate** (section 11) before every commit. One task per commit, with a task id.
5. **Look at your result.** After any UI change render a screenshot (section 9), read it, compare with the references, fix what is off.

---

## 3. Stack (pin these; bump together)

Pin what is current on day one and record the pins in `docs/architecture.md`; the versions here are orientation only.
Read the pinned library's headers/docs before writing against them.

| Concern | Choice |
|---|---|
| Language | C++23 (`std::expected`, `std::span`, coroutines from C++20); C only inside vendored dependencies |
| Compilers | Clang 18+ (primary, all platforms incl. clang-cl on Windows), MSVC 19.40+ and GCC 13+ must also build |
| Build | CMake 3.28+ with `CMakePresets.json` (dev, release, asan, ubsan, tsan, fuzz, wasm, ci); Ninja |
| Packages | vcpkg manifest mode (`vcpkg.json` with `builtin-baseline` pinned; overlay ports for anything patched); `vcpkg-configuration.json` committed |
| Window / input / dialogs / clipboard | SDL3 (Zlib) — `SDL_ShowOpenFileDialog`, clipboard, IME events |
| UI | Dear ImGui docking branch (MIT), pinned by commit |
| GPU | WebGPU: Dawn (BSD-3) natively over D3D12 / Metal / Vulkan; on web the browser's WebGPU through **emdawnwebgpu** (Dawn's maintained Emscripten bindings; not the deprecated `-sUSE_WEBGPU`). **Web is WebGPU only** (section 10, Web performance). |
| Math | `glm` (MIT, f64 types `dvec3`/`dmat4` in the model) — **not Eigen (MPL-2.0)** |
| 2D geometry | own code; `Clipper2` (BSL-1.0) for polygon offset/booleans |
| 3D booleans | `manifold` (Apache-2.0) behind `SolidOps` |
| Text | FreeType (FTL) + HarfBuzz (MIT) + SheenBidi (Apache-2.0) + libunibreak (Zlib) |
| Images | `wuffs` (Apache-2.0; memory-safe by construction) for PNG/JPEG/GIF/BMP decode; `libspng` (BSD-2) for PNG encode. **Not `stb_image` on untrusted files.** |
| JSON | `nlohmann/json` (MIT) with a depth limit wrapper, or `simdjson` (Apache-2.0) for large reads |
| Archives / XML | `miniz` (MIT), `pugixml` (MIT) |
| Parallelism | `std::jthread` + a small task pool, or oneTBB (Apache-2.0) |
| Tests | Catch2 v3 (BSL-1.0); RapidCheck (BSD-2) for property tests; libFuzzer for fuzzing |
| Logging | `spdlog` (MIT) with `fmt` |

Before adding any dependency: permissive licence (MIT / Apache-2.0 / BSD / ISC / Zlib / BSL-1.0 / FTL / OFL for fonts;
no GPL/LGPL/MPL/EPL), it builds with Emscripten if it sits below the UI layer, it is maintained, its CVE history has been
looked at (`osv.dev`), and it is fuzzed upstream or fuzzed by us before it touches files from strangers. Record it in
`THIRD_PARTY.md`. A script (`tools/licenses.cmake`, run in CI) reads each installed port's `share/<port>/copyright`
and fails on a licence outside the allowlist.

Build profiles: Debug with `-O1` for our code and `-O2` for dependencies; Release `-O2`, ThinLTO, `-g` split debug info.

---

## 4. Non-negotiable rules

### Memory safety is engineered, not assumed

A crash or memory bug loses someone's work and, with hostile files, can run someone else's code. This outranks feature
work: fix it before building on top of it.

- **Ownership:** no raw `new`/`delete`, no owning raw pointers, no `malloc`/`free` in our code. `std::unique_ptr`, `std::shared_ptr<const T>` (copy-on-write), containers, values.
- **No C arrays, no pointer arithmetic, no `reinterpret_cast`, no C-style casts, no `memcpy` on non-trivial types, no varargs, no `union` without `std::variant`.** Parsers read through one bounds-checked `Reader` over `std::span<const std::byte>` (`read_u32_le()` → `std::expected<uint32_t, Error>`); nothing else touches raw bytes.
- **No references, pointers, iterators or `string_view`s held across an edit.** Edits may reallocate containers. Code that spans an edit holds stable ids (u64 newtypes) and looks things up again.
- **Integer safety:** sizes and offsets in `std::size_t`/`uint64_t` through checked helpers (`checked_add`, `checked_mul` → `std::expected`); `-Wconversion -Wsign-conversion` as errors; no NaN-to-int conversion (check `std::isfinite` first); no division by zero.
- **Hardened standard library in every build, release included:** libc++ `_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST` (or `EXTENSIVE` in debug), libstdc++ `_GLIBCXX_ASSERTIONS`, MSVC `_MSVC_STL_HARDENING=1`. `operator[]` on a container then traps instead of reading out of bounds.
- **Compiler hardening:** `-fstack-protector-strong -D_FORTIFY_SOURCE=3 -fstack-clash-protection -fcf-protection=full` (x86) / `-mbranch-protection=standard` (arm64), `-ftrivial-auto-var-init=zero`, PIE, full RELRO, `-z noexecstack`; MSVC `/sdl /guard:cf /Qspectre /CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA`.
- **Warnings are errors:** `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Werror`; MSVC `/W4 /WX /permissive-`.
- **clang-tidy** with `bugprone-*`, `cert-*`, `cppcoreguidelines-*`, `clang-analyzer-*`, `concurrency-*`, `performance-*`, `modernize-*`, `misc-*`, `readability-*` (minus a short, commented exclusion list in `.clang-tidy`), `WarningsAsErrors: '*'`.
- **Sanitizers in CI:** the whole test suite under ASan + UBSan (`-fno-sanitize-recover=all`) and, separately, TSan. A sanitizer report fails the build.
- **Fuzzing:** every reader, every parser of text the user or a file supplies, the control-line parser, the MCP parser and the script parser has a libFuzzer target built with ASan + UBSan. CI runs each on every push (corpus + 30 s) and longer nightly. Every crash becomes a regression test and a corpus entry.
- **Everything from outside is hostile**: files, command params, network, settings, scripts, add-ons, clipboard, dropped files, agent input. Validate, cap every allocation derived from input, bound recursion with explicit depth counters or seen-sets (cyclic documents exist). Stack overflow and out-of-memory cannot be caught: limits are mandatory.

### Errors, exceptions and the crash guard

- Fallible functions return `std::expected<T, Error>` (one `Error` type per library, with a code and a message). No error codes in out-parameters, no `errno`-style globals.
- Exceptions are compiled in (dependencies throw: `std::bad_alloc`, nlohmann/json) but **our code does not throw for control flow**. Every library boundary that calls throwing code catches and converts to `Error`.
- **Last-resort guard:** command execution and every import/export run inside a `try` that catches `std::exception` and `...`, **restores the document to the snapshot taken before the command**, logs, and reports. This catches exceptions only. Undefined behaviour (a bad pointer, an out-of-bounds write) is not catchable — which is why the rules above exist.
- **Out-of-process import (native):** readers of binary formats from strangers (DWG, SKP, IFC, PDF, images over a size) run in a helper process (`app-importer`) with no window, a memory limit and a time limit, and send the parsed document back as the native JSON over a pipe. A crash in a parser then costs a message, not the user's open work. (Same helper on every OS; on web the parser runs in a Worker, so a trap kills only the Worker.)
- `assert` is for programmer errors in debug only; release code never relies on it. No `std::abort`, `std::terminate`, `exit` on an input path.
- **Every crash fix lands with a small synthetic regression test that crashed (or tripped a sanitizer) before the fix.**

### Everything is a command

User-visible behaviour is a command in the engine, never logic in UI code:

- Menus, shortcuts, the command palette, the CLI, the control channel and the agent server all call the **same** `Session::execute(id, params)`.
- A tool gesture is a sequence of commands (section 6). UI-only commands (toggle panel, zoom, open dialog) live in the UI library in a `kUiCommands` table, reachable the same way.
- Every journaled command is replayable as a script. Commands are deterministic: no wall clock, randomness or thread-order effects inside `run` except values the session passes in, which are journaled.

### The UI is thin

Panels read engine state and act through `app.run(id, params)`. Layout and widgets only. Colours come from theme tokens.

### Layering is enforced

Libraries are numbered layers; a library may link only strictly lower layers (section 5). **Nothing below the UI layer
may include SDL, ImGui or WebGPU headers.** Each target is declared with `cc_library(NAME … LAYER n DEPS …)`, a CMake
function that fails at configure time if a dependency's layer is not lower, and a CI script greps include lines of L0–L5
sources for the UI toolkits.

### Clean-room

- Observe other products black-box only: run, use with synthetic data, screenshot, measure. Never read, disassemble or copy their binaries, resources, icons, artwork, presets or wording beyond feature names.
- Never copy GPL/AGPL/LGPL/MPL code. Copyleft tools are **test oracles run as programs** only; never link, ship or read their source.
- Silent spec → experiment on real files and write it down ("Spec gap: …").
- No other product's name in titles or branding; mention it only to say what a format or workflow is compatible with. No other vendor's design-system values.

### Assets

- Every asset is original, OSI-licensed or CC0, with a row in `ASSETS.md`; `tools/assets.cmake` fails CI on a file without a row. Prefer art generated in code.
- Fonts: OFL only. Large/CJK font sets are an optional build input (`-DFONTS_DIR=…`); the app and tests work without it.
- Large binary fixtures stay out of the repo: fetched pinned by commit and sha256-verified (`cmake -P tools/corpus.cmake`). User or customer files never go in any repo.

### Web build

Never break it: the `wasm` preset (Emscripten, `-fwasm-exceptions`, WebGPU port) builds layers L0–L6 in CI. Anything
native-only (threads where not isolated, sockets, helper processes, native dialogs) sits behind `#if !defined(__EMSCRIPTEN__)`
with a web alternative.

---

## 5. Source layout

```
repo/
  CMakeLists.txt  CMakePresets.json  vcpkg.json  vcpkg-configuration.json
  .clang-format  .clang-tidy
  AGENTS.md  ROADMAP.md  ASSETS.md  THIRD_PARTY.md  LICENSE  NOTICE  SECURITY.md
  cmake/                # cc_library(), hardening flags, sanitizer and fuzz options
  assets/
  libs/
    geom/               # L0  math, units, transforms, hit testing, tolerances
    <domain>/           # L1  document model (plain data + JSON)
    ...                 # L2  algorithms (kernels, text/layout, solvers)
    render/             # L3  document -> pixels / GPU scene description (no UI)
    io-*/               # L3  one library per format family
    format/             # L3  native file format (versioned)
    tools/              # L4  tool state machines
    engine/             # L5  Session, commands, history, recovery, scripts
    ui-imgui/           # L6  the whole ImGui frontend
    mcp/                # L6  MCP server
  apps/
    <name>/             # L7  desktop binary
    <name>-cli/         # L7  headless CLI
    <name>-importer/    # L7  out-of-process import helper
    <name>-web/         # L7  Emscripten entry + index.html + CSP
  fuzz/                 # one libFuzzer target per parser
  tests/                # Catch2 suites per library + golden fixtures
  tools/                # cmake -P scripts: layers, assets, licenses, corpus, ci
  docs/
```

Rules of thumb: one library per reason to change; files under about 2,000 lines; headers expose `std::span`,
`std::string_view` (parameters only), ids and values — never internal containers by reference.

| Layer | Holds | May depend on |
|---|---|---|
| L0 | geom, color, units | std + math |
| L1 | document model, fonts | L0 |
| L2 | algorithms | L0–L1 |
| L3 | render, io-*, format | L0–L2 |
| L4 | tools | L0–L3 |
| L5 | engine | L0–L4 |
| L6 | ui-imgui, mcp | L0–L5 |
| L7 | apps | everything |

---

## 6. The core architecture

### Document model

- Plain data + JSON (de)serialisation. No UI types, no I/O.
- Large parts in `std::shared_ptr<const T>`; an edit copies only what it touches, so **an undo snapshot is O(1)**. Local undo = a stack of snapshots.
- **Every journaled edit also yields an op record** `{command, params, base_revision, result_revision}` for the journal, scripts and co-editing. Co-editing never merges snapshots.
- Stable ids (u64 strong types) for everything referenced across edits; ids minted on different machines carry an origin part.
- One internal unit and coordinate convention, documented at the top of the model library; convert only at the edges.
- Versioned format: refuse newer files with a clear message, migrate older ones, a fixture + test per migration.
- Native format = zip with `document.json`, `assets/`, `meta.json`, through an atomic-write helper (temp file in the same directory, flush, `fsync`/`F_FULLFSYNC`/`FlushFileBuffers`, rename (`MoveFileExW` with `MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH` on Windows), fsync the directory on Unix). Keep `name.bak` until the new file is verified readable.
- Reading any zip: cap entry count, per-entry and total uncompressed size, ratio; reject entry names with `..`, absolute paths, drive letters, backslashes.

### Commands

```cpp
struct CommandSpec {
    std::string_view id;                 // "guide.add"
    std::string_view label;              // translation key
    std::span<const std::string_view> menu;
    std::optional<std::string_view> shortcut;
    std::string_view params;             // human/agent-readable params doc incl. return shape
    std::expected<void, std::string> (*enabled)(const Session&);
    std::expected<Json, Error> (*run)(Session&, const Json& params);
    bool journal;
    bool undoable;
    bool touches_files;                  // subject to confinement (section 9)
};
```

- Registered per area (`std::vector<CommandSpec> specs()` per module), aggregated once; a test checks ids are unique and every `params` doc parses.
- Params validated strictly (type, range, finite, size caps, unknown fields refused) with an error naming command and field.
- Mutation through one helper `session.edit([&](Doc&, EditCtx&) -> std::expected<void, Error> {...})`: snapshot, run, record the undo step only if something changed, restore the snapshot on error or caught exception.
- No command runs a shell, loads native code, or fetches a URL chosen by a document or script.

### Commands that ask questions

Interactive commands that wait for points, distances or selections are C++20 coroutines (`Task<Json> line(CmdCtx& c)` that
`co_await c.point(...)`), driven by the UI loop or by scripted input. The same coroutine runs headless with an input
queue, so the CLI and tests drive it exactly like the UI does. Coroutine frames never hold references into the document
across a `co_await` (ids only).

### Tools and gestures

Tools are state machines in their own library; **they never mutate the document**. In: pointer events (`Down|Drag|Up|Move|DoubleClick`,
canvas coordinates, modifiers, click count) and keys. Out: `Action`s — `Begin(cmd)`, `Preview(cmd, params)`, `Commit`,
`Cancel`, `Exec(cmd, params)`, `Dialog(id, params)`, `SwitchTool(id)`, `View(params)` (a `std::variant`). A drag is
Begin → Previews → Commit = one undo step, one op record. Snap settings are a plain struct with one bool per category and a pixel zone.

### Session

Owns documents, history, selection, caches, preferences, the command registry, recovery and scripting; the only thing
UI, CLI and agent servers talk to.

- **Recovery:** every 30 s write dirty documents to a per-instance locked recovery folder (`0700` / per-user ACL); offer them at start; preserve on exit; report when recovery is unavailable. **Web:** OPFS, else IndexedDB, through `Services`; quota errors reported.
- **Caches** key on revision counters of their inputs; invalidate by changing the key.
- **Scripts** are hostile input: commands only, same validation and confinement as agents, an instruction/time budget.

### Threads

- The document is mutated on one thread (the session thread). Workers get immutable snapshots (`shared_ptr<const>`) and return results as values; nothing shares mutable state without a documented lock.
- Every lock has a name and an order in `docs/architecture.md`; TSan runs in CI.
- Background jobs are cancellable (`std::stop_token`) and keep only the newest request.

### Rendering

- Rendering is a pure function of `(document, view, options)` in the render library (no UI), so it also powers headless export and test screenshots (Dawn's null/SwiftShader backend or a CPU path in CI).
- 3D: the viewport is a WebGPU render pass into a texture shown by an ImGui image; picking, snapping and hit-testing on the CPU in lower layers. One GPU mesh per shared definition with instance transforms; render on demand.
- 2D: rasterise on a worker (own scanline rasteriser or the GPU path), present as one texture; overscan for panning; damage-region repaint that is pixel-identical to a full render (tested).
- Overlays (selection, handles, guides, snap markers, measurements) are drawn as vectors by the UI over the view.
- Cap textures at the device limit; sample down rather than fail.

### Files

One library per format family: `std::expected<ReadResult, Error> read(std::span<const std::byte>, const Limits&)` (document +
warnings) / `std::expected<std::vector<std::byte>, Error> write(const Doc&, const Options&)`. Every reader enforces
size, count and depth limits and reports what was dropped. Round-trip tests for every writer; a fuzz target for every reader.

---

## 7. The UI, in C++

### App skeleton

`main` creates the SDL window, the WebGPU device (with the fallback in section 10), ImGui, a `UiApp` (Session + `UiState`)
and runs the loop: poll SDL events → translate to ImGui and to `PointerEvent`s → `ui_app.frame()` → render → present. The
loop sleeps when idle (render on demand; `SDL_WaitEventTimeout`). `UiApp::run(id, params)` is the single entry: UI
commands handled in the UI library, the rest forwarded to `Session::execute`. `UiState` is JSON (unknown fields ignored,
missing ones defaulted) in the platform config dir (`ui.json`, `prefs.json`); `APP_CONFIG_DIR` / `APP_NO_PREFS` isolate tests.
Set `io.IniFilename = nullptr` and persist the docking layout through `UiState` so workspaces are ours, validated and versioned.

### Platform services are injected

The UI library never calls SDL dialogs, the filesystem or the clipboard directly; the host passes a `Services` struct of
`std::function`s: `pick_open`, `pick_save`, `read`, `write`, `open_async` + `inbox` (web), `download` (web), `clipboard_text`,
`recovery`. Capture the operation context before any async picker. Size-check every inbox item before reading it.

### Window anatomy, panels, dialogs, menus

Same as the Rust guide: app bar (~36 px), contextual control bar, tools strip, document tabs + canvas with rulers,
dock with icon rail and tab groups (ImGui docking; layouts saved as named workspaces), status bar with the tool hint
and numeric input, optional contextual task bar. One file per panel; stable panel ids; menus as a data table from the
command registry; disabled items show the `enabled` reason; dialogs for the long tail generated from the `params` doc;
modal dialogs keyboard-driven (Enter confirms, Esc cancels). Docking moves are also commands (`window.panel.*`).

### Custom widgets (do not ship stock ImGui look)

A `widgets` module measured from the references: field 21 px, button 20 px, label 11 px, 1 px borders. Numeric field
with drag-to-scrub, units and arithmetic (`12+3`, `3m`, `10'`, `1p6`) through one `parse_measure` (length-capped,
depth-capped, result checked finite; property test + fuzz target) and one `format_measure`; invalid text reverts and
flashes. Colour field, searchable dropdown, tab strip, section header, toggle, segmented control, tooltip with shortcut.
Hit areas ≥ 18–20 px (40 px+ on touch). Keyboard navigation on (`ImGuiConfigFlags_NavEnableKeyboard`). ImGui has no
screen-reader support: a **known issue, skipped for now** (owner, 2026-10-15). List it in `docs/gaps.md`, don't spend time
on it, and never claim screen-reader support.

### Icons

Drawn in code on a 20×20 design grid through a small `Pen` over `ImDrawList` (line, closed, fill, rect, circle, arc),
or our own monochrome SVGs rasterised at build time (never parse SVG at runtime from documents or the network). App icon
as SVG + PNG + `.ico` + `.icns`; Windows version resource in the `.rc`.

### Theme

One flat `Tokens` struct of colour roles (same list as the Rust guide: `app_bar … focus_ring, radius`); widgets read
tokens, never literals. Presets Dark, Medium Dark, Medium Light, Light, High Contrast; a test checks WCAG contrast of
each. Apply once per change into `ImGuiStyle` (all `ImGuiCol_*` from tokens, rounding, borders, spacing), fonts through
FreeType with fallback fonts for supported scripts, UI scale from a preference (rebuild the font atlas on change).

### Localisation and text direction

`tr(lang, "key")` against JSON catalogs (`lang/<locale>.json`, English fallback); tests fail on keys missing from English.
Format strings with named fills, never concatenated fragments. ImGui does no shaping or bidi: text that needs it
(document text, RTL or complex-script UI) is shaped with HarfBuzz + SheenBidi and drawn as glyph runs. IME through SDL3
text-editing events; test CJK input through the control channel on each OS before claiming a language.

### The canvas widget

Own view transform (pan, zoom, rotation) with one `to_screen`/`to_canvas` pair; zoom about the pointer; pan with
Space-drag, middle drag and scroll; zoom clamped so transforms stay finite. Convert input to `PointerEvent`s once.
Draw order: view texture, overlays, HUD. Cursors per tool and hover target.

---

## 8. Design language

Identical to the Rust guide section 8: dense, flat, dark professional tool by default; neutral grey chrome; one accent
(the product's brand colour, never another vendor's design-system value); radius 1–4 px; 1 px borders; no shadows;
11 px labels, 20–21 px controls; one humanist sans (Inter / Source Sans 3) + JetBrains Mono; thin monochrome line icons;
every tool says what it wants next in the status bar; destructive in-document actions undoable rather than confirmed
(actions outside the document — deleting files, overwriting files on disk, network sends — are confirmed); no motion over
100 ms; start screen with recents, presets, a sample and help; contrast presets, focus rings, UI scale 75–200 %, nothing
by colour alone. Measure references into `docs/ui-notes.md`; match layout and density screen by screen.

---

## 9. Agent control, headless use, testing, security

### Control channel (desktop only)

`app --control <port>` (or `APP_CONTROL_PORT`): loopback-only JSON-lines, `{"id","method","params"}` in,
`{"id","ok","result"|"error"}` out. Rules (all required):

- Bind `127.0.0.1` only; off unless started for this run.
- Token: 32 bytes from the OS RNG, base64url, printed to stderr and written to `control.token` in the config dir (`0600` / per-user ACL). First line must be `{"method":"auth","params":{"token":"…"}}`; constant-time compare; wrong/missing → one error, connection closed. Never in a URL, log or screenshot.
- A line that is not a JSON object with a string `method` → one error, connection closed. Line cap 4 MiB (enforced before parsing); JSON depth cap; at most 16 connections; idle timeout; queued synthetic input capped.
- File confinement: every path in `app.open/save/export`, `ui.screenshot`, `ui.render` and `touches_files` commands must lie inside the read/write roots (default: documents folder + per-run temp; widened by `--automation-read-root/--automation-write-root`) after canonicalising (`std::filesystem::weakly_canonical` then a prefix check on path components; no symlink/junction escapes; Windows: refuse `\\?\`, UNC, device paths, alternate data streams, reserved names).

Methods: the same table as the Rust guide (`engine.execute`, `engine.commands`, `document.inspect`, `ui.inspect`,
`ui.menu.*`, `ui.tool.*`, `ui.pointer`, `ui.key`, `ui.text`, `ui.move/click/drag`, `ui.set`, `ui.dialog.*`,
`ui.screenshot`, `ui.render`, `app.open/save/export/quit`, `window.panel.*`).

### CLI and MCP

`app-cli run --sample --cmd 'id={json}' --export out.png`, `script`, `commands`, `describe`, `mcp` — same engine, no window.
MCP: stdio JSON-RPC 2.0 (`app-cli mcp`), the current protocol version at build time plus the two before it, headless or
`--connect <port> --token-file <f>`; returns PNGs; **confinement on by default** (roots = current directory), widened by
the automation root flags; tool descriptions from the registry, never document content.

### Security checklist (CI and review)

- `SECURITY.md`; sanitizers and fuzzing as in section 4; dependency CVE check (`osv-scanner` on `vcpkg.json`) in CI — a known vulnerability fails the build until fixed or waived in `docs/security-waivers.md` with a reason and date.
- Logs never contain document contents, file contents, tokens or credentials.
- No telemetry, crash upload or update check unless the user switched it on.
- Updates signed (ed25519 / minisign) and verified against a compiled-in key before anything is written.
- Add-ons/plugins are scripts or data through the command API; never `dlopen`/`LoadLibrary` from user-writable locations. On Windows call `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR)` at start (DLL planting).
- The importer helper runs with the least it needs: no network, a job object / rlimits for memory and CPU time, and (where available) a sandbox profile (seccomp on Linux, `sandbox_init`/App Sandbox on macOS, AppContainer or restricted token on Windows).

### Tests

- Catch2 unit tests per library; RapidCheck for parsers, geometry and anything that eats input.
- Golden tests for kernels (fixtures first, then code).
- Command tests through `Session::execute`: document state, undo/redo round trip, op-record replay from empty gives the same document.
- Tool tests: `PointerEvent`s in, `Action`s asserted.
- UI tests: render the full window offscreen (Dawn null/SwiftShader adapter) and snapshot-compare with a small tolerance; a `ui_shot` tool reads a jsonl control script and writes PNGs.
- Format tests: round trip + black-box comparison with an independent reader run as a program.
- Crash-guard tests: a throwing test command leaves the document equal to its snapshot; a crashing importer helper leaves the app running with a message.
- Platform input (copy/paste, context menus, IME, OS-stolen shortcuts) regression-tested through the control channel on every OS shipped.

---

## 10. Platform, packaging, resilience

- **GPU start-up fallback:** pick the WebGPU backend before the window exists from a list; record attempts in `gpu.json`; skip a backend that never presented a frame next start and say so; `APP_GPU_BACKEND` overrides.
- **Quit and close:** one confirm transaction for every close path; macOS termination hook; recovery state on forced exit.
- **macOS:** native menu bar mapped onto the command table (small Objective-C++ file in `apps/<name>/macos/`, the only non-C++ code besides vendored C; reviewed like everything else). **Windows:** GUI subsystem in release, manifest with long paths and per-monitor DPI v2. **Linux:** X11 and Wayland through SDL3.
- **Web:** same UI library via Emscripten; no control socket; browser picker through `inbox`, downloads for Save/Export; strict Content-Security-Policy (`script-src 'self' 'wasm-unsafe-eval'`, no inline script).
- **Web performance (decided: speed is the deciding factor).**
  - **WebGPU only, no WebGL2 fallback.** Design for WebGPU's capabilities: compute culling and picking, storage buffers, indirect draws, render bundles for static geometry, instancing per shared definition, timestamp queries when available.
  - A browser without WebGPU gets one clear screen ("this preview needs WebGPU: use Chrome or Edge, Safari 26+, Firefox 141+ on Windows / 145+ on Apple-silicon Macs, or CadCraft Studio").
  - **Threads and SIMD:** `-pthread` with a fixed pool, `-msimd128`, `-O3 -flto`, `-fwasm-exceptions`, `wasm-opt -O3`. The host sends COOP/COEP so pthreads work; without them the app tells the user it is running on one thread.
  - **Delivery:** brotli, streaming compilation, hashed file names, heavy engines in separate side modules loaded on first use.
  - **Measure, do not assume:** a `bench` CLI command (native) and a `?bench` page (web) time file open, first frame, and frame time while orbiting on the showcase files and a 1M-triangle model; numbers go in `ROADMAP.md`; a slowdown over 5 % needs a reason in its commit.
- **Hosting the test version** (owner, 2026-10-15): the web build is published at **`https://cpp.getcadcraft.com`** ("c++" is not a valid host name), public but unlisted, by invitation: no password; `noindex` (meta + `X-Robots-Tag`) and `robots.txt` disallow all; not linked from getcadcraft.com, its sitemap or `llms.txt`. Deploys there are allowed once it exists; announcing or linking it is the owner's call.
- **Mobile/tablet:** SDL3 runs on Android and iOS, ImGui is not touch-first; say in the roadmap whether and how tablets are covered.
- **Packaging:** MSI (WiX), deb, rpm, `.app`/dmg (signed + notarised), AppImage, Flatpak as needed; CLI and importer ship beside the app. Signing material never in the repo; signing in CI or on a release machine from secrets outside the repo. Reproducible builds from the tag (pinned vcpkg baseline and toolchain, `SOURCE_DATE_EPOCH`, `-ffile-prefix-map`).
- **Settings and logs:** platform config dir, portable mode, atomic saves, backup of a damaged file, schema migrations; spdlog file sink in the config dir, rotated and size-capped; crash handler writes a minidump locally (never uploaded without consent) and recovery state.

---

## 11. Quality gate and workflow

`cmake --workflow --preset ci` must pass before every commit:

1. `clang-format --dry-run --Werror` on changed files
2. Configure + build (Clang, `-Werror`, hardening on) — layer check runs at configure
3. `clang-tidy` on changed files (whole tree nightly)
4. `ctest` (Release) and `ctest` under ASan + UBSan
5. TSan run of the threaded suites
6. `tools/assets.cmake`, `tools/licenses.cmake`, `osv-scanner`
7. `wasm` preset build of L0–L6
8. Fuzz smoke (each target: corpus + 30 s)

MSVC and GCC builds run in CI on every push (a compiler-specific failure is a bug, not noise).

Workflow:

- **One task id per commit** (`M2.1: caret navigation in the text tool`); no unrelated edits; no build output.
- **Docs are part of the work:** update the affected doc and status line, `docs/gaps.md`, one line in `ROADMAP.md`'s progress log; each doc has `Last reviewed / Last updated / Change`.
- **Parallel agents:** separate build directories, edit only the libraries you own, delete your build dir when done.
- **Performance:** measure first (frame, compose, render time); typing and dragging at 60 fps on the sample document.
- **Dependencies on other repos:** pin by commit (vcpkg overlay port or `FetchContent` with `GIT_TAG <sha>` + `URL_HASH` for archives), never a branch.
- **Do not stop to ask** about what the plan covers; ask for product decisions, licences, names, spending, publishing or releasing, anything destructive.

---

## 12. If the app is a 3D modeller / CAD tool

Everything in the Rust guide section 12 applies unchanged: f64 kernel in lower layers with one documented tolerance and
deterministic iteration (`std::map` or insertion-ordered containers, never iteration order of `unordered_*` in output);
**camera-relative rendering for large coordinates** (survey coordinates like x = 500 000 m, y = 4 000 000 m steady to 1 mm,
with a test fixture); B-rep-style topology with definitions and instances; geometry edits as commands with preview variants;
viewport with BVH picking, view cube, standard views, perspective/parallel, sections, display styles, sun and shadows,
reversed-Z; the ranked inference engine with axis and inference locks; the typed value box with units, `[x,y,z]`,
`<dx,dy,dz>`, counts and factors (capped); readers with limits and dropped-feature reports, clean-room for proprietary formats;
printer-readiness checks; 1M triangles interactive. **Booleans** through `manifold` behind `class SolidOps`, fuzzed with
degenerate, non-manifold and huge inputs.

## 13. If the app is a 2D page/vector/photo tool

Same as the Rust guide section 13, with FreeType + HarfBuzz for text, our own line breaker if typography matters,
colour management in its own library (ICC v2/v4; `lcms2` is MIT if a CMM is needed), a guarded PDF writer with font
subsets and real text, document-embedded fonts parsed only through FreeType with limits, and vector overlays.

---

## 14. Definition of done for any feature

- A command exists with doc string, `enabled`, tests, and undo works.
- It runs headless (`app-cli`) and through the control channel.
- The UI exposes it using only tokens and shared widgets.
- A screenshot has been taken and checked against the reference.
- `cmake --workflow --preset ci` passes; docs and `ROADMAP.md` updated; one commit with a task id.
- No new raw ownership, unchecked indexing or casts; hostile-input paths are bounded, fuzzed and sanitizer-clean; a regression test exists for every crash found.

---

## 15. Project rules: the CadCraft C++ fork

This section wins over sections 1–14.

- **What it is.** A fork of CadCraft (the eight apps and CadCraft Studio) in C++. The reference app is the CadCraft 0.5 JavaScript build in `getcadcraft.com` (`public_html/assets/apps/<app>/dev/`, docs in `docs/apps/`, the fork map at `/roadmap-issues/fork-map`). It is ours, so it may be read as source; clean-room rules apply to everyone else's products. CadCraft's clean-room engines (cc-dwg, cc-ifc, cc-skp, m3d) port with their spec-gap notes.
- **Parity is the gate.** A ported engine or format is done only when it matches the JS build on the corpus (`/var/www/CADCRAFT/testfiles`, fetched by the corpus script, never committed) and the existing oracles. Port the JS tests and golden fixtures first. Report parity numbers.
- **Same files, same words, same agents.** Read and write the file formats the 0.5 apps save; use the 0.5 `lang/*.json` catalogs and keys unchanged (en and es-MX at least; es-MX ships as draft) and the Markdown help; MCP tool names and shapes match 0.5's `host.js` / `docs/AGENTS-SPEC.md`.
- **Control channel = Studio's rules** (`desktop-shell/src-tauri/src/control.rs`): 127.0.0.1 only, bearer token (32 random bytes, base64url, constant-time, never in a URL), Host check, any `Origin` → refused, JSON only, 4 MB cap. MCP over HTTP, if the fork has it, follows control.rs exactly.
- **Studio Live:** `docs/LIVE-PROTOCOL.md` version 1, passing `tools/live-test/vectors.json`; edits travel as op records.
- **Licence:** Apache-2.0 for our code; the section 3 allowlist for dependencies. OpenCascade (LGPL), web-ifc (MPL), LibreDWG (GPL), Eigen (MPL) and Qt (LGPL/commercial) stay out — oracles run as programs at most. No server code, secrets, `.env` values or site-only code in the fork's repo.
- **SketchUp files: port our own cc-skp (`lib/skp/`), not OpenSKP.** The owner allowed OpenSKP's C++ (MIT) if nothing faster existed. Measured 2026-10-15, warm, best of 5, parse only: OpenSKP C++ took 630 ms on the 5.5 MB corpus file against cc-skp's 295 ms in Node, and 99 ms against 36 ms on another; the rest were within ±25 %. It is not the fast option. OpenSKP stays a black-box oracle and the speed baseline your port must beat. If a measured port ever falls behind it, raise it with the owner.
- **Identity:** commits and tags as `CadCraft <ddgconsultant@getcadcraft.com>`, no assistant co-author trailers or "Generated with" lines anywhere (commits, PRs, READMEs, release notes, app copy); install the CadCraft commit-msg hook that refuses those names.
- **Repo and test site (owner, 2026-10-15):** the fork lives in its own public repo **`github.com/getcadcraft/cadcraft-cpp`** (approved); its web build goes on **`cpp.getcadcraft.com`** (section 10, Hosting). Pushing to that repo and deploying there are fine.
- **Everything else waits for the owner:** releases, moving any `current` symlink on getcadcraft.com, links from the main site, Discord posts, store uploads, signing with release keys. Report "ready" and ask.
- **Copy:** do not market the fork as "native" or "no browser engine" on the site, in the apps or in posts. Call it a test version of CadCraft.
- **Tablets:** 0.5 Studio runs on Android and iPad; the fork's roadmap says whether and how it covers them.
- **Large coordinates** (section 12) are a requirement: CivilCraft and the GIS layers work in projected survey coordinates.
- **Gate cadence (owner, 2026-10-28):** with parallel workstreams, the full `cmake --workflow --preset ci` runs once per
  stack of commits at its tip, not on every commit; every commit still builds, is clang-tidy clean on the files it
  changes and passes the unit tests and parity scripts of what it changed. The integrator runs the full gate again on
  every integration batch before anything is pushed to main.
- **Release plan (owner, 2026-10-21):** the fork releases two versions behind CadCraft (CadCraft 0.7 → fork 0.5,
  0.9 → 0.7), at parity with the frozen JS of that version (docs/roadmap.md, Release plan).
