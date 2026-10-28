#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Seeded random model scripts for tools/parity/m3d.mjs (M6.2): rectangles, polygons and circles on the ground and on
// faces, lines across faces, push/pull by any distance, moves, turns, copies, erasing, groups, explode, curves, offset,
// undo/redo inside change() steps — the edits a session makes, in random order, so the C++ kernel meets the arrangements
// the hand-written scripts do not.
//   node tools/parity/m3d-gen.mjs --seeds A-B --per N --out <file.json>
import { writeFileSync } from 'node:fs';

const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const [s0, s1] = opt('--seeds', '1-10').split('-').map(Number);
const per = Number(opt('--per', '10'));
const out = opt('--out', 'm3d-random.json');

function rng(seed) { let s = seed >>> 0 || 1; return () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s / 4294967296; }; }
const scripts = [];
for (let seed = s0; seed <= s1; seed++) for (let k = 0; k < per; k++) {
  const r = rng(seed * 7919 + k), pick = (a) => a[Math.floor(r() * a.length)];
  const q = (lo, hi, step = 0.25) => Math.round((lo + (hi - lo) * r()) / step) * step;   // on a grid: edges meet
  const P = () => [q(-3, 3), q(-3, 3), 0];
  const N = [[0, 0, 1], [0, 0, -1], [1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0]];
  const steps = [];
  const n = 4 + Math.floor(r() * 14);
  for (let i = 0; i < n; i++) {
    const kind = pick(['rect', 'rect', 'rect', 'push', 'push', 'push', 'edge', 'ngon', 'polygon', 'move', 'rotate', 'copy', 'erase', 'group', 'explode', 'undo', 'offset', 'arc', 'face-rect', 'soften', 'cleanup']);
    let s;
    if (kind === 'rect') s = { op: 'rect', p: [q(-3, 3), q(-3, 3), pick([0, 0, 0, q(0, 2)])], w: q(0.5, 4), h: q(0.5, 4) };
    else if (kind === 'face-rect') { const n0 = pick(N); s = { op: 'rect', p: [q(-2, 2), q(-2, 2), q(0, 2)], u: n0[2] ? [1, 0, 0] : [0, 0, 1], v: n0[2] ? [0, 1, 0] : n0[0] ? [0, 1, 0] : [1, 0, 0], w: q(0.25, 2), h: q(0.25, 2) }; }
    else if (kind === 'push') s = { op: 'pushPull', face: { facing: pick(N), nth: Math.floor(r() * 3) }, d: q(-3, 3) || 1 };
    else if (kind === 'edge') s = { op: 'edge', p: P(), q: P() };
    else if (kind === 'ngon') s = { op: 'ngon', c: P(), r: q(0.25, 2), sides: 3 + Math.floor(r() * 20), ...(r() < 0.5 ? { curve: 'circle' } : {}) };
    else if (kind === 'polygon') { const c = P(), m = 3 + Math.floor(r() * 5), pts = []; for (let j = 0; j < m; j++) { const a = (2 * Math.PI * j) / m; const rr = q(0.5, 2); pts.push([c[0] + rr * Math.cos(a), c[1] + rr * Math.sin(a), 0]); } s = { op: 'polygon', pts }; }
    else if (kind === 'move') s = { op: 'move', sel: { all: pick(['faces', 'edges', 'everything']) }, by: [q(-2, 2), q(-2, 2), q(-1, 1)] };
    else if (kind === 'rotate') s = { op: 'rotate', sel: { all: 'everything' }, axis: pick([[0, 0, 1], [1, 0, 0]]), deg: pick([90, 45, 30, 180]), about: P() };
    else if (kind === 'copy') s = { op: 'copy', sel: { all: 'everything' }, by: [q(-6, 6), q(-6, 6), 0] };
    else if (kind === 'erase') s = { op: 'erase', sel: [{ t: pick(['f', 'e']), id: { last: true } }] };
    else if (kind === 'group') s = { op: 'group', sel: { all: pick(['faces', 'everything']) }, kind: pick(['group', 'component']) };
    else if (kind === 'explode') s = { op: 'explode', inst: 1 + Math.floor(r() * 200) };
    else if (kind === 'undo') s = { op: pick(['undo', 'redo']) };
    else if (kind === 'offset') s = { op: 'offsetFace', face: { facing: pick(N) }, d: q(-1, 1) || 0.25 };
    else if (kind === 'arc') s = { op: 'arc', c: P(), r: q(0.5, 2), sweep: pick([90, 180, 270]), segs: 4 + Math.floor(r() * 12) };
    else if (kind === 'soften') s = { op: 'soften', edges: { all: true }, deg: pick([0, 20, 45, 90]) };
    else s = { op: 'cleanup' };
    // most edits as one undo step each, as the tools make them
    steps.push(s.op === 'undo' || s.op === 'redo' || r() < 0.3 ? s : { op: 'change', label: s.op, steps: [s] });
  }
  steps.push({ op: 'solidInfo' }, { op: 'solidProblems' });
  scripts.push({ name: `seed ${seed}.${k}`, units: 'm', steps });
}
writeFileSync(out, JSON.stringify({ about: 'random scripts (tools/parity/m3d-gen.mjs)', scripts }) + '\n');
console.log(`wrote ${scripts.length} scripts to ${out}`);
