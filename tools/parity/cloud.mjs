#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Point clouds: CadCraft 0.5's pointcloud.js against `cadcraft-cli cloud` on the same files — what the file is (its
// header or its first 64 KB), the cloud read (every kept position, colour, intensity and class; the bounds; the class
// counts; the notes), every colour mode, slices, the nearest points of the snapping grid and the summary, at the
// default budget and at the smallest (1000: the keeper's thinning). The file's coordinate system (unit, crsNote) is
// left out until M5.8h2.
//   node tools/parity/cloud.mjs <file|dir>… [--golden-out <dir>]
import { readFileSync, writeFileSync, mkdirSync, readdirSync, statSync } from 'node:fs';
import { join, resolve, basename, dirname } from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { registerHooks } from 'node:module';

const here = dirname(fileURLToPath(import.meta.url));
const site = process.env.CADCRAFT_SITE || '/var/www/CADCRAFT/getcadcraft.com';
const { canon, diff } = await import(join(site, 'tools/fork-parity/lib/canon.mjs'));
const testfiles = process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles';
const js05 = process.env.FORK_PARITY_JS || join(testfiles, 'fork-parity/js-0.5');
const app = join(js05, 'site/public_html/assets/apps/cadcraft/dev');
// the app's import map (proj4, laz-perf) onto the site's vendored files, as the page loads them
const manifest = JSON.parse(readFileSync(join(app, 'manifest.json'), 'utf8'));
registerHooks({
  resolve(spec, ctx, next) {
    const m = manifest.imports?.[spec];
    if (m && !m.startsWith('.')) return { url: pathToFileURL(join(site, 'public_html', m)).href, shortCircuit: true };
    return next(spec, ctx);
  },
});
const imp = (p) => import(pathToFileURL(join(app, p)).href);
(await imp('i18n.js')).init({ locale: 'en', strings: JSON.parse(readFileSync(join(app, 'lang/en.json'), 'utf8')) });
const P = await imp('pointcloud.js');
// LAZ: 0.5's laz-perf (WebAssembly) with its binary handed over, as the site's own pointcloud test does
const L = await imp('las.js');
globalThis.window = globalThis;   // laz-perf's web build checks it is in a page
const LP = L.lazPerf({ wasmBinary: readFileSync(join(site, 'public_html/assets/vendor/laz-perf/current/laz-perf.wasm')) });
await LP;
delete globalThis.window;

const args = process.argv.slice(2);
const gi = args.indexOf('--golden-out');
const goldenOut = gi >= 0 ? args.splice(gi, 2)[1] : null;
const cli = process.env.CC_CLI || resolve(here, '../../build/release/apps/cadcraft-cli/cadcraft-cli');
const files = [];
for (const a of args) {
  const st = statSync(a);
  if (st.isDirectory()) for (const f of readdirSync(a).sort()) { if (/\.(las|laz|xyz|pts)$/i.test(f)) files.push(join(a, f)); }
  else files.push(a);
}

const MODES = ['rgb', 'height', 'intensity', 'class', 'single'];
async function runJs(path, budget) {
  const name = basename(path);
  const file = new File([readFileSync(path)], name);
  const r = { name, budget };
  let info;
  try { info = await P.readCloudInfo(file); } catch (e) { r.error = e.message; return r; }
  r.info = { kind: info.kind, name: info.name, size: info.size, count: info.count, estimate: info.estimate, min: info.min,
    max: info.max, color: info.color, intensity: info.intensity, classes: info.classes, notes: info.notes, columns: info.columns,
    version: info.version, format: info.format };
  let c;
  try { c = await P.loadCloud(file, info, { budget, lazPerf: LP }); } catch (e) { r.error = e.message; return r; }
  r.cloud = { name: c.name, kind: c.kind, read: c.read, n: c.n, origin: c.origin, pos: Array.from(c.pos),
    rgb: c.rgb ? Array.from(c.rgb) : null, inten: c.inten ? Array.from(c.inten) : null, cls: c.cls ? Array.from(c.cls) : null,
    min: c.min, max: c.max, classCounts: [...c.classCounts.entries()].sort((a, b) => a[0] - b[0]), notes: c.notes };
  r.colors = Object.fromEntries(MODES.map((m) => [m, Array.from(P.colorize(c, m))]));
  r.single120 = Array.from(P.colorize(c, 'single', { color: [120, 120, 120] }));
  r.modes = P.colorModes(c);
  const mid = (a) => (c.min[a] + c.max[a]) / 2;
  const slices = [['z', c.min[2] + 1.2, 0.1], ['z', c.min[2] + 0.5, 0.3], ['x', mid(0), 0.2], ['y', mid(1), -0.4]];
  r.slices = slices.map(([axis, at, thick]) => Array.from(P.sliceOf(c, { axis, at, thick })));
  const queries = [[mid(0), mid(1), 0.05], [mid(0), mid(1), 0.5], [c.min[0], c.min[1], 0.2], [c.max[0] + 5, c.max[1], 1],
    [mid(0), mid(1), 1e9]];
  const near = (g) => queries.map(([x, y, rr]) => g.nearest(x, y, rr));
  r.grid = near(new P.PlanGrid(c, null));
  r.gridSlice = near(new P.PlanGrid(c, P.sliceOf(c, { axis: 'z', at: c.min[2] + 1.2, thick: 0.1 })));
  r.gridCell = near(new P.PlanGrid(c, null, 0.25));
  r.summary = P.cloudSummary(c);
  return r;
}

let same = 0, n = 0;
const golden = [];
for (const f of files) {
  for (const budget of [2e6, 1000]) {
    n++;
    const ref = await runJs(f, budget);
    golden.push(canon(ref));
    let got;
    try { got = JSON.parse(execFileSync(cli, ['cloud', f, '--budget', String(budget)], { maxBuffer: 1 << 30 }).toString()); }
    catch (e) { got = { error: 'cadcraft-cli failed: ' + String(e).slice(0, 200) }; }
    const d = diff(canon(ref), canon(got), { abs: 0, rel: 0, max: 1e6 });
    if (!d.length) { same++; continue; }
    console.log(`DIFF ${basename(f)} budget ${budget}: ${d.length}`);
    for (const x of d.slice(0, 8)) console.log(`   ${x.path}: js=${JSON.stringify(x.a)?.slice(0, 140)} cpp=${JSON.stringify(x.b)?.slice(0, 140)}`);
  }
}
if (goldenOut) { mkdirSync(goldenOut, { recursive: true }); writeFileSync(join(goldenOut, 'cloud.json'), JSON.stringify(golden)); }
console.log(`${same}/${n} clouds the same`);
