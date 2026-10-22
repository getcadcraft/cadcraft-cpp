// SPDX-License-Identifier: Apache-2.0
// pcdiff/ops-stage3.mjs — JS ops for cc-part stage 3 (M10.3): faces.js, meshfaces.js and refgeom.js, plus the JS mesh
// kernel (kernel.js on the manifold-3d 3.5.4 wasm the 0.5 build ships) to make the meshes they are tested on. A module
// load hook wraps meshfaces.js's namedFaces export at load time (the file on disk is untouched) so the generator can
// record exactly what the kernel hands it. Mirrored by crates/part/src/script3.rs.
import { register } from 'node:module';
import { DEV } from './driver.mjs';

const HOOK = `
export async function load(url, ctx, next) {
  const r = await next(url, ctx);
  if (/\\/partcraft\\/dev\\/meshfaces\\.js(\\?|$)/.test(url)) {
    let src = String(r.source);
    src = src.replace('export function namedFaces(', 'export function namedFaces(...__a) { if (globalThis.__pcNamed) globalThis.__pcNamed(__a); return __namedFaces0(...__a); }\\nfunction __namedFaces0(');
    return { ...r, source: src, shortCircuit: true };
  }
  return r;
}`;
register('data:text/javascript,' + encodeURIComponent(HOOK));

export const F = await import(DEV + 'faces.js');
export const MF = await import(DEV + 'meshfaces.js');
export const RG = await import(DEV + 'refgeom.js');
export const K = await import(DEV + 'kernel.js');
const WASM = (process.env.FORK_PARITY_JS || (process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles') + '/fork-parity/js-0.5') + '/vendor/manifold-3d/3.5.4/manifold.js';
export const M = await (await import(WASM)).default();
M.setup();

const meshOf = (m) => ({ positions: Float32Array.from(m.positions), indices: Uint32Array.from(m.indices), ...(m.triFace ? { triFace: Int32Array.from(m.triFace) } : {}) });
const tables = (res) => {
  const look = (tab, r) => { const k = JSON.stringify(r); if (!res?.[tab] || !Object.hasOwn(res[tab], k)) throw new Error('unresolved ' + k); return structuredClone(res[tab][k]); };
  return { plane: (r) => look('planes', r), dir: (r) => look('dirs', r), point: (r) => look('points', r), face: (r) => look('faces', r), edge: (r) => look('edges', r), fail: (m) => { throw new Error(m); } };
};

export const OPS = {
  'faces.planarGroupsMesh': (m, tol) => F.planarGroupsMesh(meshOf(m), tol ?? undefined),
  'faces.faceGroups': (m) => F.faceGroups(meshOf(m)),
  'faces.loops': (m) => { const mesh = meshOf(m); return F.planarGroups(mesh).groups.map((g) => { const r = F.faceLoops2D(mesh, g); return { raw: F.boundaryLoops(mesh, g.tris), frame: r.frame, loops: r.loops, area: r.area }; }); },
  'faces.dropCollinear': (l, tol) => F.dropCollinear(l, tol ?? undefined),
  'mf.namedFaces': (m, triTool, surfs, seg) => MF.namedFaces(meshOf(m), Int32Array.from(triTool), surfs, { segments: seg ?? undefined }),
  'mf.moveSurfs': (surfs, m4, suffix) => MF.moveSurfs(surfs, m4, suffix),
  'ref.feature': (f, res) => RG.refFeature(f, tables(res)),
  'ref.planeQuad': (g, s) => RG.planeQuad(g, s),
};
