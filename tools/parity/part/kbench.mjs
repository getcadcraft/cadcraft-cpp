// SPDX-License-Identifier: Apache-2.0
// pcdiff/kbench.mjs — M12: regeneration speed, JS mesh kernel (kernel.js on the manifold-3d 3.5.4 wasm) vs cc-part's
// kernel (cadcraft-cli part --bench), on the parts the kernel generators make.
//   node pcdiff/kbench.mjs --cli <cadcraft-cli> [--seeds 1-3] [--runs 5] [--top 8]
// Every distinct part the `kernel` and `kthread` generators rebuild is timed once in JS; the slowest (plus every
// first modelled threads, the slowest patterns of 20+ instances and the largest other meshes) are then timed both ways: JS = K.regenerate in a fresh kernel.js
// module and wasm instance per run (median, one warm-up); Rust = a one-call script `util.get(stats($k part), 'tris')`
// under `part --bench` (median, same warm-up rule; the script is parsed once outside the timed runs).
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
const T = new URL('.', import.meta.url).pathname;
const D = await import(T + 'driver.mjs');
const OK = await import(T + 'ops-kernel.mjs');
Object.assign(D.EXTOPS, OK.OPS);
const G = (await import(T + 'gen-kernel.mjs')).GENS;
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const cli = arg('--cli'), runs = +arg('--runs', 5), top = +arg('--top', 8), [s0, s1] = arg('--seeds', '1-3').split('-').map(Number);
function mulberry32(a) { return () => { a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }
const median = (a) => a.slice().sort((x, y) => x - y)[a.length >> 1];
const parts = new Map();
const collect = (v) => {
  if (Array.isArray(v)) return v.forEach(collect);
  if (!v || typeof v !== 'object') return;
  if (v.$k && v.$k.features) parts.set(JSON.stringify(v.$k), v.$k);
  if (v.format === 'partcraft' && Array.isArray(v.features)) parts.set(JSON.stringify(v), v);
  Object.values(v).forEach(collect);
};
for (const name of ['kernel', 'kthread']) for (let s = s0; s <= s1; s++) for (const sc of await G[name](mulberry32(s * 7919 + name.length * 131 + name.charCodeAt(0)), s)) collect(sc.calls);
const regenJs = async (part) => { await OK.freshKernel(); const K = OK.kernelK(), M = OK.kernelM(); const p = structuredClone(part); const t = performance.now(); let r = null; try { r = K.regenerate(p, M); } catch (e) { trapped++; } return { ms: performance.now() - t, r }; };
let trapped = 0;
const label = (part, r) => {
  const f = part.features.map((x) => x.type);
  const th = [...(r.threads || new Map()).values()].find((x) => x.modelled);
  const pat = part.features.filter((x) => /pattern/.test(x.type)).map((x) => (x.count ?? 1) * (x.count2 ?? 1));
  return th ? `modelled thread ${th.callout ?? ''}`.trim() : pat.length && Math.max(...pat) >= 20 ? `pattern ×${Math.max(...pat)}` : `${part.name || 'part'} (${f.length} features)`;
};
const all = [];
for (const part of parts.values()) {
  const { ms, r } = await regenJs(part);
  if (!r?.stats) continue;
  all.push({ part, ms, tris: r.stats.tris, what: label(part, r), modelled: [...(r.threads || new Map()).values()].some((x) => x.modelled) });
}
all.sort((a, b) => b.ms - a.ms);
const big = all.filter((x) => !x.modelled).sort((a, b) => b.tris - a.tris);
const pick = [...new Set([...all.slice(0, top), ...all.filter((x) => x.modelled).slice(0, 3), ...all.filter((x) => /^pattern/.test(x.what)).sort((a, b) => b.ms - a.ms).slice(0, 3), ...big.slice(0, 3)])];
const tmp = fs.mkdtempSync('/tmp/kbench-');
console.log(`${parts.size} parts, ${all.length} with a mesh (${trapped} wasm traps skipped); timing ${pick.length}`);
console.log('| part | triangles | JS ms | Rust ms | JS / Rust |\n|---|---:|---:|---:|---:|');
let sj = 0, sr = 0;
for (const [i, c] of pick.entries()) {
  const ms = [];
  await regenJs(c.part);
  for (let k = 0; k < runs; k++) ms.push((await regenJs(c.part)).ms);
  const file = `${tmp}/p${i}.json`;
  fs.writeFileSync(file, JSON.stringify({ calls: [{ op: 'util.get', args: [{ $k: c.part, get: 'stats' }, 'tris'] }] }));
  const rs = JSON.parse(execFileSync(cli, ['part', '--bench', file, '--runs', String(runs)], { maxBuffer: 1 << 30 }).toString());
  const js = median(ms);
  sj += js; sr += rs.median;
  console.log(`| ${c.what} | ${c.tris} | ${js.toFixed(1)} | ${rs.median.toFixed(1)} | ${(js / rs.median).toFixed(2)}× |`);
}
console.log(`| **total** | | ${sj.toFixed(1)} | ${sr.toFixed(1)} | ${(sj / sr).toFixed(2)}× |`);
fs.rmSync(tmp, { recursive: true, force: true });
