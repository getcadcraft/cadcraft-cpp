// SPDX-License-Identifier: Apache-2.0
// pcdiff/gen-stage4.mjs — stage-4 generators: assemblies of real parts built by the JS mesh kernel (the frozen
// "two flanges face to face" section as written, then random assemblies: joints of every type on the parts' face
// candidates and axes, limits, loops, free parts moved by hand, files written / read / tampered, interference filters,
// exploded views), and the kernel's planeFrame / faceRefAt on the same builds. M12: every build, placement and joint tree
// in the calls comes from EACH side's own kernel ("$k" / "$call" placeholders); the JS rebuild here only picks calls.
import * as D from './driver.mjs';
import { ri, pick, chance } from './gen-core.mjs';
import { flange, blockSketch } from './gen-part.mjs';
import { K, M } from './ops-stage3.mjs';
import { A, X } from './ops-stage4.mjs';
import { freshKernel } from './ops-kernel.mjs';

const KB = (part) => ({ $k: part, get: 'build' });
const worldOf = (asm, b) => ({ $call: 'util.get', args: [{ $call: 'asm.placeAll', args: [asm, b] }, 'world'] });

const { S, P } = D;
const V = await import(D.DEV + 'vec.js');
const round = (x, d = 3) => Math.round(x * 10 ** d) / 10 ** d;
const rv3 = (r, s = 1) => [round((r() * 2 - 1) * s, 3), round((r() * 2 - 1) * s, 3), round((r() * 2 - 1) * s, 3)];
const clone = (v) => structuredClone(v);

function blockPart(r) {
  const bp = P.newPart('Block');
  const b = blockSketch(round(60 + r() * 60, 1), round(30 + r() * 40, 1));
  const s1 = P.addFeature(bp, 'sketch', { plane: { kind: 'base', name: pick(r, ['top', 'front']) }, sketch: b.sk });
  const e1 = P.addFeature(bp, 'extrude', { sketch: s1.id, depth: round(10 + r() * 20, 1) });
  const hs = S.newSketch();
  for (let i = 0; i < ri(r, 1, 3); i++) S.addPoint(hs, round(10 + r() * 30, 1), round(8 + r() * 15, 1));
  const s2 = P.addFeature(bp, 'sketch', { plane: { kind: 'face', feature: e1.id, face: 'end' }, sketch: hs });
  P.addFeature(bp, 'hole', { sketch: s2.id, d: round(4 + r() * 6, 1), through: true });
  return bp;
}
function pinPart(r) {
  const pp = P.newPart('Pin');
  const sk = S.newSketch();
  S.addCircle(sk, [0, 0], round(3 + r() * 5, 1));
  const s1 = P.addFeature(pp, 'sketch', { plane: { kind: 'base', name: 'top' }, sketch: sk });
  P.addFeature(pp, 'extrude', { sketch: s1.id, depth: round(20 + r() * 30, 1) });
  return pp;
}
const regen = (part) => { try { return K.regenerate(part, M); } catch { return null; } };

function endsOf(r, inst, part, b) {
  const cands = [];
  for (const [fid, list] of b?.candidates || []) for (const c of list) cands.push({ kind: 'face', feature: fid, face: c.ref.face, ...(c.ref.entity ? { entity: c.ref.entity } : {}) });
  const extra = [{ kind: 'base', name: pick(r, ['top', 'front', 'right']) }, { kind: 'fixed', o: rv3(r, 20), n: rv3(r) }, { kind: 'face', feature: 'f9', face: 'end' }, { kind: 'face', feature: 'f1', face: 'named', name: 'f1.end' }, { kind: 'ref', feature: 'f3' }];
  const plane = chance(r, 0.92) && cands.length ? pick(r, cands) : pick(r, extra);
  const e = { inst, plane };
  if (chance(r, 0.3)) {
    const axes = A.axesOf(b, part);
    e.axis = axes.length && chance(r, 0.85) ? { feature: pick(r, axes).feature, ...(chance(r, 0.6) ? { index: 0 } : {}) } : { feature: 'f8' };
  }
  return e;
}

async function genFlanges() {
  await freshKernel();
  const calls = [];
  const fl = flange();
  let asm = A.newAssembly('Flange joint');
  const i1 = A.insertPart(asm, fl.part);
  const i2 = A.insertPart(asm, fl.part, { pid: i1.part, place: V.translation([300, 0, 0]) });
  const b = { [i1.part]: KB(asm.parts[i1.part].part) };
  const bottom = { kind: 'face', feature: fl.ids.revolve, face: 'side', entity: fl.lines[0] };
  const top = { kind: 'face', feature: fl.ids.revolve, face: 'side', entity: fl.lines[2] };
  calls.push({ op: 'asm.placeAll', args: [clone(asm), b] });
  const j = A.addJoint(asm, 'revolute', { inst: i1.id, plane: bottom }, { inst: i2.id, plane: bottom });
  calls.push({ op: 'asm.placeAll', args: [clone(asm), b] });
  for (const [k, v] of [['offset', -2], ['angle', 22.5], ['offset', 0]]) { j[k] = v; calls.push({ op: 'asm.placeAll', args: [clone(asm), b] }); }
  j.type = 'slider'; j.offset = 5;
  calls.push({ op: 'asm.placeAll', args: [clone(asm), b] });
  calls.push({ op: 'asm.insertPart', args: [clone(asm), null, null, i1.part] });
  const i3 = A.insertPart(asm, fl.part, { pid: i1.part });
  calls.push({ op: 'asm.addJoint', args: [clone(asm), 'rigid', { inst: i2.id, plane: top, axis: { feature: fl.ids.hole, index: 0 } }, { inst: i3.id, plane: bottom }, {}] });
  A.addJoint(asm, 'rigid', { inst: i2.id, plane: top, axis: { feature: fl.ids.hole, index: 0 } }, { inst: i3.id, plane: bottom });
  calls.push({ op: 'asm.placeAll', args: [clone(asm), b] });
  A.addJoint(asm, 'rigid', { inst: i1.id, plane: bottom }, { inst: i3.id, plane: bottom });
  calls.push({ op: 'asm.placeAll', args: [clone(asm), b] });
  const text = A.toJSON(asm);
  calls.push({ op: 'asm.toJSON', args: [clone(asm)] }, { op: 'asm.read', args: [text] });
  const t = JSON.parse(text); t.joints[0].b.inst = 'i999';
  const t2 = JSON.parse(text); t2.version = 9;
  calls.push({ op: 'asm.read', args: [JSON.stringify(t)] }, { op: 'asm.read', args: [JSON.stringify(t2)] });
  calls.push({ op: 'asm.addJoint', args: [clone(asm), 'revolute', { inst: i1.id, plane: bottom }, { inst: i1.id, plane: top }, {}] });
  asm = A.readAssembly(text);
  calls.push({ op: 'asm.jointTree', args: [clone(asm), b, worldOf(clone(asm), b)] }, { op: 'asm.freeIds', args: [clone(asm)] });
  return [{ calls }];
}

function mutateAsm(r, j) {
  const t = clone(j);
  const k = r();
  if (k < 0.1) t.instances[0].place[3] = 'x';
  else if (k < 0.2) t.joints.push({ ...clone(t.joints[0] || {}), id: t.joints[0]?.id || 'j1' });
  else if (k < 0.3) t.parts = [];
  else if (k < 0.4) { if (t.joints[0]) t.joints[0].limits = pick(r, [{ min: 5, max: 1 }, { min: -5000, max: 1 }, { nope: { min: 0, max: 1 } }, { angle: { min: 0, max: 10 } }, null, 3]); }
  else if (k < 0.5) t.clashFilter = pick(r, [{ hide: ['touch', 'overlap', 'touch'], pairs: [['i2', 'i3'], ['i3', 'i2'], ['i9', 'i2']] }, { hide: ['x'] }, { pairs: [['i2', 'i2']] }, [], { hide: [] }]);
  else if (k < 0.6) t.views = pick(r, [[{ id: 'v90', name: '\u0001 Exploded ', steps: [{ id: 's91', insts: [t.instances[0].id, t.instances[0].id], dir: [0, 2, 0], dist: 30 }] }], [{ id: 'v90', steps: [{ id: 's91', insts: ['i999'], dir: [0, 1, 0], dist: 1 }] }], 'x', [{ steps: [] }]]);
  else if (k < 0.7) { if (t.joints[0]) t.joints[0].type = pick(r, ['ball', 'weld', 'pinslot']); }
  else if (k < 0.8) { if (t.joints[0]) t.joints[0].a.axis = pick(r, [{ feature: 'f2', index: -1 }, { feature: 'x' }, { feature: 'f2', index: null }, { feature: 'f2', index: 3 }]); }
  else if (k < 0.9) t.instances.push({ id: 'i77', part: 'p99', name: 'x', place: t.instances[0].place });
  else { t.version = pick(r, [1, 4, 5, 0, '2']); t.name = pick(r, ['', '\u0002x', 'y'.repeat(130)]); }
  return JSON.stringify(t);
}

async function genAssemblies(r) {
  await freshKernel();
  const scripts = [];
  for (let s = 0; s < 3; s++) {
    const calls = [];
    const parts = [flange({ holes: ri(r, 2, 6), resolution: 32 }).part, blockPart(r), pinPart(r)];
    let asm = A.newAssembly(pick(r, ['Asm', 'Gearbox', undefined]));
    const pids = [];
    for (const p of parts) {
      calls.push({ op: 'asm.insertPart', args: [clone(asm), p, chance(r, 0.5) ? V.translation(rv3(r, 100)) : null, null] });
      const inst = A.insertPart(asm, p, { place: chance(r, 0.5) ? V.translation(rv3(r, 100)) : null });
      pids.push(inst.part);
    }
    for (let i = 0; i < ri(r, 1, 3); i++) A.insertPart(asm, null, { pid: pick(r, pids), place: V.rotationAbout(rv3(r, 50), rv3(r), round(r() * 360, 1)) });
    const regs = new Map(pids.map((pid) => [pid, regen(asm.parts[pid].part)]));
    const b = Object.fromEntries([...regs].filter(([, g]) => g).map(([pid]) => [pid, KB(asm.parts[pid].part)]));
    const instOf = (id) => asm.instances.find((x) => x.id === id);
    for (let k = 0; k < ri(r, 4, 10); k++) {
      const ia = pick(r, asm.instances), others = asm.instances.filter((x) => x !== ia), ib = chance(r, 0.9) && others.length ? pick(r, others) : ia;
      const type = pick(r, Object.keys(A.JOINT_TYPES).concat(['weld']));
      const ea = endsOf(r, ia.id, asm.parts[ia.part].part, regs.get(ia.part)), eb = endsOf(r, ib.id, asm.parts[ib.part].part, regs.get(ib.part));
      const more = { angle: round(r() * 180 - 90, 1), offset: round(r() * 10 - 5, 2), ...(chance(r, 0.5) ? { dx: round(r() * 20, 1), dy: round(r() * 20, 1), tiltX: round(r() * 30, 1), tiltY: round(r() * 30, 1), slide: round(r() * 10, 1), slotDir: round(r() * 360, 1) } : {}), ...(chance(r, 0.2) ? { slide: NaN } : {}) };
      calls.push({ op: 'asm.addJoint', args: [clone(asm), type, ea, eb, more] });
      try { const j = A.addJoint(asm, type, ea, eb, more); if (chance(r, 0.4)) j.limits = pick(r, [{ min: -10, max: 10 }, { angle: { min: 0, max: 45 }, offset: { min: -1, max: 1 } }, { dx: { min: 0, max: 3 } }]); } catch { /* refused */ }
    }
    calls.push({ op: 'asm.placeAll', args: [clone(asm), b] });
    const pr = A.placeAll(asm, new Map([...regs].filter(([, g]) => g)));
    const world = worldOf(clone(asm), b);
    calls.push({ op: 'asm.jointTree', args: [clone(asm), b, world] }, { op: 'asm.freeIds', args: [clone(asm)] });
    for (const j of asm.joints) {
      calls.push({ op: 'asm.jointMotion', args: [j] }, { op: 'asm.limitOf', args: [j, null] }, { op: 'asm.jointValue', args: [j, null] });
      for (const key of ['angle', 'offset', 'dx', 'tiltY', 'slide', 'nope']) calls.push({ op: 'asm.limitOf', args: [j, key] }, { op: 'asm.jointValue', args: [j, key] });
      calls.push({ op: 'asm.readLimits', args: [pick(r, [{ min: -1, max: 1 }, { angle: { min: 0, max: 4000 } }, { offset: { min: 2, max: 1 } }, {}, { min: 0 }, 'x', { tiltX: { min: -5, max: 5 } }]), j.type] });
      const ia = instOf(j.a.inst);
      if (ia) {
        calls.push({ op: 'asm.mateFrame', args: [j.a, b[ia.part] ?? null, asm.parts[ia.part].part] });
        calls.push({ op: 'asm.planeFrame', args: [j.a.plane, b[ia.part] ?? null, asm.parts[ia.part].part] });
      }
    }
    for (const pid of pids) {
      if (!b[pid]) continue;
      calls.push({ op: 'asm.axesOf', args: [b[pid], asm.parts[pid].part] });
      for (const [fid, list] of regs.get(pid).candidates) for (let ci = 0; ci < Math.min(2, list.length); ci++) {
        const fr = (k) => ({ $call: 'util.path', args: [b[pid], ['candidates', fid, ci, 'frame', k]] });
        calls.push({ op: 'asm.faceRefAt', args: [b[pid], fid, fr('n'), { $call: 'util.add3', args: [fr('o'), chance(r, 0.5) ? [0, 0, 0] : rv3(r, 0.01)] }] });
        calls.push({ op: 'asm.faceRefAt', args: [b[pid], fid, rv3(r), rv3(r, 30)] });
      }
    }
    calls.push({ op: 'asm.rigidInverse', args: [pick(r, asm.instances).place] });
    // moving free parts by hand
    const free = [...A.freeIds(asm)];
    for (const id of [...free.slice(0, 2), asm.instances[0].id, 'i999', asm.instances[asm.instances.length - 1].id]) {
      const place = V.translation(rv3(r, 40));
      calls.push({ op: 'asm.moveInstance', args: [clone(asm), id, chance(r, 0.15) ? place.slice(0, 15) : place] });
    }
    for (let k = 0; k < 4; k++) calls.push({ op: 'asm.movePlace', args: [pick(r, asm.instances).place, { ...(chance(r, 0.7) ? { move: chance(r, 0.1) ? [NaN, 0, 0] : rv3(r, 20) } : {}), ...(chance(r, 0.7) ? { rotate: { axis: chance(r, 0.1) ? [0, 0, 0] : rv3(r), angle: round(r() * 360, 1), about: rv3(r, 10) } } : {}) }] });
    // interference filters
    const ids = asm.instances.map((i) => i.id);
    const clashes = Array.from({ length: ri(r, 0, 6) }, () => ({ a: pick(r, ids), b: pick(r, ids), volume: round(r() * 100, 2), kind: pick(r, A.CLASH_KINDS) }));
    for (let k = 0; k < 5; k++) {
      if (chance(r, 0.5)) { const kind = pick(r, A.CLASH_KINDS.concat(['nope'])), h = chance(r, 0.7); calls.push({ op: 'asm.setKindHidden', args: [clone(asm), kind, h] }); A.setKindHidden(asm, kind, h); }
      else { const a = pick(r, ids), bb = pick(r, ids), h = chance(r, 0.7); calls.push({ op: 'asm.setPairHidden', args: [clone(asm), a, bb, h] }); A.setPairHidden(asm, a, bb, h); }
      calls.push({ op: 'asm.filterClashes', args: [clone(asm), clashes] });
    }
    calls.push({ op: 'asm.readClashFilter', args: [asm.clashFilter ?? null, ids] });
    // exploded views
    calls.push({ op: 'explode.newView', args: [clone(asm), chance(r, 0.5) ? 'Out' : null] });
    const vw = X.newView(asm);
    for (let k = 0; k < ri(r, 1, 4); k++) {
      const insts = ids.filter(() => chance(r, 0.4));
      const dir = chance(r, 0.1) ? [0, 0, 0] : rv3(r);
      const dist = chance(r, 0.05) ? NaN : round(r() * 80, 1);
      calls.push({ op: 'explode.addStep', args: [clone(asm), vw.id, insts, dir, dist] });
      try { X.addStep(asm, asm.views.find((v) => v.id === vw.id), insts, dir, dist); } catch { /* refused */ }
    }
    const view = asm.views.find((v) => v.id === vw.id);
    for (const tt of [0, 0.3, 0.5, 1, null]) calls.push({ op: 'explode.offsets', args: [view, tt] });
    calls.push({ op: 'explode.exploded', args: [world, Object.fromEntries(X.offsets(view, 0.7))] });
    calls.push({ op: 'explode.trails', args: [view, Object.fromEntries(ids.map((id) => [id, rv3(r, 50)])), pick(r, [1, 0.4])] });
    const items = ids.map((id) => { const c = rv3(r, 50); return { id, box: { min: c, max: V.add(c, [round(r() * 30, 1), round(r() * 30, 1), round(r() * 30, 1)]) } }; });
    void pr;
    const tree = { $call: 'asm.jointTree', args: [clone(asm), b, worldOf(clone(asm), b)] };
    calls.push({ op: 'explode.autoSteps', args: [items, 'radial', {}] }, { op: 'explode.autoSteps', args: [items, 'joints', { tree, factor: pick(r, [1.25, 2]) }] }, { op: 'explode.autoSteps', args: [items, 'spiral', {}] });
    // files
    const text = A.toJSON(asm);
    calls.push({ op: 'asm.toJSON', args: [clone(asm)] }, { op: 'asm.fileVersion', args: [clone(asm)] }, { op: 'asm.read', args: [text] });
    const j = JSON.parse(text);
    for (let k = 0; k < 10; k++) calls.push({ op: 'asm.read', args: [mutateAsm(r, j)] });
    calls.push({ op: 'asm.read', args: [pick(r, ['', '{', 'null', '{"format":"partcraft-assembly","version":2,"parts":{},"instances":[],"joints":[]}'])] });
    for (const id of [pick(r, ids), 'i999']) calls.push({ op: 'asm.removeInstance', args: [clone(asm), id] });
    asm = null;
    scripts.push({ calls });
  }
  return scripts;
}

export const GENS = { asmflange: genFlanges, asm: genAssemblies };
