#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Manifold parity (M6.1): the same boolean cases through manifold-3d 3.5.4 (the WebAssembly CadCraft 0.5 loads) and
// through `cadcraft-cli manifold <cases.json>` (cc::solid on Manifold 3.5.4 in C++, cmake/Manifold.cmake). Every
// result float is compared as its bits: status, emptiness, volume, the mesh's vertex properties and triangles.
//   node tools/parity/manifold.mjs [--seeds A-B] [--per N] [--only SEED] [--prefixes] [--detail]
//   … --golden-out <file>: write the cases with the wasm's results (tests/golden/js-0.5-manifold.json; the libs/solid
//     test replays them)
// A case is a small stack program (libs/solid/include/cc/solid/cases.hpp has the steps): meshes given as numbers
// (welded and merged as solids.js does), the constructors, transforms, the booleans, trim and split by a plane.
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { execFileSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';

const testfiles = process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles';
const js05 = process.env.FORK_PARITY_JS || join(testfiles, 'fork-parity/js-0.5');
// the frozen copy's vendor/manifold-3d/3.5.4 (manifold.js + manifold.wasm, the npm files); MANIFOLD_JS overrides
const mjs = process.env.MANIFOLD_JS || join(js05, 'vendor/manifold-3d/3.5.4/manifold.js');
const cli = process.env.CC_CLI || 'build/release/apps/cadcraft-cli/cadcraft-cli';
const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const detail = args.includes('--detail');
const [s0, s1] = opt('--seeds', '1-20').split('-').map(Number);
const per = Number(opt('--per', '25'));
const goldenOut = opt('--golden-out', null);

const M = await (await import(pathToFileURL(mjs).href)).default();
M.setup();

// ---- cases
function rng(seed) { let s = seed >>> 0 || 1; return () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s / 4294967296; }; }
/** A box as triangles (solids.js shape: corners, then triangles CCW from outside). */
function boxTris(o, s) {
  const P = [];
  for (let i = 0; i < 8; i++) P.push([o[0] + (i & 1 ? s[0] : 0), o[1] + (i & 2 ? s[1] : 0), o[2] + (i & 4 ? s[2] : 0)]);
  const q = [[0, 2, 3, 1], [4, 5, 7, 6], [0, 1, 5, 4], [2, 6, 7, 3], [0, 4, 6, 2], [1, 3, 7, 5]];
  const out = [];
  for (const [a, b, c, d] of q) { out.push([P[a], P[b], P[c]], [P[a], P[c], P[d]]); }
  return out;
}
/** A prism over a regular n-gon: radius r, height h, at c. */
function prismTris(c, r, h, n) {
  const ring = (z) => Array.from({ length: n }, (_, i) => [c[0] + r * Math.cos((2 * Math.PI * i) / n), c[1] + r * Math.sin((2 * Math.PI * i) / n), c[2] + z]);
  const lo = ring(0), hi = ring(h), out = [];
  for (let i = 1; i + 1 < n; i++) { out.push([lo[0], lo[i + 1], lo[i]], [hi[0], hi[i], hi[i + 1]]); }
  for (let i = 0; i < n; i++) { const j = (i + 1) % n; out.push([lo[i], lo[j], hi[j]], [lo[i], hi[j], hi[i]]); }
  return out;
}
/** solids.js toManifold's weld (0.1 µm about o) into the numbers of a Mesh. */
function meshOf(tris, o) {
  const key = new Map(), verts = [], idx = [];
  const vid = (p) => {
    const q = [p[0] - o[0], p[1] - o[1], p[2] - o[2]];
    const k = q.map((x) => Math.round(x * 1e7)).join(',');
    let i = key.get(k);
    if (i === undefined) { i = verts.length / 3; key.set(k, i); verts.push(...Float32Array.from(q)); }
    return i;
  };
  for (const t of tris) { const a = vid(t[0]), b = vid(t[1]), c = vid(t[2]); if (a !== b && b !== c && a !== c) idx.push(a, b, c); }
  return { vp: verts, tv: idx };
}
function makeCase(seed) {
  const r = rng(seed), pick = (a) => a[Math.floor(r() * a.length)], num = (lo, hi) => lo + (hi - lo) * r();
  const steps = [];
  const shape = () => {
    const kind = pick(['box', 'box', 'prism', 'cube', 'cylinder', 'sphere']);
    const o = [num(-1, 1), num(-1, 1), num(-1, 1)];
    if (kind === 'box') steps.push({ mesh: meshOf(boxTris(o, [num(0.2, 2), num(0.2, 2), num(0.2, 2)]), [0.3, -0.2, 0.1]), merge: true });
    else if (kind === 'prism') steps.push({ mesh: meshOf(prismTris(o, num(0.2, 1.5), num(0.2, 2), 3 + Math.floor(r() * 30)), [0, 0, 0]), merge: true });
    else if (kind === 'cube') steps.push({ cube: [num(0.1, 2), num(0.1, 2), num(0.1, 2)], center: r() < 0.5 }, { translate: o });
    else if (kind === 'cylinder') steps.push({ cylinder: [num(0.1, 2), num(0.1, 1), r() < 0.5 ? -1 : num(0, 1), 3 + Math.floor(r() * 40), r() < 0.5] }, { translate: o });
    else steps.push({ sphere: [num(0.2, 1.2), 4 + Math.floor(r() * 28)] }, { translate: o });
    if (r() < 0.3) {
      const a = num(0, Math.PI * 2), c = Math.cos(a), s = Math.sin(a);
      steps.push({ transform: [c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, num(-0.5, 0.5), num(-0.5, 0.5), 0, 1] });
    }
  };
  const n = 2 + Math.floor(r() * 3);
  for (let i = 0; i < n; i++) shape();
  const op = pick(['add', 'subtract', 'intersect', 'union', 'intersection', 'mix']);
  if (op === 'union' || op === 'intersection') steps.push({ [op]: n });
  else if (op === 'mix') { for (let i = 1; i < n; i++) steps.push({ op: pick(['add', 'subtract', 'intersect']) }); }
  else for (let i = 1; i < n; i++) steps.push({ op });
  if (r() < 0.25) { const v = [num(-1, 1), num(-1, 1), num(-1, 1)]; steps.push({ [pick(['trim', 'split'])]: [...v, num(-0.5, 0.5)] }); }
  return { seed, steps };
}
// solids.js' own flow on a hand-made case: an overlapping pair of boxes, their union and difference
const fixed = [
  { name: 'boxes union', steps: [{ mesh: meshOf(boxTris([0, 0, 0], [1, 1, 1]), [0.75, 0.5, 0.5]), merge: true }, { mesh: meshOf(boxTris([0.5, 0, 0], [1, 1, 1]), [0.75, 0.5, 0.5]), merge: true }, { union: 2 }] },
  { name: 'box minus prism', steps: [{ mesh: meshOf(boxTris([0, 0, 0], [2, 2, 1]), [1, 1, 0.5]), merge: true }, { mesh: meshOf(prismTris([1, 1, -0.5], 0.4, 2, 24), [1, 1, 0.5]), merge: true }, { op: 'subtract' }] },
  { name: 'open mesh refused', steps: [{ mesh: { vp: [0, 0, 0, 1, 0, 0, 0, 1, 0], tv: [0, 1, 2] }, merge: true }] },
  { name: 'disjoint intersection is empty', steps: [{ cube: [1, 1, 1], center: false }, { cube: [1, 1, 1], center: false }, { translate: [3, 0, 0] }, { op: 'intersect' }] },
];

// ---- the wasm side
const hexF = (f) => { const b = new DataView(new ArrayBuffer(4)); b.setFloat32(0, f); return b.getUint32(0).toString(16).padStart(8, '0'); };
const hexD = (d) => { const b = new DataView(new ArrayBuffer(8)); b.setFloat64(0, d); return b.getBigUint64(0).toString(16).padStart(16, '0'); };
function runJs(c) {
  const st = [], made = [];
  const keep = (m) => { made.push(m); return m; };
  try {
    for (const s of c.steps) {
      if (s.mesh) {
        const mesh = new M.Mesh({ numProp: 3, vertProperties: new Float32Array(s.mesh.vp), triVerts: new Uint32Array(s.mesh.tv) });
        if (s.merge) mesh.merge();
        try { st.push(keep(new M.Manifold(mesh))); } catch (e) { return { error: e.code || String(e.message) }; }
      } else if (s.cube) st.push(keep(M.Manifold.cube(s.cube, s.center)));
      else if (s.cylinder) st.push(keep(M.Manifold.cylinder(...s.cylinder)));
      else if (s.sphere) st.push(keep(M.Manifold.sphere(...s.sphere)));
      else if (s.translate) st.push(keep(st.pop().translate(s.translate)));
      else if (s.transform) st.push(keep(st.pop().transform(s.transform)));
      else if (s.op) { const b = st.pop(), a = st.pop(); st.push(keep(s.op === 'add' ? a.add(b) : s.op === 'subtract' ? a.subtract(b) : a.intersect(b))); }
      else if (s.union || s.intersection) { const k = s.union || s.intersection, xs = st.splice(st.length - k, k); st.push(keep(s.union ? M.Manifold.union(xs) : M.Manifold.intersection(xs))); }
      else if (s.trim) st.push(keep(st.pop().trimByPlane(s.trim.slice(0, 3), s.trim[3])));
      else if (s.split) { const [a, b] = st.pop().splitByPlane(s.split.slice(0, 3), s.split[3]); st.push(keep(a), keep(b)); }
    }
    return { out: st.map((m) => { const g = m.getMesh(); return { status: m.status(), empty: m.isEmpty(), volume: hexD(m.volume()), numProp: g.numProp, vp: Array.from(g.vertProperties, hexF).join(''), tv: Array.from(g.triVerts).join(',') }; }) };
  } finally { for (const m of made) m.delete?.(); }
}

let cases = [...fixed];
for (let seed = s0; seed <= s1; seed++) for (let k = 0; k < per; k++) cases.push(makeCase(seed * 1000 + k));
// --only <seed>: that case alone; --prefixes: each case cut after each of its steps too (finds the step that differs)
if (opt('--only', null)) cases = cases.filter((c) => String(c.seed) === opt('--only'));
if (args.includes('--prefixes')) cases = cases.flatMap((c) => c.steps.map((_, i) => ({ ...c, name: `${c.name || c.seed} steps 1-${i + 1}`, steps: c.steps.slice(0, i + 1) })));
const tj = process.hrtime.bigint();
const want = cases.map(runJs);
const jsMs = Number(process.hrtime.bigint() - tj) / 1e6;
if (goldenOut) {
  writeFileSync(goldenOut, JSON.stringify({ about: 'manifold-3d 3.5.4 (wasm) results for tools/parity/manifold.mjs cases', cases: cases.map((c, i) => ({ ...c, want: want[i] })) }) + '\n');
  console.log(`wrote ${cases.length} cases to ${goldenOut}`);
}
const dir = mkdtempSync(join(tmpdir(), 'mf-'));
try {
  writeFileSync(join(dir, 'cases.json'), JSON.stringify({ cases }));
  const t0 = process.hrtime.bigint();
  const got = JSON.parse(execFileSync(cli, ['manifold', join(dir, 'cases.json')], { maxBuffer: 1 << 30 }).toString()).results;
  const ms = Number(process.hrtime.bigint() - t0) / 1e6;
  let same = 0, errors = 0;
  const bad = [];
  want.forEach((w, i) => {
    if (w.error) errors++;
    if (JSON.stringify(w) === JSON.stringify(got[i])) same++; else bad.push(i);
  });
  console.log(`manifold: ${same}/${cases.length} identical (${errors} refused alike or not), wasm ${jsMs.toFixed(0)} ms, C++ ${ms.toFixed(0)} ms for all (the C++ time includes the process and JSON)`);
  for (const i of bad.slice(0, detail ? 50 : 5)) console.log(`  differs: ${cases[i].name || 'seed ' + cases[i].seed}`, detail ? JSON.stringify({ want: want[i], got: got[i] }).slice(0, 2000) : '');
  process.exitCode = bad.length ? 1 : 0;
} finally { rmSync(dir, { recursive: true, force: true }); }
