#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// m3d export parity (M6.3): the model each model script (tests/m3d/*.json, tools/parity/m3d.mjs) ends with, read back
// from its file form, exported in every format by CadCraft 0.5's io.js (ModelCraft's copy) and by `cadcraft-cli
// m3d-export` (cc::m3d::io): GLB, STL (bytes), OBJ, OBJ with materials + MTL, DXF R12, the cc-dwg model (canonical),
// Collada and IFC4 (text, with a fixed date). Bytes must be identical; the first differing byte is shown.
//   node tools/parity/m3d-io.mjs <scripts.json>… [--detail]
//   … --golden-out <dir>: also write each file's results as size + CRC-32 per format (tests/golden/README.md)
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, basename } from 'node:path';
import { execFileSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import zlib from 'node:zlib';

const site = process.env.CADCRAFT_SITE || '/var/www/CADCRAFT/getcadcraft.com';
const { canon, diff } = await import(join(site, 'tools/fork-parity/lib/canon.mjs'));
const testfiles = process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles';
const js05 = process.env.FORK_PARITY_JS || join(testfiles, 'fork-parity/js-0.5');
const app = join(js05, 'site/public_html/assets/apps/modelcraft/dev');
const cli = process.env.CC_CLI || 'build/release/apps/cadcraft-cli/cadcraft-cli';
const imp = (p) => import(pathToFileURL(join(app, p)).href);
const { init } = await imp('i18n.js');
init({ locale: 'en', strings: JSON.parse(readFileSync(join(app, 'lang/en.json'), 'utf8')) });
const K = await imp('model.js');
const IO = await imp('io.js');

const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const detail = args.includes('--detail');
const goldenOut = opt('--golden-out', null);
const files = args.filter((a, i) => !a.startsWith('--') && args[i - 1] !== '--golden-out');

// the model a script ends with, as tools/parity/m3d.mjs builds it (its runner, its JSON)
const out = execFileSync('node', [new URL('./m3d.mjs', import.meta.url).pathname, '--emit-models', ...files], { maxBuffer: 1 << 30 }).toString();
const models = out.trim().split('\n').map((l) => JSON.parse(l));

const DATE = new Date('2026-10-16T00:00:00Z');
const enc = new TextEncoder();
function exportsOf(json) {
  const m = new K.Model();
  m.load(json);
  const r = {};
  r.glb = IO.exportGLB(m);
  r.stl = IO.exportSTL(m);
  r.obj = enc.encode(IO.exportOBJ(m));
  r.objMtl = enc.encode(IO.exportOBJ(m, { mtl: 'model.mtl' }));
  r.mtl = enc.encode(IO.exportMTL(m));
  try { r.dxf = enc.encode(IO.exportDXF(m).text); } catch (e) { r.dxfErr = e.message; }
  try { r.dwg = canon(IO.toDwgModel(m).dwg); } catch (e) { r.dwgErr = e.message; }
  r.dae = enc.encode(IO.exportDAE(m, { name: 'Parity model', date: DATE }));
  r.ifc = enc.encode(IO.exportIFC(m, { name: 'Parity model', date: DATE }));
  return r;
}
const crc = (b) => zlib.crc32(b) >>> 0;

let total = 0, same = 0;
const byFile = new Map();
for (const mm of models) {
  if (!byFile.has(mm.file)) byFile.set(mm.file, []);
  byFile.get(mm.file).push(mm);
}
for (const file of files) {
  const list = byFile.get(basename(file)) || [];
  const got = execFileSync(cli, ['m3d-export', file], { maxBuffer: 1 << 30 }).toString().trim().split('\n').map((l) => JSON.parse(l));
  const golden = [];
  list.forEach((mm, i) => {
    const w = exportsOf(mm.model), g = got[i] || {};
    const gold = { name: mm.name };
    const bad = [];
    for (const k of ['glb', 'stl', 'obj', 'objMtl', 'mtl', 'dxf', 'dae', 'ifc']) {
      if (w[k] === undefined) { if (g[k] !== undefined) bad.push(k + ': JS refused'); continue; }
      gold[k] = [w[k].length, crc(w[k])];
      const gb = k === 'glb' || k === 'stl' ? Buffer.from(g[k] || '', 'base64') : enc.encode(g[k] ?? '');
      if (Buffer.compare(Buffer.from(w[k]), Buffer.from(gb)) !== 0) {
        let at = 0; while (at < w[k].length && w[k][at] === gb[at]) at++;
        bad.push(`${k}: ${w[k].length} vs ${gb.length} bytes, first difference at ${at}` +
          (detail && k !== 'glb' && k !== 'stl' ? ` (JS «${Buffer.from(w[k].slice(Math.max(0, at - 60), at + 60)).toString()}» C++ «${Buffer.from(gb.slice(Math.max(0, at - 60), at + 60)).toString()}»)` : ''));
      }
    }
    if (w.dxfErr !== g.dxfErr) bad.push('dxf refusal: ' + w.dxfErr + ' vs ' + g.dxfErr);
    if (w.dwgErr !== g.dwgErr) bad.push('dwg refusal: ' + w.dwgErr + ' vs ' + g.dwgErr);
    if (w.dwg) { const d = diff(w.dwg, canon(g.dwg), { abs: 0, rel: 0 }); if (d.length) bad.push('dwg model: ' + JSON.stringify(d.slice(0, 3))); gold.dwg = crc(enc.encode(JSON.stringify(w.dwg))); }
    total++;
    if (!bad.length) same++;
    else console.log(`  differs: ${basename(file)} · ${mm.name}: ${bad.join('; ').slice(0, detail ? 4000 : 300)}`);
    golden.push(gold);
  });
  if (goldenOut) { mkdirSync(goldenOut, { recursive: true }); writeFileSync(join(goldenOut, basename(file)), JSON.stringify(golden) + '\n'); }
}
console.log(`m3d-io: ${same}/${total} models export the same bytes as 0.5's io.js (GLB, STL, OBJ, OBJ+MTL, MTL, DXF, DWG model, DAE, IFC)`);
process.exitCode = same === total ? 0 : 1;
