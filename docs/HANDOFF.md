# Handoff

Last updated 2026-10-21 · Change: the fork moves to another machine after M5.8h5.

Where the C++ fork stands, and what the next machine needs to carry on. The plan is docs/roadmap.md, the log
docs/progress.md, what is missing docs/gaps.md; the rules are AGENTS.md / CLAUDE.md (section 15 wins).

## State at the move

- `main` is complete through **M5.8h5** (VIEWBASE). Every commit passed `cmake --workflow --preset ci` before it was made;
  CI on GitHub was green at M5.8g2 (the runs after it were not checked before the move: check `gh run list` first).
- Parity with CadCraft 0.5 (the frozen JS): command scripts 190/190 (tests/commands/*.json), point clouds 34/34
  (tools/parity/cloud.mjs), walk 108/108, doc 214/214, plot 108/108, DXF save 216/216, DWG save 648/648.

## Next, in order

1. **M5.8i — LISP** (`lisp/` in the 0.5 app, about 3,500 lines: reader, types, an async evaluator, builtins, cadfns,
   host-cad, dxflist, ssget, vla / vla-host, DCL; `install.js` adds APPLOAD, ALIASEDIT, REINIT, DELBLOCKS, `_LISPLOAD`,
   `_LISPEVAL` and every routine's `c:` command; `appload.js` is the page side). The JS evaluator awaits at every step
   because `(command …)` waits on the engine, so the C++ one evaluates with coroutines (`Task<Value>`) and needs an
   explicit depth cap (V8's stack overflow cannot be matched: list it in gaps). Start with the core (types, reader,
   interp, builtins) and a parity runner on pure LISP programs, then the drawing side.
2. Then the 3D tab (m3d, M6) and the other apps, as docs/roadmap.md lists them.

## Waiting on the owner

- **M5.8h2 — a point cloud's coordinate system.** 0.5 reads it through crs.js and `crs-db.json`, a catalog generated
  from the EPSG registry (via PROJ). EPSG data is under IOGP's terms of use, not an OSI licence or CC0, and section 4
  allows only those for assets. Shipping the catalog in this repo is the owner's call. Until then a cloud's unit is
  unknown (its numbers are used as drawing units, with 0.5's note).

## What lives outside the repo

The parity scripts and some tests read files that are never committed (client and corpus files stay out of every repo):

| What | Where it is looked for | Used by |
|---|---|---|
| The frozen CadCraft 0.5 app | `$FORK_PARITY_JS` (default `$CADCRAFT_TESTFILES/fork-parity/js-0.5`) | every tools/parity script |
| The site checkout (canon/diff, vendored proj4 and laz-perf for the JS side) | `$CADCRAFT_SITE` | every tools/parity script; cloud.mjs and commands.mjs load `public_html/assets/vendor/laz-perf/current/laz-perf.wasm` and map `proj4` from the app's manifest |
| The test corpus (DWG/DXF samples, generated files) | `$CADCRAFT_TESTFILES` | walk/doc/plot/save parity; `generated/pointcloud-room/` for cloud.mjs (made once with the site's `tools/pointcloud/fixtures.py make`) |
| The cadcraft-cli the harness runs | `$CC_CLI` (default `build/release/apps/cadcraft-cli/cadcraft-cli`) | every tools/parity script |

Goldens of the JS results are committed (tests/golden/), so `ctest` needs none of the above; regenerate them after adding
command scripts:

```
node tools/parity/commands.mjs tests/commands/*.json --golden-out $G/commands
(cd $G && tar --sort=name --owner=0 --group=0 --mtime=2026-10-16 -czf js-0.5-commands.tar.gz commands)
cp $G/js-0.5-commands.tar.gz tests/golden/
```

## Working habits that held

- Format, build and tidy the changed files before the full gate (tidy findings are errors; the fuzz targets build only
  in the fuzz preset, so the pre-gate misses them).
- Write parity scripts so their steps reach the prompts they mean (a `{cloudFile}` or `{pictures}` step comes before
  the command), and check a script's output says what its name says: two sides refusing the same way also match.
- Every crash found becomes a regression test and a corpus entry (fuzz/corpus/).
- Third-party code that reads files from strangers is built with the hardened standard library and the sanitizers, and
  patched at configure time where it trusts its input (cmake/LazPerf.cmake: seven patches, each anchor checked).
- Commits and tags as `CadCraft <ddgconsultant@getcadcraft.com>`, no assistant names anywhere.
