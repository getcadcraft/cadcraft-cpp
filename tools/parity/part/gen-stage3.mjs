// SPDX-License-Identifier: Apache-2.0
// pcdiff/gen-stage3.mjs — stage-3 generators: the Catalog flange and random parts are rebuilt by EACH side's own mesh
// kernel (M12: "$k" placeholders, ops-kernel.mjs / script6.rs); what that kernel hands meshfaces.js's namedFaces
// (welded mesh, triangle → tool, tool surfaces, segments) is replayed in both, then the planar faces / boundary loops of
// the same meshes, moved surface lists, small hand-made meshes (open, non-manifold, needles), and reference geometry
// features against resolver tables (refgeom.js). The JS rebuild at generation time only decides which calls to make.
import * as D from './driver.mjs';
import { ri, pick, chance } from './gen-core.mjs';
import { randomPart, flange } from './gen-part.mjs';
import { K, M } from './ops-stage3.mjs';
import { freshKernel } from './ops-kernel.mjs';

const V = await import(D.DEV + 'vec.js');
const round = (x, d = 3) => Math.round(x * 10 ** d) / 10 ** d;
const rv3 = (r, s = 1) => [round((r() * 2 - 1) * s, 3), round((r() * 2 - 1) * s, 3), round((r() * 2 - 1) * s, 3)];

function captureRegen(part) {
  let cap = null;
  globalThis.__pcNamed = (a) => { cap = a; };
  let regen = null;
  try { regen = K.regenerate(part, M); } catch { regen = null; }
  globalThis.__pcNamed = null;
  return { cap, regen };
}

function meshCalls(r, calls, cap, regen, part) {
  // every mesh, triangle list and surface list is this side's own kernel's (placeholders); the JS capture only sizes
  const K3 = (get) => ({ $k: part, get });
  const mesh = K3('capMesh'), triTool = K3('capTriTool'), surfs = K3('capSurfs'), seg = K3('capSeg');
  calls.push({ op: 'mf.namedFaces', args: [mesh, triTool, surfs, seg] });
  if (chance(r, 0.5)) calls.push({ op: 'mf.namedFaces', args: [mesh, triTool, surfs, pick(r, [8, 3, 200, 17])] });
  if (chance(r, 0.4)) {
    const swaps = {};
    for (let i = 0; i < cap[1].length; i++) if (chance(r, 0.1)) swaps[i] = pick(r, [-1, 99]);
    calls.push({ op: 'mf.namedFaces', args: [mesh, { $call: 'util.replaceAt', args: [triTool, swaps] }, surfs, seg] });
  }
  const nt = cap[0].indices.length / 3;
  if (nt < 30000) {
    calls.push({ op: 'faces.planarGroupsMesh', args: [mesh, null] });
    if (chance(r, 0.3)) calls.push({ op: 'faces.planarGroupsMesh', args: [mesh, pick(r, [1e-3, 1e-8, 0.1])] });
    const withTri = { $call: 'util.set', args: [mesh, 'triFace', { $call: 'util.path', args: [K3('regen'), ['mesh', 'triFace']] }] };
    if (regen?.mesh?.triFace) calls.push({ op: 'faces.faceGroups', args: [withTri] });
    calls.push({ op: 'faces.loops', args: [mesh] });
    if (regen?.mesh?.triFace && chance(r, 0.5)) calls.push({ op: 'faces.loops', args: [withTri] });
  }
  for (let i = 0; i < Math.min(3, cap[2].length); i++) {
    const m4 = pick(r, [V.rotationAbout(rv3(r, 50), rv3(r), round(r() * 360, 2)), V.reflection(rv3(r, 20), rv3(r)), V.translation(rv3(r, 30))]);
    calls.push({ op: 'mf.moveSurfs', args: [{ $call: 'util.get', args: [surfs, i] }, m4, pick(r, ['@f4#1', '', '~f2'])] });
  }
}

async function genKernelMeshes(r, seed) {
  await freshKernel();
  const scripts = [];
  for (let s = 0; s < 3; s++) {
    const calls = [];
    for (let k = 0; k < 3; k++) {
      const part = s === 0 && k === 0 ? flange({ holes: ri(r, 1, 8), resolution: pick(r, [32, 64]) }).part : randomPart(r);
      const { cap, regen } = captureRegen(part);
      if (cap) meshCalls(r, calls, cap, regen, part);
    }
    calls.push({ op: 'mf.moveSurfs', args: [null, V.translation([1, 2, 3]), 'x'] });
    calls.push({ op: 'mf.moveSurfs', args: [[{ name: '', type: 'plane', n: [0, 0, 1], o: [0, 0, 0] }, { type: 'sphere', c: [1, 1, 1], r: 2 }, { name: 'a', type: 'other', within: { o: [0, 0, 0], d: [0, 0, 1], r: 3 }, span: { a: [0, 0, 0], d: [1, 0, 0], len: 4 } }], V.reflection([0, 0, 0], [1, 1, 0]), '@m'] });
    scripts.push({ calls });
  }
  void seed;
  return scripts;
}

/** Small hand-made meshes: a box, a box with a slit, an open strip, a fan with needles, a non-manifold pair. */
function smallMeshes(r) {
  const box = (x0, y0, z0, x1, y1, z1) => {
    const P = [[x0, y0, z0], [x1, y0, z0], [x1, y1, z0], [x0, y1, z0], [x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1]];
    const I = [0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7];
    return { positions: P.flat(), indices: I };
  };
  const out = [box(0, 0, 0, 10, 5, 2), box(-1e5, 3, 0.5, 1e5 + 0.25, 7, 3)];
  // a grid plate split into many coplanar triangles with a hole
  const n = ri(r, 3, 7), P = [], I = [];
  for (let j = 0; j <= n; j++) for (let i = 0; i <= n; i++) P.push(i * 2, j * 2, chance(r, 0.1) ? 1e-7 : 0);
  for (let j = 0; j < n; j++) for (let i = 0; i < n; i++) {
    if (i === 1 && j === 1) continue;
    const a = j * (n + 1) + i, b = a + 1, c = a + n + 1, d = c + 1;
    I.push(a, b, d, a, d, c);
  }
  out.push({ positions: P, indices: I });
  // needles and duplicates
  out.push({ positions: [0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 2, 2, 0], indices: [0, 1, 3, 1, 2, 3, 0, 1, 3, 1, 4, 3] });
  return out;
}

async function genFacesSmall(r) {
  const calls = [];
  for (const m of smallMeshes(r)) {
    calls.push({ op: 'faces.planarGroupsMesh', args: [m, null] }, { op: 'faces.loops', args: [m] });
    const nt = m.indices.length / 3;
    calls.push({ op: 'faces.faceGroups', args: [{ ...m, triFace: Array.from({ length: nt }, () => ri(r, 0, 2)) }] });
    const surfs = [[{ name: 'f1.start', type: 'plane', n: [0, 0, -1], o: [0, 0, 0] }, { name: 'f1.end', type: 'plane', n: [0, 0, 1], o: [0, 0, 2] }, { name: 'f1.side:l1', type: 'plane', n: [0, -1, 0], o: [0, 0, 0] }, { name: 'h', type: 'cylinder', axis: { o: [5, 2.5, 0], d: [0, 0, 1] }, r: 2 }]];
    calls.push({ op: 'mf.namedFaces', args: [m, Array.from({ length: nt }, () => (chance(r, 0.8) ? 0 : -1)), surfs, 64] });
  }
  for (let i = 0; i < 20; i++) {
    const l = Array.from({ length: ri(r, 2, 8) }, () => [round(r() * 10, 1), round(r() * 10, 1)]);
    if (chance(r, 0.5)) l.splice(1, 0, [(l[0][0] + l[1 % l.length][0]) / 2, (l[0][1] + l[1 % l.length][1]) / 2]);
    calls.push({ op: 'faces.dropCollinear', args: [l, chance(r, 0.3) ? 1e-2 : null] });
  }
  return [{ calls }];
}

async function genRefgeom(r) {
  const calls = [];
  const planeRefs = [{ kind: 'base', name: 'top' }, { kind: 'base', name: 'front' }, { kind: 'face', feature: 'f2', face: 'end' }, { kind: 'ref', feature: 'f5' }, { kind: 'face', feature: 'f9', face: 'start' }];
  const pointRefs = [{ kind: 'vertex', at: 1 }, { kind: 'vertex', at: 2 }, { kind: 'vertex', at: 3 }, { kind: 'sketch', id: 'p1' }, { kind: 'missing' }];
  const faceRefs = [{ name: 'f2.end' }, { name: 'f3.wall:p1' }, { name: 'f4.cone' }, { name: 'gone' }];
  const edgeRefs = [{ faces: ['a', 'b'] }, { faces: ['c', 'd'] }, { faces: ['e', 'f'] }, { faces: ['z', 'z'] }];
  const dirRefs = [{ kind: 'world', axis: 'x' }, { kind: 'axis', feature: 'f6' }, { kind: 'line', sketch: 'f1', entity: 'l1' }];
  for (let s = 0; s < 40; s++) {
    const n1 = rv3(r), n2 = chance(r, 0.2) ? [...n1] : rv3(r), n3 = chance(r, 0.15) ? V.add(n1, n2) : rv3(r);
    const res = { planes: {}, points: {}, faces: {}, edges: {}, dirs: {} };
    const frame = (n, o) => ({ ...V.frameFor(n, o), ...(chance(r, 0.5) ? { center: rv3(r, 10) } : {}) });
    const ps = [frame(n1, rv3(r, 10)), frame(n2, rv3(r, 10)), frame(n3, rv3(r, 10)), frame([0, 1, 0], [0, 0, 0])];
    planeRefs.forEach((p, i) => { if (i < 4 || chance(r, 0.3)) res.planes[JSON.stringify(p)] = ps[i % 4]; });
    const a = rv3(r, 10), b = chance(r, 0.15) ? [...a] : rv3(r, 10), c = chance(r, 0.15) ? V.add(a, V.mul(V.sub(b, a), 2)) : rv3(r, 10);
    [a, b, c, rv3(r, 5)].forEach((p, i) => { res.points[JSON.stringify(pointRefs[i])] = p; });
    res.faces[JSON.stringify(faceRefs[0])] = { type: 'plane', n: V.norm(n1), o: rv3(r, 5) };
    res.faces[JSON.stringify(faceRefs[1])] = { type: 'cylinder', axis: { o: rv3(r, 5), d: V.norm(rv3(r)) }, r: round(1 + r() * 5, 2) };
    res.faces[JSON.stringify(faceRefs[2])] = chance(r, 0.5) ? { type: 'cone', axis: { o: rv3(r, 5), d: V.norm(rv3(r)) }, r: 2 } : { type: 'other' };
    res.edges[JSON.stringify(edgeRefs[0])] = { type: 'line', a: rv3(r, 10), b: chance(r, 0.1) ? [0, 0, 0] : rv3(r, 10) };
    res.edges[JSON.stringify(edgeRefs[1])] = { type: 'circle', a: rv3(r, 10), b: rv3(r, 10), ...(chance(r, 0.8) ? { axis: { o: rv3(r, 5), d: V.norm(rv3(r)) } } : {}) };
    res.edges[JSON.stringify(edgeRefs[2])] = { type: 'curve', a: rv3(r, 10), b: rv3(r, 10) };
    res.dirs[JSON.stringify(dirRefs[0])] = { o: [0, 0, 0], d: [1, 0, 0] };
    res.dirs[JSON.stringify(dirRefs[1])] = { o: rv3(r, 5), d: chance(r, 0.1) ? V.norm(n1) : rv3(r) };
    const type = pick(r, ['point', 'axis', 'plane', 'plane', 'plane']);
    const modes = { point: ['xyz', 'at', 'mid', 'meet', 'planes3', 'centre', 'nope'], axis: ['2pt', 'cyl', 'planes', 'edge', 'normal', 'what'], plane: ['offset', 'angle', '3pt', 'mid', 'tangent', 'edgept', 'midnormal', 'normal', 'odd'] }[type];
    for (let k = 0; k < 4; k++) {
      const f = { id: 'f7', type, mode: pick(r, modes) };
      if (chance(r, 0.8)) f.points = Array.from({ length: ri(r, 0, 3) }, () => pick(r, pointRefs));
      if (chance(r, 0.8)) f.planes = Array.from({ length: ri(r, 0, 3) }, () => pick(r, planeRefs));
      if (chance(r, 0.5)) f.face = pick(r, faceRefs);
      if (chance(r, 0.5)) f.edge = pick(r, edgeRefs);
      if (chance(r, 0.4)) f.axis = pick(r, dirRefs);
      if (chance(r, 0.3)) f.flip = true;
      if (f.mode === 'xyz') f.p = rv3(r, 20);
      if (chance(r, 0.7)) f.d = round(r() * 40 - 10, 2);
      if (chance(r, 0.7)) f.angle = round(r() * 360 - 90, 2);
      calls.push({ op: 'ref.feature', args: [f, res] });
      let g = null;
      try { g = D.EXTOPS['ref.feature'](f, res); } catch { g = null; }
      if (g && g.kind === 'plane' && !JSON.stringify(g).includes('null')) calls.push({ op: 'ref.planeQuad', args: [g, pick(r, [10, 25.5, 0])] });
    }
  }
  return [{ calls }];
}

export const GENS = { kmesh: genKernelMeshes, facesmall: genFacesSmall, refgeom: genRefgeom };
