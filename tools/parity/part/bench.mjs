// SPDX-License-Identifier: Apache-2.0
// pcdiff/bench.mjs — speed of the JS engine vs cadcraft-cli on the same call scripts.
//   node bench.mjs --cli <cadcraft-cli> [--seeds 1-3] [--runs 5] [--only gen,gen]
// For each generator: every scenario of the seeds, concatenated into one script; JS = D.run in this process after one
// warm-up (median of runs); Rust = `cadcraft-cli part --bench` (median, same warm-up rule). Both include argument
// handling; Rust also parses the script JSON once (outside the timed runs).
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
const T = new URL('.', import.meta.url).pathname;
const D = await import(T + 'driver.mjs');
for (const f of ['ops-stage2.mjs', 'ops-stage3.mjs', 'ops-stage4.mjs', 'ops-stage5.mjs']) Object.assign(D.EXTOPS, (await import(T + f)).OPS);
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const cli = arg('--cli'), runs = +arg('--runs', 5), [s0, s1] = arg('--seeds', '1-3').split('-').map(Number);
const only = arg('--only', '').split(',').filter(Boolean);
function mulberry32(a) { return () => { a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }
const files = ['gen-core.mjs', 'gen-sketch.mjs', 'gen-part.mjs', 'gen-stage2.mjs', 'gen-stage3.mjs', 'gen-stage4.mjs', 'gen-stage4b.mjs', 'gen-stage5.mjs'];
const tmp = fs.mkdtempSync('/tmp/pcbench-');
const median = (a) => a.slice().sort((x, y) => x - y)[a.length >> 1];
const rows = [];
for (const f of files) {
  const G = (await import(T + f)).GENS;
  for (const [name, g] of Object.entries(G)) {
    if (only.length && !only.includes(name)) continue;
    const calls = [];
    for (let s = s0; s <= s1; s++) for (const sc of await g(mulberry32(s), s)) calls.push(...sc.calls);
    const script = { calls };
    const text = JSON.stringify(script);
    const file = `${tmp}/${name}.json`;
    fs.writeFileSync(file, text);
    await D.run(JSON.parse(text));
    const ms = [];
    for (let i = 0; i < runs; i++) { const sc = JSON.parse(text); const t = performance.now(); await D.run(sc); ms.push(performance.now() - t); }
    const rs = JSON.parse(execFileSync(cli, ['part', '--bench', file, '--runs', String(runs)], { maxBuffer: 1 << 30 }).toString());
    const js = median(ms);
    rows.push({ name, calls: calls.length, js, rs: rs.median, x: js / rs.median });
    console.log(`${name.padEnd(12)} ${String(calls.length).padStart(6)} calls  JS ${js.toFixed(1).padStart(9)} ms  Rust ${rs.median.toFixed(1).padStart(9)} ms  ${(js / rs.median).toFixed(1)}x`);
  }
}
fs.rmSync(tmp, { recursive: true, force: true });
