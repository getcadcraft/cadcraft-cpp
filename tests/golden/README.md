# Golden references

`js-0.5-dwg.tar.gz` — the CadCraft 0.5 JavaScript reader's output (cc-dwg `readDwg`, frozen at site commit 594ab3e)
for the public fixtures listed in `tools/corpus-public.txt`, in the parity kit's canonical JSON (keys sorted,
`report.ms` left out). Made with the kit (`tools/fork-parity/ref.mjs --public-out <dir>` in the CadCraft site repo),
then `tar --sort=name --owner=0 --group=0 --mtime=2026-10-16 -czf js-0.5-dwg.tar.gz dwg`. The fixtures themselves are
fetched by `tools/corpus.cmake`; the private corpus never enters this repo.

The test `io-dwg.golden` reads each fixture and compares the model with its reference, numbers within the kit's DWG
tolerance (abs 1e-9 + rel 1e-12). The full corpus check (78 files) runs with the kit on the build server.

`js-0.5-dxf.tar.gz` — BimCraft 0.5's `readDxfModel` (dxf-model.js, same frozen commit) for the three public DXF
fixtures, decoded as BimCraft decodes them (UTF-8, Windows-1252 when that gives U+FFFD), in the same canonical JSON. The
parity kit has no DXF engine, so these come from the fork's own script: `node tools/parity/dxf.mjs build/corpus/fixtures
--golden-out <dir>/dxf`, then the same `tar` line with `dxf`. The test `io-dxf.golden` compares them as above; the full
DXF check (104 files: the corpus `_truth` exports, apparel patterns, public fixtures) is `tools/parity/dxf.mjs`.

`js-0.5-compile.tar.gz` — BimCraft 0.5's `compileDrawing` (dwg-compile.js) for the seven public DWG/DXF fixtures, in
the plain form `tools/parity/compile.mjs` compares (Maps as value arrays, typed arrays as arrays, a JSON round trip),
canonical. Made with `node tools/parity/compile.mjs build/corpus/fixtures --golden-out <dir>/compile`, then the same
`tar` line with `compile`. The test `drawing.golden` compares them at the DWG tolerance; the full check (202 files) is
`tools/parity/compile.mjs`.

`js-0.5-import.tar.gz` — CadCraft 0.5's `importDwg` (dwg-import.js) for the four public DWG fixtures: the drawing the
app's document loads, canonical. Made with `node tools/parity/import.mjs build/corpus/fixtures --golden-out <dir>/import`,
then the same `tar` line with `import`. The test `cad.golden` compares them at the DWG tolerance; the full check (110
files) is `tools/parity/import.mjs`.

`js-0.5-doc.tar.gz` — the 0.5 `Doc` (doc.js) after `load()` of that drawing (a DXF: of dxf.js readDxf's drawing), dumped by `tools/parity/doc.mjs` (maps as
[key, value] lists), canonical. Made with `node tools/parity/doc.mjs build/corpus/fixtures --golden-out <dir>/doc`. The
test `doc.golden` compares them; the full check (214 files: 110 DWG, 104 DXF) is `tools/parity/doc.mjs`.

`js-0.5-dwg-write.tar.gz` — the 0.5 `writeDwg` (lib/dwg/write.js) for the four public DWG fixtures in all six versions,
with a fixed date and the harness's seeded random bytes: size, CRC-32 of the bytes and the canonical report of each
write (`writes.json`). Made with `node tools/parity/dwg-write.mjs build/corpus/fixtures --golden-out <dir>/dwg-write/writes.json`,
then the same `tar` line with `dwg-write`. The test `io-dwg.the writer matches the JS writer…` compares them; the full
check (648 writes) is `tools/parity/dwg-write.mjs`.

`js-0.5-dwg-save.tar.gz` — the 0.5 `saveDwg` (dwg-save.js) for the four public DWG fixtures in all six versions: the
fixture opened as the app opens it (readDwg → importDwg → Doc.load) and saved with a fixed date and the harness's
seeded random bytes; size, CRC-32 and canonical report of each save (`saves.json`). Made with
`node tools/parity/dwg-save.mjs build/corpus/fixtures --golden-out <dir>/dwg-save/saves.json`, then the same `tar` line
with `dwg-save`. The test `save.golden` compares them; the full check (660 saves) is `tools/parity/dwg-save.mjs`.

`js-0.5-dxf-save.tar.gz` — the 0.5 `writeDxf` (dxf.js, R12) and `writeDxf2000` (dxf2000.js, R2000) for the four public
DWG fixtures opened as the app opens them: size and CRC-32 of the UTF-8 text and the canonical report of each save
(`saves.json`). Made with `node tools/parity/dxf-save.mjs build/corpus/fixtures --golden-out <dir>/dxf-save/saves.json`,
then the same `tar` line with `dxf-save`. The test `save.golden: saving the public fixtures as DXF…` compares them; the
full check (216 saves) is `tools/parity/dxf-save.mjs`.

`js-0.5-commands.tar.gz` — CadCraft 0.5's command line (commands.js `Engine` on a fresh `Doc`, a stub host) for every
script file in `tests/commands/`: what it printed and prompted, the model, the block definitions, the layers, the drawing's meta, the UCS (current and saved) and the named views after, the current settings, the next id and
the undo / redo labels, canonical (`commands/<file name>`). Made with `node tools/parity/commands.mjs tests/commands/*.json
--golden-out <dir>/commands`, then the same `tar` line with `commands`. The test `cmd.golden: the command scripts …`
replays the scripts and compares.

`js-0.5-walk.tar.gz` — CadCraft 0.5's walk.js (what the app draws) over the model and every sheet of the four public
DWG fixtures opened as the app opens them: every event in order (strokes with their dashed primitives, fills, texts,
points, viewports, masks, construction lines), canonical (`walk/<file>.json`). Made with `node tools/parity/walk.mjs
build/corpus/fixtures --golden-out <dir>/walk`, then the same `tar` line with `walk`. The test `docview.golden: …`
compares; the full check (108 drawings) is `tools/parity/walk.mjs`.

