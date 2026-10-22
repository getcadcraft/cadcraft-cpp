#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// CivilCraft engine timings: the frozen 0.5 JS modules against `civilcraft-cli bench` on the same call scripts (big
// surfaces at survey coordinates). Both in-process, best of N after a warm-up; the C++ time includes reading the
// script's JSON and writing the results, the JS time does not.
//   node tools/parity/civil-bench.mjs [--runs N]
import { writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const D = await import(pathToFileURL(path.join(HERE, 'civil/driver.mjs')).href);
const args = process.argv.slice(2);
const RUNS = Number(args[args.indexOf('--runs') + 1] || 3) || 3;
const CLI = process.env.CIVIL_CLI || path.resolve(HERE, '../../build/release/apps/civilcraft-cli/civilcraft-cli');
const rng = (a) => () => { a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };

function site(n, nb, seed) {
  const r = rng(seed), ox = 500000, oy = 4000000, size = 2000;
  const f = (x, y) => 300 + 0.01 * (x - ox) + 20 * Math.sin((x - ox) / 150) * Math.cos((y - oy) / 210);
  const points = [];
  for (let i = 0; i < n; i++) { const x = ox + r() * size, y = oy + r() * size; points.push({ n: String(i + 1), x, y, z: Math.round(f(x, y) * 1000) / 1000, d: 'GRD' }); }
  const breaklines = [];
  for (let b = 0; b < nb; b++) { const pts = []; let x = ox + r() * size, y = oy + r() * size; for (let k = 0; k < 8; k++) { pts.push({ x, y, z: f(x, y) }); x += (r() - 0.5) * 300; y += (r() - 0.5) * 300; } breaklines.push({ pts }); }
  const calls = [{ op: 'tin.buildSurface', args: [{ points, breaklines }], as: 'S' }, { op: 'surface.contours', self: 'S', args: [1] }];
  for (let i = 0; i < 2000; i++) calls.push({ op: 'surface.elevationAt', self: 'S', args: [ox + r() * size, oy + r() * size] });
  return { calls };
}
const cases = [['10k points, 20 breaklines', site(10000, 20, 1)], ['100k points, 100 breaklines', site(100000, 100, 2)]];
const tmp = mkdtempSync(path.join(os.tmpdir(), 'civil-bench-'));
console.log('| Case | 0.5 JS (ms) | C++ (ms) | Speed-up |\n|---|---:|---:|---:|');
for (const [name, script] of cases) {
  let best = Infinity;
  await D.run(script);
  for (let i = 0; i < RUNS; i++) { const t = performance.now(); await D.run(script); best = Math.min(best, performance.now() - t); }
  const f = path.join(tmp, 'b.json');
  writeFileSync(f, JSON.stringify(script));
  const c = JSON.parse(execFileSync(CLI, ['bench', f, '--runs', String(RUNS + 1)], { encoding: 'utf8' }));
  console.log(`| ${name} | ${best.toFixed(0)} | ${c.best_ms.toFixed(0)} | ${(best / c.best_ms).toFixed(2)}× |`);
}
rmSync(tmp, { recursive: true, force: true });
