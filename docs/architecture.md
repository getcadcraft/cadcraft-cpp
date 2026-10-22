# Architecture

Last updated 2026-10-21 · Change: libs/solid (SolidOps on Manifold 3.5.4, M6.1); libs/live (Studio Live's MessagePack, envelope and session decisions, M13.1); the Studio sources' provenance; libs/io-ifc, the STEP reader (M7.1); libs/lisp, the LISP core (M5.8i1); LispError beside ReadOverrun.

## Toolchain (pinned)

| Tool | Version | Where (this server) | Notes |
|---|---|---|---|
| Clang / clang-tidy / clang-format / lld / libc++ | 22.1.8 | `/usr/lib/llvm-22` (apt.llvm.org `llvm-toolchain-jammy-22`) | side by side; the system `clang` stays 14 |
| CMake | 4.4.4 | `/opt/cadcraft-cpp/cmake-4.4.4` | Kitware release tarball, SHA-256 checked |
| Ninja | 1.13.2 | `/opt/cadcraft-cpp/ninja-1.13.2` | |
| vcpkg | tag 2026.07.29 = baseline `9e593bb18ea69cc5095e012465dcd675a822ed0d` | `/opt/cadcraft-cpp/vcpkg` | manifest mode; the baseline is in `vcpkg.json` and `vcpkg-configuration.json` |
| Emscripten SDK | 6.0.10 | `/opt/cadcraft-cpp/emsdk` | |
| osv-scanner | 2.7.0 | `/opt/cadcraft-cpp/osv-2.7.0` | not usable on vcpkg (see "Gate", osv) |
| GCC (CI job only) | 14 | GitHub `ubuntu-24.04` image | the server has GCC 11 only; installing a newer libstdc++ system-wide would touch other sites |
| MSVC (CI job only) | 19.4x | GitHub `windows-2022` image | |
| Node (reference runs) | 24.19.0 | system | runs the 0.5 JS build for parity and timings |

`. tools/env.sh` puts these first on PATH for the current shell; nothing else on the server changes.

## Build

- `cc_library(name LAYER n …)` (cmake/CcLibrary.cmake) is the only way a library enters the build; configure fails
  when a library links a library of the same or a higher layer, or a UI toolkit below layer 6. `tools/layers.cmake`
  also greps L0–L5 sources for SDL/ImGui/WebGPU includes.
- cmake/Policy.cmake gives every target of ours: the warning set as errors, the hardened standard library
  (libc++ `_LIBCPP_HARDENING_MODE` FAST, EXTENSIVE in Debug; libstdc++ `_GLIBCXX_ASSERTIONS`; MSVC
  `_MSVC_STL_HARDENING`), compiler hardening, and the preset's sanitizers or fuzzing instrumentation.
- vcpkg triplets in `triplets/`: `x64-linux-cc` (clang + libc++ + lld, so dependencies share our standard library),
  `x64-linux-gcc`, `wasm32-emscripten-cc` (wasm exceptions, threads, SIMD). Dependencies are built release-only, so a
  Debug build of ours links -O2 dependencies.
- Presets: dev, release, asan (ASan + UBSan), ubsan, tsan, fuzz, ci, gcc, msvc, wasm. Each has a workflow preset.

## Gate

`cmake --workflow --preset ci` = Release build with `-Werror`, the unit tests, and these ctest entries (label `gate`;
`heavy` ones build another preset):

| Check | Script |
|---|---|
| gate.format | `tools/format.cmake` — clang-format `--dry-run --Werror` on every source |
| gate.layers | `tools/layers.cmake` |
| gate.tidy | `tools/tidy.cmake` — clang-tidy on files changed against origin/main (`CC_TIDY_ALL=1`: all) |
| gate.assets | `tools/assets.cmake` |
| gate.licenses | `tools/licenses.cmake` — each port's SPDX licence from vcpkg's SBOM against the allowlist |
| gate.osv | `tools/osv.cmake` — osv-scanner cannot read vcpkg (its SBOM purls are `pkg:vcpkg`, which OSV does not index), so the script asks api.osv.dev, the same database, by each port's upstream repository and tag (ecosystem GIT); a hit fails unless waived in docs/security-waivers.md |
| gate.identity | `tools/identity.cmake` — every commit by CadCraft, no assistant attribution |
| gate.asan, gate.tsan, gate.wasm, gate.fuzz (heavy) | `cmake --workflow --preset <p>` |
| gate.fuzz-smoke (heavy) | `tools/fuzz_smoke.cmake` — each libFuzzer target 30 s on a copy of its corpus |

CI (`.github/workflows/ci.yml`) runs the same pieces as parallel jobs, plus GCC and MSVC builds.

## Libraries

| Library | Layer | What |
|---|---|---|
| `libs/base` | L0 | `Error`/`Result`, `Task<T>` (task.hpp: the awaited coroutine commands and LISP evaluation are made of), checked integers, `ByteReader` / `BitReader` (the bounds-checked readers), atomic file writes, `env_var`, `Value` (a JS-shaped value for engine models: undefined, null, bool, number, string, array, ordered object, bytes; JSON in the parity kit's form), `js.hpp` (JS semantics the ports need: trim, parseFloat/parseInt, UTF-16 lengths, toUpperCase/toLowerCase and the ICU numeric collation, both from tables generated from Node by `tools/gen/`) |
| `libs/drawing` | L2 | `cc::drawing::compile_drawing` — BimCraft's dwg-compile.js: a drawing model → typed draw lists (spaces → batches of polylines, fills, texts, in world units), what the 2D view and the renderer read |
| `libs/cad` | L2 | `cc::cad::import_dwg` — CadCraft 0.5's dwg-import.js (and the parts of attrs.js, dwg-props.js, dyn-blocks.js, model3d.js, panes.js, pictures.js, sheets.js, xclip.js, gcons.js it calls): a reader's model → the drawing the CadCraft document loads; JSON-shaped (base::Value), so it compares with the JS on the corpus (tools/parity/import.mjs) |
| `libs/live` | L2 | `cc::live` — Studio Live (docs/LIVE-PROTOCOL.md v1), shared by every app: live-client.js's MessagePack (`pack` / `unpack`: JS numbers, BigInt past 2^53, plain-object key order, TextDecoder's U+FFFD, depth 512, counts checked before allocating), the envelope `{t s c n q b}`, and `SessionCore`, the decisions of desktop-shell live/session.rs (`check` / `commit`: role, locks, holds, versions per key and mode). Passes tools/live-test/vectors.json (copied to tests/golden/js-0.5-live-vectors.json): 54 envelopes, 4 decode-only, 6 bad, 11 scenarios |
| `libs/m3d` | L2 | `cc::m3d` — the start of the ModelCraft kernel (m3d/model.js, vec.js, earcut.js): definitions, faces, instances, `toJSON`, `faceTris`, `meshDef`; and model3d.js's drawing side (`from_file`: a drawing's 3D faces as one group; `for_file3d`: 3DFACEs + CadCraft's JSON for saving) |
| `libs/solid` | L2 | `cc::solid` — `SolidOps`: the mesh booleans of every 3D app on Manifold 3.5.4 + Clipper2 (cmake/Manifold.cmake, pinned by commit and hash), built to compute what 0.5's manifold-3d wasm computes (sequential, no FMA, libc++; one patch for libc++ 22's multimap): `Solid::of_mesh` (the status' JS name when refused), add / subtract / intersect, `union_of` / `intersection_of` (BatchBoolean), trim / split by plane, transforms, the constructors, `mesh()`; `raw()` for the rest of Manifold's API. Meshes are checked before Manifold indexes them (indices past the vertices, runs past the triangles, no positions: the wasm reads arbitrary memory there). The JS keeps every Manifold it made until it deletes it, and Manifold's lazy CSG tree flattens a nested operation only when no other handle holds it: a port keeps the same handles alive (cases.cpp does) or its triangles come out in another order. Manifold numbers meshes from one process-wide counter. `run_cases` runs tools/parity/manifold.mjs's cases |
| `libs/image` | L2 | PNG writing (screenshots), zlib for PDF streams, and pictures.js's file side for print (`picture.hpp`: jpegInfo, 0.5's own decodePng, shrink, pdfImage); `picture-decode` (render builds only) decodes PNG / JPEG for the screen through wuffs (M5.8d) |
| `libs/doc` | L3 | `cc::doc::Doc` — CadCraft 0.5's doc.js: the drawing a session edits (spaces, layers, blocks, layouts), one undo step per `change()`, `load()` of a drawing; equal to the JS document on the corpus (tools/parity/doc.mjs); constraints and parameters (`gcons.hpp`: gcons.js; `params_doc.hpp`: params-doc.js) on `cad::gsolve` (gsolve.js, the solver) and `cad::params` (params.js) |
| `libs/docview` | L4 | `cc::docview::walk` — CadCraft 0.5's walk.js: what a document draws (blocks, dimensions, multileaders expanded; colours, linetypes and lineweights resolved by layer and by block; linetype dashes, pattern hatch lines, polyline widths as bands, text masks), the same events as the JS (tools/parity/walk.mjs); `to_drawing` turns those events into the renderer's draw lists, so the viewer draws the document it edits; `doc::pattern_of` (linetypes.js) lives in libs/doc |
| `libs/lisp` | L4 | `cc::lisp::Interp` — CadCraft 0.5's LISP (lisp/types.js, reader.js, interp.js, builtins.js, vl.js, wild.js, missing.js): values (nil, T, 32-bit integers apart from reals, WTF-8 strings indexed in UTF-16 units, interned symbols, shared immutable conses freed without recursion, objects), the reader, an evaluator of coroutines (`Task<Value>`: it awaits the drawing as commands do; symmetric transfer keeps the native stack flat, so depth is an explicit counter, `kMaxDepth`), dynamic scope restored by RAII, `*error*`, every built-in that needs no drawing, wildcards matched by hand with JS RegExp semantics; `run_session` runs a parity session (tools/parity/lisp.mjs); the drawing side reaches a `Host` (libs/cmd, M5.8i2) |
| `libs/cmd` | L5 | `cc::cmd::Engine` — CadCraft 0.5's command line (commands.js): commands are C++20 coroutines (`Task<T>`, base/task.hpp) that ask through a context (`co_await c.point(…)`, `c.select(…)`), edit the document with `doc.change()` and are one undo step each; Esc destroys the waiting command's frames (no exception for control flow; their destructors are the JS's `finally`). The host interface is what the viewer, the CLI's script runner and the tests implement. libs/cad's edit.hpp holds geom.js's editing geometry the commands use (primitives and intersections, offsetEntity, trimSplit / trimResult, extendEntity, inWindow). `run_script` replays a command script headless (`cadcraft-cli script`; tools/parity/commands.mjs runs the same scripts through the 0.5 Engine) |
| `libs/importer` | L4 | out-of-process import: `run_helper()` spawns `apps/cadcraft-importer`, sends the file bytes on stdin, reads the model JSON back, with a wall-clock limit and an output cap; a crash, hang or garbage answer becomes an `Error` |
| `libs/save` | L4 | `cc::save::save_dwg` — CadCraft 0.5's dwg-save.js: the document → cc-dwg's writer model → `write_dwg`; the same bytes as the JS for the same document (tools/parity/dwg-save.mjs); `write_dxf` (dxf.js, R12, straight from the document) and `write_dxf2000` (dxf2000.js, R2000, from the DWG writer model with `model_only`), the same text as the JS (tools/parity/dxf-save.mjs). The helpers it shares with the commands live in libs/cad (geom.hpp: text layout, bounds, attributes, leaders; dims.hpp: dims.js + dimstyle.js; save_parts.hpp: dwg-props.js write side, colours, hatch patterns, vports, pictures, clips) |
| `libs/io-dxf` | L3 | `cc::dxf::read_dxf_model` — BimCraft's dxf-model.js: ASCII DXF → the same model as read_dwg; `decode_text` as BimCraft decodes the bytes |
| `libs/io-ifc` | L3 | `cc::ifc` — cc-ifc (CadCraft 0.5 lib/ifc, the IFC engine BimCraft and DetailCraft use): `scan_step` / `parse_params` / `parse_complex` (step.js: one pass indexes every instance, parameters parsed when read, strings decoded in UTF-16 as the JS does), step values as `Raw`; the parity kit's ifc-step engine (`step_head_json`, `step_line_json`). Model, geometry and the API follow (M7.2+) |
| `libs/io-dwg` | L3 | `cc::dwg::read_dwg` — the C++ port of cc-dwg `readDwg` (container, LZ, Reed–Solomon, header, classes, object map, ~100 object types, tables, multileaders, dynamic blocks, ACIS, data storage); one source file per JS module. `cc::dwg::write_dwg` — the port of `writeDwg` (src/w*.cpp: bits, CRCs, R2004/R2007 compressors and file layouts, the object encoders, the source's objects copied, tables, solids, surfaces, data storage), byte for byte the JS writer's output (tools/parity/dwg-write.mjs) |

### The JS reference

The engines port the CadCraft 0.5 JavaScript build, frozen for the forks at
`/var/www/CADCRAFT/testfiles/fork-parity/js-0.5/` (site commit 594ab3e; `PIN.json` says what it is): `lib/{dwg,ifc,skp,shapes}`
for the engines, `site/` for the app code of M3+. Never read them from the moving site repo, and never copy anything from
`testfiles/` into this repo. The parity kit (`tools/fork-parity` in the site repo) loads the same copy.

### Studio's sources (W6)

Studio (the shell that hosts the apps) is two repos in 0.5: the site (`assets/js`: host.js, live-client.js,
live-crypto.js, addons.js, addons/trust.js, app-autosave.js, the shell modules; the frozen copy above, site 594ab3e) and
**desktop-shell** (the Tauri app: `src-tauri/src/control.rs`, the control channel and MCP over HTTP; `src-tauri/src/live/`
mod, crypto, session, msg, net, http, relay, ws: the Live session server). The fork ports **desktop-shell at commit
d132555** (2026-10-16, the day of the freeze; its crypto.rs gives tools/live-crypto/vectors.json), never its later 0.6
Live fixes (76a9151). The owner copied those files, with the site's docs/LIVE-CRYPTO.md, tools/live-crypto/vectors.json,
bin/test-live-crypto.mjs and tools/skp-test/{test,sync}.mjs from 594ab3e, into `$CADCRAFT_SITE` at the same relative
paths (2026-10-21). The 0.5 tooling the Rust fork's harnesses use (tools/live-test/*.mjs, tools/mcp, tools/cli) is read
from `/var/www/CADCRAFT/forks/lead/js05/tools`; noble (MIT) from the later site snapshot's `assets/vendor`, only to run
the frozen live-crypto.js in Node. None of it enters this repo except the shared Live vectors (tests/golden).

### Engine models as `Value`

The engines build their output as `cc::base::Value` with the JS field names, order and types (a JS boolean is a bool,
a JS number a double, an absent field stays absent), so the parity kit compares them field for field. This costs speed
(a generic tree of small allocations); the measured numbers are in ROADMAP.md, and the plan to recover it (a typed
fast path for the viewer, an arena for the model) is in docs/roadmap.md, M3.

### Parity checks beyond the kit

The kit compares DWG models. DXF models and draw lists are checked by this repo's own scripts with the kit's
`canon`/`diff`: `tools/parity/dxf.mjs` (readDxfModel, DWG tolerance) and `tools/parity/compile.mjs` (compileDrawing;
abs 1e-9 + rel 1e-9 as the Rust fork's cdiff.mjs, and it reports how many also match at rel 1e-12). Both also run over
the fuzzers' corpora (differential fuzzing: every input the fuzzer kept, JS against C++), which is how the hostile-input
behaviour in docs/gaps.md was found and matched.

### Doubles bit for bit (M4.4)

A saved DWG keeps every double the save computes, so the ports that feed a file compute as V8 does, not merely within a
tolerance: Math.sin / Math.cos / Math.atan / Math.atan2 are fdlibm's in Node (`base::js_sin` … `js_atan2`, checked bit
for bit against Node on millions of inputs; glibc differs on 2.5 % of sin inputs), Math.hypot is V8's own scaled Kahan
sum (`base::js_hypot`), toFixed / toExponential round an exact half away from zero (`base::js_to_fixed`), `+v || 0`
turns -0 into 0, and no compiler may fuse `a * b + c` into an FMA (`-ffp-contract=off` everywhere; GCC fuses by default
where the CPU has FMA, and every arm64 CPU has it). The renderer and the UI keep the C library's functions.

### The 2D viewer (M3, planned)

```
Drawing (typed, libs/drawing)  →  libs/render (L3): scene buffers per space and batch — floats relative to a per-batch
                                   origin (double), so survey coordinates stay steady; polyline segments; fill fans for
                                   an even-odd stencil fill; text runs; the dwg-view.js colour rules (ink, contrast mix)
                                   and viewport transforms (vpMatrix)
libs/text (L2): FreeType       →  glyph metrics and a signed-distance-field atlas (Archivo, OFL, the 0.5 drawing font)
libs/ui-imgui (L6)             →  WebGPU (Dawn native / emdawnwebgpu): pipelines for segments, stencil fills and SDF
                                   text; SDL3 window and input; ImGui chrome (spaces, layers, status bar)
apps/cadcraft (L7) desktop · apps/cadcraft-web (L7) Emscripten · apps/cadcraft-shot (L7) headless PNGs (Vulkan / lavapipe)
```

Rendering follows BimCraft's dwg-view.js: hairlines on screen (print view: line weights), pattern fills at 18 % alpha,
text in screen space skipped under 2 px, sheets drawn on paper with each viewport's model clipped, scaled and twisted.
Colours: the site's tokens (paper: sheet #F3F6F8, ink #17202B; model: sheet #1D232B, ink #E8EEF4), ACI 7 = ink, pale
colours darkened on paper and dark ones lightened on a dark screen.

## The import helper (AGENTS.md section 4, out-of-process import)

`cadcraft-importer <format>` reads the file's bytes on stdin (it never opens a file and gets an empty environment),
then locks itself down before parsing: RLIMIT_AS 4 GiB, RLIMIT_CPU 120 s, no core files, no file writes, no new
processes, `PR_SET_NO_NEW_PRIVS`, and a seccomp-BPF filter that kills it on any system call outside read, write,
close, exit, memory management (brk, mmap, munmap, mremap, madvise, mprotect), futex, rt_sigreturn, clock_gettime,
gettid, getpid. It writes the model as one JSON document (base/json.hpp parses it back with depth and size limits).
Sanitizer builds skip the address-space cap and the filter (their runtimes need both). The client enforces the wall
clock (default 120 s) and the size of the answer. Linux only so far; Windows (job object, restricted token) and macOS
(sandbox profile) read in-process until their desktop builds (docs/gaps.md). Tests: a helper that crashes, hangs or
answers garbage costs a message; the whole corpus parses inside the filter; the helper's model equals the in-process one.

## Errors

Libraries return `std::expected<T, Error>` at their public boundary. **One deliberate internal exception:** the bit
readers (`cc::base::BitReader`, and the DWG decoder built on it) throw `ReadOverrun` on a read past the end. The
decoder catches it at each record boundary and turns it into a note on that record, exactly as cc-dwg's
`DwgReadError` does in the JS reference — so a damaged record keeps the same fields it has in the JS model, which
the parity gate compares. It never crosses a library's public API.

**A second one, in libs/lisp:** `LispError` carries an error of a LISP program (AutoLISP's words: "bad argument
type: consp: 1") from where it happens to the innermost function application, where `*error*` hears it while that
function's locals are still bound, and on to the top level, which returns it as a `Result`. The JS throws through its
awaits the same way, and routines depend on where `*error*` runs, so modelling it as values through every built-in
would only re-implement unwinding. It is copied, never mutated through an `exception_ptr` (MSVC may copy on rethrow),
and stays inside the library: the public API (`Interp::eval_text` … `Result`, `wcmatch` → `Result<bool>`, `balanced`)
never throws it. Cancelling (Esc) is the step check's `LispError` with `cancel`, so `*error*` hears "Function
cancelled" as in 0.5.

## Locks

Every lock gets a name and a place in this order when it is added (AGENTS.md section 6, Threads).

1. `lisp.symbols` (libs/lisp value.cpp): the table of interned symbols. A leaf: held only inside `cc::lisp::sym()`,
   which calls nothing that locks.
