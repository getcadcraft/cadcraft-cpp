// SPDX-License-Identifier: Apache-2.0
// pcdiff/ops-kernel.mjs — M12: the mesh kernel itself (kernel.js on the manifold-3d 3.5.4 wasm) as an op, and the
// "$k" / "$call" argument placeholders every generator uses so each side builds its meshes with its OWN kernel:
//   { "$k": <part>, "get": "<what>", "upTo": n? }   regenerate the part on this side, then take <what> of the result
//   { "$call": "<op>", "args": [...] }              run another op on this side and use its result
// A fresh wasm instance and a fresh kernel.js module (its Minkowski offset cache) per script, so mesh IDs start at 1
// on both sides (the Rust CLI runs one script per process). Mirrored by crates/part/src/script6.rs.
import { DEV, HOOKS } from './driver.mjs';
// first: its module hook wraps meshfaces.js's namedFaces (the "cap*" gets), and drawing.js / assembly.js below load
// meshfaces.js through kernel.js
import './ops-stage3.mjs';

const WASM = (process.env.FORK_PARITY_JS || (process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles') + '/fork-parity/js-0.5') + '/vendor/manifold-3d/3.5.4/manifold.js';
let M = null, K = null, gen = 0;
const regens = new Map();
export async function freshKernel() {
  M = await (await import(WASM)).default();
  M.setup();
  K = await import(DEV + 'kernel.js?pcdiff=' + gen++);
  regens.clear();
}
export const kernelM = () => M;
export const kernelK = () => K;
HOOKS.beforeScript.push(async (script) => {
  const t = JSON.stringify(script);
  if (t.includes('"$k"') || t.includes('"kernel.') || t.includes('"$call"') || t.includes('"mesh.')) await freshKernel();
});

/** The regenerate() result of a part on this side (one rebuild per distinct part per script, as on the Rust side). */
export function regen(part, upTo = null) {
  const key = JSON.stringify([part, upTo]);
  if (!regens.has(key)) {
    const p = structuredClone(part);
    let r = null, cap = null;
    globalThis.__pcNamed = (a) => { cap = a; };
    try { r = K.regenerate(p, M, upTo == null ? {} : { upTo }); } catch (e) { r = { thrown: String(e && e.message) }; }
    globalThis.__pcNamed = null;
    regens.set(key, { r, p, cap });
  }
  return regens.get(key);
}

const arr = (a) => (a ? Array.from(a) : a);
/** What a "$k" placeholder takes from a rebuild. */
export function pick(rg, get) {
  const { r, p } = rg;
  const m = r?.mesh;
  switch (get) {
    case 'regen': return { ...r, ...(r?.mesh ? { mesh: Object.fromEntries(Object.entries(r.mesh).map(([k, v]) => [k, ArrayBuffer.isView(v) ? arr(v) : v])) } : {}), part: p };
    case 'mesh': return m ? { positions: arr(m.positions), indices: arr(m.indices) } : null;
    case 'meshTri': return m ? { positions: arr(m.positions), indices: arr(m.indices), ...(m.triFace ? { triFace: arr(m.triFace) } : {}) } : null;
    case 'meshFull': return m ? { positions: arr(m.positions), indices: arr(m.indices), ...(m.triFace ? { triFace: arr(m.triFace) } : {}), ...(m.edgeSegs ? { edgeSegs: arr(m.edgeSegs), segEdge: arr(m.segEdge) } : {}) } : null;
    case 'faces': return r?.faces ?? null;
    case 'edges': return r?.edges ?? null;
    case 'stats': return r?.stats ?? null;
    case 'named': return !!r?.named;
    case 'threads': return Object.fromEntries([...(r?.threads || new Map())]);
    case 'build': return r?.results ? { results: Object.fromEntries(r.results), candidates: Object.fromEntries(r.candidates), axes: Object.fromEntries(r.axes), holeAxes: Object.fromEntries(r.holeAxes), refGeom: Object.fromEntries(r.refGeom) } : null;
    case 'part': return p;
    case 'capMesh': return rg.cap ? { positions: arr(rg.cap[0].positions), indices: arr(rg.cap[0].indices) } : null;
    case 'capTriTool': return rg.cap ? arr(rg.cap[1]) : null;
    case 'capSurfs': return rg.cap ? rg.cap[2] : null;
    case 'capSeg': return rg.cap ? rg.cap[3]?.segments ?? null : null;
    case 'drawG': return { named: !!r?.named, faces: r?.faces ?? null, threads: Object.fromEntries([...(r?.threads || new Map())].map(([k, th]) => [k, { callout: th.callout }])) };
    case 'ifcG': return { mesh: pick(rg, 'mesh'), stats: r?.stats ?? null };
    case 'measureB': return { exact: false, named: !!r?.named, edges: r?.edges || [], faces: r?.faces || [], stats: r?.stats ?? null, mesh: pick(rg, 'meshFull') };
    case 'asmBuild': return r?.results ? { ...pick(rg, 'build'), mesh: pick(rg, 'mesh'), threads: pick(rg, 'threads') } : { mesh: null };
    default: throw new Error('unknown $k get ' + get);
  }
}

/** Replace every placeholder in an argument list (depth-first, left to right). */
async function resolveArgs(v) {
  if (Array.isArray(v)) { const out = []; for (const x of v) out.push(await resolveArgs(x)); return out; }
  if (v && typeof v === 'object') {
    if (Object.hasOwn(v, '$k')) return pick(regen(await resolveArgs(v.$k), v.upTo ?? null), v.get);
    if (Object.hasOwn(v, '$call')) { const { callOp } = await import('./driver.mjs'); return await callOp(v.$call, await resolveArgs(v.args || [])); }
    const out = {};
    for (const [k, x] of Object.entries(v)) out[k] = await resolveArgs(x);
    return out;
  }
  return v;
}
HOOKS.resolve = resolveArgs;

const DR = await import(DEV + 'drawing.js');
const A = await import(DEV + 'assembly.js');
const map = (o) => new Map(Object.entries(o || {}));
const meshOf = (m) => ({ positions: Float32Array.from(m.positions), indices: Uint32Array.from(m.indices) });
/** A build for placement + interference: the regen tables as Maps, the mesh, the threads Map. */
const asmBuild = (b) => (b ? { results: map(b.results), candidates: map(b.candidates), axes: map(b.axes), holeAxes: map(b.holeAxes), refGeom: map(b.refGeom), exact: false, mesh: b.mesh || null, threads: map(b.threads) } : undefined);
const asmBuilds = (o) => new Map(Object.entries(o || {}).map(([k, b]) => [k, asmBuild(b)]));

export const OPS = {
  'draw.meshViewsSec': (m, iso, section) => DR.meshViews(meshOf(m), { iso: iso ?? undefined, M, section: section ?? null }),
  'asm.interference': (asm, builds, world, opts) => A.interference(asm, asmBuilds(builds), map(world), M, opts || {}),
  'asm.motionCheck': (asm, builds, jointId, from, to, steps, key) => A.motionCheck(structuredClone(asm), asmBuilds(builds), M, jointId, from, to, steps ?? undefined, key ?? null),
  'kernel.regenerate': (part, upTo) => pick(regen(part, upTo ?? null), 'regen'),
  'util.set': (o, k, v) => ({ ...o, [k]: v }),
  'util.get': (o, k) => (o == null ? null : o[k] ?? null),
  'util.path': (o, path) => { let c = o; for (const k of path) c = c == null ? null : c[k] ?? null; return c; },
  'util.replaceAt': (a, swaps) => a.map((x, i) => (Object.hasOwn(swaps, String(i)) ? swaps[i] : x)),
  'util.json': (o) => (o === undefined ? null : JSON.parse(JSON.stringify(o))),
  'util.add3': (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]],
};
