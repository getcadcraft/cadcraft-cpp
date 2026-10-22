// SPDX-License-Identifier: Apache-2.0
// pcdiff/gen-kernel.mjs — M12: kernel.js's regenerate() on both sides (each with its own kernel): the Catalog flange,
// random parts, and parts whose fillets / chamfers / shells / drafts / threads name real faces and edges (taken from a
// first rebuild of the base part on the JS side at generation time), modelled threads, big patterns.
import * as D from './driver.mjs';
import { ri, pick, chance } from './gen-core.mjs';
import { randomPart, flange, blockSketch } from './gen-part.mjs';
import { freshKernel, regen } from './ops-kernel.mjs';

const { S, P } = D;
const round = (x, d = 3) => Math.round(x * 10 ** d) / 10 ** d;

/** A block (extrude), maybe with a round boss or a bore, as a base for edge features. */
function basePart(r) {
  const part = P.newPart('Base');
  part.resolution = pick(r, [32, 48, 64]);
  const w = round(40 + r() * 60, 1), h = round(20 + r() * 40, 1);
  const b = blockSketch(w, h);
  const sk1 = P.addFeature(part, 'sketch', { plane: { kind: 'base', name: pick(r, ['top', 'front', 'right']) }, sketch: b.sk });
  const ex = P.addFeature(part, 'extrude', { sketch: sk1.id, depth: round(8 + r() * 30, 2), end: pick(r, ['blind', 'mid']) });
  if (chance(r, 0.6)) {
    const cs = S.newSketch();
    const c = S.addCircle(cs, [round(w / 2, 2), round(h / 2, 2)], round(4 + r() * 6, 2));
    void c;
    const sk2 = P.addFeature(part, 'sketch', { plane: { kind: 'face', feature: ex.id, face: 'end' }, sketch: cs });
    P.addFeature(part, 'extrude', { sketch: sk2.id, depth: round(5 + r() * 15, 2), op: chance(r, 0.5) ? 'cut' : 'add', end: 'blind' });
  }
  return part;
}

function withEdgeFeature(r, part) {
  const rg = regen(part).r;
  if (!rg?.edges?.length || !rg?.faces?.length) return null;
  const p = structuredClone(part);
  const t = pick(r, ['fillet', 'chamfer', 'shell', 'draft', 'thread', 'fillet', 'chamfer']);
  const lineEdges = rg.edges.filter((e) => e.type === 'line');
  const planes = rg.faces.filter((f) => f.type === 'plane' && f.name);
  const cyls = rg.faces.filter((f) => f.type === 'cylinder' && f.name);
  const er = (e) => ({ faces: [...e.faces], at: e.at.map((v) => +v.toFixed(6)) });
  const fr = (f) => ({ name: f.name, at: f.at.map((v) => +v.toFixed(6)) });
  const props = {};
  if (t === 'fillet' || t === 'chamfer') {
    if (!lineEdges.length) return null;
    props.edges = Array.from({ length: ri(r, 1, Math.min(4, lineEdges.length)) }, () => er(pick(r, lineEdges)));
    if (t === 'fillet') props.r = round(0.5 + r() * 3, 2);
    else { props.d = round(0.5 + r() * 2.5, 2); if (chance(r, 0.4)) Object.assign(props, { mode: pick(r, ['two', 'angle']), d2: round(r() * 2 + 0.3, 2), angle: round(r() * 60 + 15, 1), flip: chance(r, 0.3) }); }
  } else if (t === 'shell') {
    if (!planes.length) return null;
    Object.assign(props, { faces: [fr(pick(r, planes))], t: round(0.8 + r() * 2, 2), outward: chance(r, 0.3) });
  } else if (t === 'draft') {
    if (!planes.length) return null;
    Object.assign(props, { faces: Array.from({ length: ri(r, 1, 2) }, () => fr(pick(r, planes))), angle: round((r() * 8 + 1) * (chance(r, 0.2) ? -1 : 1), 1), neutral: { kind: 'base', name: pick(r, ['top', 'front', 'right']) } });
  } else {
    if (!cyls.length) return null;
    Object.assign(props, { faces: [fr(pick(r, cyls))], form: 'iso', size: chance(r, 0.7) ? 'auto' : pick(r, ['M10', 'M8', 'M12']), length: chance(r, 0.5) ? null : round(r() * 10 + 2, 1), modelled: chance(r, 0.5), hand: pick(r, ['right', 'left']), flip: chance(r, 0.2) });
  }
  try { P.addFeature(p, t, props); } catch { return null; }
  return p;
}

/** A modelled-thread part (M10): a shaft or a bore with a modelled thread, or a modelled tapped hole. */
function threadPart(r) {
  const part = P.newPart('Thread');
  part.resolution = pick(r, [32, 48]);
  const k = r();
  if (k < 0.5) {
    const cs = S.newSketch();
    S.addCircle(cs, [0, 0], pick(r, [5, 4, 6]));
    const sk = P.addFeature(part, 'sketch', { plane: { kind: 'base', name: 'top' }, sketch: cs });
    P.addFeature(part, 'extrude', { sketch: sk.id, depth: round(10 + r() * 15, 1), end: 'blind' });
    const rg = regen(part).r;
    const cyl = rg?.faces?.find((f) => f.type === 'cylinder');
    if (!cyl) return null;
    P.addFeature(part, 'thread', { faces: [{ name: cyl.name, at: cyl.at.map((v) => +v.toFixed(6)) }], form: 'iso', size: 'auto', length: chance(r, 0.5) ? null : 8, modelled: true, hand: pick(r, ['right', 'left']) });
  } else {
    const b = blockSketch(40, 30);
    const sk1 = P.addFeature(part, 'sketch', { plane: { kind: 'base', name: 'front' }, sketch: b.sk });
    const ex = P.addFeature(part, 'extrude', { sketch: sk1.id, depth: 12, end: 'blind' });
    const hs = S.newSketch();
    for (let i = 0; i < ri(r, 1, 2); i++) S.addPoint(hs, 10 + 15 * i, 15);
    const sk2 = P.addFeature(part, 'sketch', { plane: { kind: 'face', feature: ex.id, face: 'end' }, sketch: hs });
    P.addFeature(part, 'hole', { sketch: sk2.id, kind: 'tapped', size: pick(r, ['M6', 'M8', 'M10']), d: 6, through: chance(r, 0.5), depth: 10, threadDepth: chance(r, 0.5) ? 8 : null, modelled: true, clearance: round(r() * 0.2, 2) });
  }
  return part;
}

/** A big pattern: a hole patterned many times along a line or round an axis. */
function patternPart(r) {
  const { part, ids } = flange({ holes: ri(r, 12, 40), resolution: pick(r, [32, 64]) });
  void ids;
  return part;
}

async function genKernel(r) {
  await freshKernel();
  const scripts = [];
  for (let s = 0; s < 4; s++) {
    const calls = [];
    for (let k = 0; k < 3; k++) {
      let part;
      const x = r();
      if (s === 0 && k === 0) part = flange({ holes: ri(r, 1, 8), resolution: pick(r, [32, 64]) }).part;
      else if (x < 0.35) part = randomPart(r);
      else if (x < 0.75) part = withEdgeFeature(r, basePart(r)) || basePart(r);
      else if (x < 0.9) part = threadPart(r) || basePart(r);
      else part = patternPart(r);
      calls.push({ op: 'kernel.regenerate', args: [part, chance(r, 0.15) ? ri(r, 0, part.features.length) : null] });
    }
    scripts.push({ calls });
  }
  return scripts;
}

// ---------------------------------------------------------------- meshSection, interference, motion check

const V = await import(D.DEV + 'vec.js');
const A = await import(D.DEV + 'assembly.js');
const rv3 = (r, s = 1) => [round((r() * 2 - 1) * s, 3), round((r() * 2 - 1) * s, 3), round((r() * 2 - 1) * s, 3)];
const K = (part, get) => ({ $k: part, get });

async function genSection(r) {
  await freshKernel();
  const scripts = [];
  for (let s = 0; s < 3; s++) {
    const calls = [];
    for (let k = 0; k < 3; k++) {
      const part = s === 0 && k === 0 ? flange({ holes: ri(r, 1, 8), resolution: 32 }).part : (chance(r, 0.5) ? randomPart(r) : basePart(r));
      const g = regen(part).r;
      if (!g?.mesh || g.mesh.indices.length / 3 > 15000) continue;
      for (let i = 0; i < 2; i++) {
        const sec = { of: pick(r, ['front', 'top', 'right', 'right', 'iso']), ...(chance(r, 0.4) ? { at: round((r() - 0.5) * 40, 2) } : {}) };
        calls.push({ op: 'draw.meshViewsSec', args: [K(part, 'mesh'), chance(r, 0.3) ? false : null, sec] });
      }
    }
    scripts.push({ calls });
  }
  return scripts;
}

function blockPart(r, tapped = false) {
  const bp = P.newPart('Block');
  const b = blockSketch(round(60 + r() * 40, 1), round(30 + r() * 30, 1));
  const s1 = P.addFeature(bp, 'sketch', { plane: { kind: 'base', name: 'front' }, sketch: b.sk });
  const e1 = P.addFeature(bp, 'extrude', { sketch: s1.id, depth: round(10 + r() * 15, 1) });
  const hs = S.newSketch();
  for (let i = 0; i < ri(r, 1, 3); i++) S.addPoint(hs, round(12 + 12 * i, 1), round(10 + r() * 10, 1));
  const s2 = P.addFeature(bp, 'sketch', { plane: { kind: 'face', feature: e1.id, face: 'end' }, sketch: hs });
  P.addFeature(bp, 'hole', tapped ? { sketch: s2.id, kind: 'tapped', size: pick(r, ['M6', 'M8']), d: 6, through: chance(r, 0.5), depth: 8 } : { sketch: s2.id, d: round(4 + r() * 6, 1), through: true });
  return bp;
}
function pinPart(r, d = null) {
  const pp = P.newPart('Pin');
  const sk = S.newSketch();
  S.addCircle(sk, [0, 0], d ? d / 2 : round(3 + r() * 5, 1));
  const s1 = P.addFeature(pp, 'sketch', { plane: { kind: 'base', name: 'front' }, sketch: sk });
  P.addFeature(pp, 'extrude', { sketch: s1.id, depth: round(20 + r() * 20, 1), end: chance(r, 0.5) ? 'mid' : 'blind' });
  return pp;
}

function endsOf(r, inst, part, b) {
  const cands = [];
  for (const [fid, list] of b?.candidates || []) for (const c of list) cands.push({ kind: 'face', feature: fid, face: c.ref.face, ...(c.ref.entity ? { entity: c.ref.entity } : {}) });
  const plane = cands.length ? pick(r, cands) : { kind: 'base', name: 'front' };
  const e = { inst, plane };
  if (chance(r, 0.3)) { const axes = A.axesOf(b, part); if (axes.length) e.axis = { feature: pick(r, axes).feature, index: 0 }; }
  return e;
}

/** A tapped block and a bolt (a pin with a cosmetic thread) placed in its hole: the thread zones classify the overlap. */
function boltAsm(r) {
  const size = pick(r, ['M6', 'M8', 'M10']);
  const major = { M6: 6, M8: 8, M10: 10 }[size];
  const bp = P.newPart('Tapped');
  const b = blockSketch(40, 30);
  const s1 = P.addFeature(bp, 'sketch', { plane: { kind: 'base', name: 'front' }, sketch: b.sk });
  const e1 = P.addFeature(bp, 'extrude', { sketch: s1.id, depth: 12 });
  const hs = S.newSketch();
  S.addPoint(hs, 20, 15);
  const s2 = P.addFeature(bp, 'sketch', { plane: { kind: 'face', feature: e1.id, face: 'end' }, sketch: hs });
  P.addFeature(bp, 'hole', { sketch: s2.id, kind: 'tapped', size, d: major, through: chance(r, 0.6), depth: 9, threadDepth: chance(r, 0.4) ? 6 : null });
  const pin = pinPart(r, major * pick(r, [1, 0.98]));
  if (chance(r, 0.7)) {
    const g = regen(pin).r;
    const cyl = g?.faces?.find((f) => f.type === 'cylinder');
    if (cyl) P.addFeature(pin, 'thread', { faces: [{ name: cyl.name, at: cyl.at.map((v) => +v.toFixed(6)) }], form: 'iso', size: 'auto', length: chance(r, 0.5) ? null : 10, modelled: false });
  }
  const asm = A.newAssembly('Bolt');
  const i1 = A.insertPart(asm, bp, { place: null });
  const i2 = A.insertPart(asm, pin, { place: V.translation([20 + pick(r, [0, 0, 0.3]), 15, pick(r, [-5, 2, 8])]) });
  return { asm, pids: [i1.part, i2.part] };
}

function makeAsm(r) {
  if (chance(r, 0.4)) return boltAsm(r);
  const parts = [blockPart(r, chance(r, 0.6)), pinPart(r, pick(r, [6, 8, null])), chance(r, 0.5) ? flange({ holes: ri(r, 2, 4), resolution: 32 }).part : blockPart(r)];
  const asm = A.newAssembly('Asm');
  const pids = [];
  for (const p of parts) {
    const inst = A.insertPart(asm, p, { place: chance(r, 0.6) ? V.translation(rv3(r, 15)) : null });
    pids.push(inst.part);
  }
  for (let i = 0; i < ri(r, 0, 2); i++) A.insertPart(asm, null, { pid: pick(r, pids), place: chance(r, 0.5) ? V.rotationAbout(rv3(r, 10), rv3(r), round(r() * 360, 1)) : V.translation(rv3(r, 10)) });
  return { asm, pids };
}

async function genClash(r) {
  await freshKernel();
  const scripts = [];
  for (let s = 0; s < 3; s++) {
    const calls = [];
    const { asm, pids } = makeAsm(r);
    const builds = Object.fromEntries(pids.map((pid) => [pid, K(asm.parts[pid].part, 'asmBuild')]));
    const world = Object.fromEntries(asm.instances.map((i) => [i.id, i.place]));
    calls.push({ op: 'asm.interference', args: [asm, builds, world, null] });
    calls.push({ op: 'asm.interference', args: [asm, builds, world, { touch: true, touchTol: pick(r, [1e-3, 0.5, 2]) }] });
    calls.push({ op: 'asm.interference', args: [asm, builds, world, { tol: pick(r, [1e-6, 1, 100]) }] });
    scripts.push({ calls });
  }
  return scripts;
}

async function genMotion(r) {
  await freshKernel();
  const scripts = [];
  for (let s = 0; s < 2; s++) {
    const calls = [];
    const { asm, pids } = makeAsm(r);
    const regs = new Map(pids.map((pid) => [pid, regen(asm.parts[pid].part).r]));
    for (let k = 0; k < ri(r, 1, 2); k++) {
      const ia = asm.instances[0], ib = pick(r, asm.instances.slice(1));
      const type = pick(r, ['revolute', 'slider', 'cylindrical', 'revolute']);
      const ea = endsOf(r, ia.id, asm.parts[ia.part].part, regs.get(ia.part)), eb = endsOf(r, ib.id, asm.parts[ib.part].part, regs.get(ib.part));
      try { const j = A.addJoint(asm, type, ea, eb, { angle: round(r() * 90, 1), offset: round(r() * 5, 2) }); if (chance(r, 0.4)) j.limits = { min: -10, max: 30 }; } catch { /* refused */ }
    }
    const builds = Object.fromEntries(pids.map((pid) => [pid, K(asm.parts[pid].part, 'asmBuild')]));
    for (const j of asm.joints) calls.push({ op: 'asm.motionCheck', args: [asm, builds, j.id, round(r() * -40, 1), round(r() * 60, 1), pick(r, [4, 6, 2.5, null]), null] });
    calls.push({ op: 'asm.motionCheck', args: [asm, builds, 'j99', 0, 1, 2, null] });
    scripts.push({ calls });
  }
  return scripts;
}

// ---------------------------------------------------------------- test-screw-threads.mjs on both kernels

/** The test's boss: a 40 × 40 × 10 block with a Ø dia × 20 boss on top, the thread (if any) on the boss's cylinder. */
function boss(thread = null, dia = 10) {
  const part = P.newPart('Boss'); const b = blockSketch(40, 40);
  const s1 = P.addFeature(part, 'sketch', { plane: { kind: 'base', name: 'top' }, sketch: b.sk });
  const e1 = P.addFeature(part, 'extrude', { sketch: s1.id, depth: 10 });
  const sk = S.newSketch(); S.addCircle(sk, [20, 20], dia / 2);
  const s2 = P.addFeature(part, 'sketch', { plane: { kind: 'face', feature: e1.id, face: 'end' }, sketch: sk });
  P.addFeature(part, 'extrude', { sketch: s2.id, depth: 20 });
  const r0 = regen(part).r;
  const cyl = r0.faces.find((f) => f.type === 'cylinder');
  if (thread) P.addFeature(part, 'thread', { faces: [{ name: cyl.name, at: cyl.at }], ...thread });
  return part;
}
function plate(hole) {
  const part = P.newPart('Plate'); const b = blockSketch(40, 30);
  const s1 = P.addFeature(part, 'sketch', { plane: { kind: 'base', name: 'top' }, sketch: b.sk });
  const e1 = P.addFeature(part, 'extrude', { sketch: s1.id, depth: 10 });
  const sk = S.newSketch(); S.addPoint(sk, 12, 15);
  const s2 = P.addFeature(part, 'sketch', { plane: { kind: 'face', feature: e1.id, face: 'end' }, sketch: sk });
  const h = P.addFeature(part, 'hole', { sketch: s2.id, kind: 'tapped', d: 8, size: 'M8', threadDepth: null, through: true, csinkD: 12, csinkAngle: 90, ...hole });
  return { part, h };
}

async function genThreads(r) {
  await freshKernel();
  const calls = (parts) => ({ calls: parts.map((p) => ({ op: 'kernel.regenerate', args: [p, null] })) });
  const scripts = [];
  // the M10 modelled boss (the test's own case) and its variants
  scripts.push(calls([boss({ modelled: true, clearance: 0.1 }), boss({}), boss({ modelled: true, clearance: 0.1, length: 8 }), boss({ modelled: true, clearance: 0.1, length: 8, flip: true })]));
  scripts.push(calls([boss({ modelled: true, clearance: 0.1, hand: 'left' }), boss({ modelled: true, length: 50 }), boss({ form: 'tr', modelled: true, clearance: 0 }, 16), boss({ size: 'M24', modelled: true }), boss({}, 0.6)]));
  const pat = plate({ modelled: true, clearance: 0.1 });
  P.addFeature(pat.part, 'lpattern', { seeds: [pat.h.id], dir: { kind: 'world', axis: 'x' }, count: 2, spacing: 16 });
  scripts.push(calls([plate({}).part, plate({ modelled: true, clearance: 0.1 }).part, plate({ through: false, depth: 8, threadDepth: 5, modelled: true, clearance: 0.1 }).part, pat.part]));
  // random sizes, clearances, resolutions
  const rnd = [];
  for (let k = 0; k < 3; k++) {
    const p = boss({ modelled: true, clearance: round(r() * 0.2, 2), hand: pick(r, ['right', 'left']), ...(chance(r, 0.4) ? { length: round(4 + r() * 12, 1) } : {}), ...(chance(r, 0.3) ? { flip: true } : {}) }, pick(r, [8, 10, 12, 6]));
    p.resolution = pick(r, [32, 48, 64]);
    rnd.push(p);
  }
  scripts.push(calls(rnd));
  return scripts;
}

export const GENS = { kernel: genKernel, ksection: genSection, kclash: genClash, kmotion: genMotion, kthread: genThreads };
