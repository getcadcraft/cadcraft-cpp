// SPDX-License-Identifier: Apache-2.0
// pcdiff/gen-sketch.mjs — random sketches built through the frozen sketch.js API (each building step is itself a call,
// with the sketch as it was before it), then solving (with and without drag pins), analysis, profiles, measuring,
// relations, removal, fillets and tampered files.
import * as D from './driver.mjs';
import { ri, pick, chance } from './gen-core.mjs';

const S = D.S;
const round = (x, d = 3) => Math.round(x * 10 ** d) / 10 ** d;
const RELS = ['coincident', 'horizontal', 'vertical', 'parallel', 'perpendicular', 'tangent', 'equal', 'concentric', 'midpoint', 'fix', 'symmetric', 'distance', 'hdistance', 'vdistance', 'radius', 'diameter', 'angle', 'offset', 'offsetlink', 'nope'];

/** Build a random sketch; every step is recorded as a call. */
export function buildSketch(r, calls, { big = false, clean = false } = {}) {
  const sk = S.newSketch();
  const step = (op, args, fn) => { calls.push({ op, args: [structuredClone(sk), ...args] }); try { return fn(); } catch { return null; } };
  const P = () => [round((r() - 0.5) * 100, 2), round((r() - 0.5) * 100, 2)];
  const pointIds = () => sk.entities.filter((e) => e.type === 'point').map((e) => e.id);
  const nShapes = big ? ri(r, 6, 12) : ri(r, 1, 5);
  for (let s = 0; s < nShapes; s++) {
    const k = clean ? pick(r, [0.1, 0.3, 0.5, 0.5, 0.75]) : r();
    if (k < 0.25) {
      const a = P(), b = [a[0] + round(10 + r() * 50, 2), a[1] + round(10 + r() * 40, 2)];
      const o = { construction: chance(r, 0.05) };
      const rect = step('sketch.addRect', [a, b, o], () => S.addRect(sk, a, b, o));
      if (rect && chance(r, 0.7)) {
        const l = pick(r, rect.lines);
        step('sketch.addConstraint', ['distance', [l], round(5 + r() * 60, 2)], () => S.addConstraint(sk, 'distance', [l], round(5 + r() * 60, 2)));
      }
      if (rect && chance(r, 0.5)) {
        const pid = S.byId(sk, rect.lines[0]).p1;
        step('sketch.addConstraint', ['coincident', [pid, 'O']], () => S.addConstraint(sk, 'coincident', [pid, 'O']));
      }
    } else if (k < 0.45) {
      const n = ri(r, 3, 6), c = P(), R = 10 + r() * 30;
      let prev = null, first = null;
      for (let i = 0; i < n; i++) {
        const ang = (2 * Math.PI * i) / n + r() * 0.3;
        const q = [round(c[0] + R * Math.cos(ang), 2), round(c[1] + R * Math.sin(ang), 2)];
        const ang2 = (2 * Math.PI * (i + 1)) / n;
        const q2 = [round(c[0] + R * Math.cos(ang2), 2), round(c[1] + R * Math.sin(ang2), 2)];
        const a = prev ? prev : q, b = i === n - 1 && first ? first : q2;
        const l = step('sketch.addLine', [a, b, {}], () => S.addLine(sk, a, b));
        if (!l) break;
        if (!first) first = l.p1;
        prev = l.p2;
      }
    } else if (k < 0.6) {
      const c = chance(r, 0.3) && pointIds().length ? pick(r, pointIds()) : P();
      const rad = round(2 + r() * 25, 2);
      const circ = step('sketch.addCircle', [c, rad, {}], () => S.addCircle(sk, c, rad));
      if (circ && chance(r, 0.6)) step('sketch.addConstraint', [pick(r, ['radius', 'diameter']), [circ.id], round(1 + r() * 30, 2)], () => S.addConstraint(sk, pick(r, ['radius', 'diameter']), [circ.id], round(1 + r() * 30, 2)));
    } else if (k < 0.72) {
      const c = P(), s0 = [c[0] + round(5 + r() * 20, 2), c[1]], e0 = [c[0], c[1] + round(5 + r() * 20, 2)];
      const sArg = chance(r, 0.3) && pointIds().length ? pick(r, pointIds()) : s0;
      step('sketch.addArc', [c, sArg, e0, {}], () => S.addArc(sk, c, sArg, e0));
    } else if (k < 0.82) {
      const n = ri(r, 2, 6), pts = [];
      for (let i = 0; i < n; i++) pts.push([round(i * 15 + r() * 10, 2), round((r() - 0.5) * 30, 2)]);
      const o = { kind: pick(r, ['fit', 'control']), closed: n >= 3 && (clean || chance(r, 0.3)) };
      if (clean && n < 3) continue;
      if (chance(r, 0.2)) o.a0 = round(r() * 3, 2);
      step('sketch.addSpline', [pts, o], () => S.addSpline(sk, pts, o));
    } else if (k < 0.9) {
      const a = P(), b = P();
      step('sketch.addLine', [a, b, { construction: true }], () => S.addLine(sk, a, b, { construction: true }));
    } else {
      const p = P();
      step('sketch.addPoint', [p[0], p[1], {}], () => S.addPoint(sk, p[0], p[1]));
    }
  }
  // random relations and dimensions on what is there
  const ids = () => ['O', ...sk.entities.map((e) => e.id)];
  for (let i = 0; i < (clean ? ri(r, 0, 3) : big ? 14 : ri(r, 0, 8)); i++) {
    const type = pick(r, RELS);
    const n = pick(r, [1, 2, 2, 2, 3]);
    const refs = []; for (let j = 0; j < n; j++) refs.push(pick(r, ids()));
    let value;
    const m = (() => { try { return S.measure(sk, type, S.matchSig(sk, type, refs)?.refs || refs); } catch { return {}; } })();
    if (typeof m.value === 'number' && chance(r, 0.8)) value = round(m.value * (0.8 + r() * 0.4), 3);
    else if (chance(r, 0.2)) value = pick(r, [-1, 0, 200, 'x', '12']);
    const extra = type === 'offsetlink' ? { of: pick(r, sk.constraints.map((c) => c.id).concat(['k99'])) } : {};
    step('sketch.addConstraint', [type, refs, value ?? null, extra], () => S.addConstraint(sk, type, refs, value, extra));
  }
  // a fillet at a corner now and then
  if (chance(r, 0.3)) {
    const pid = pick(r, pointIds().length ? pointIds() : ['p1']);
    const rad = round(1 + r() * 6, 2);
    step('sketch.filletCorner', [pid, rad], () => S.filletCorner(sk, pid, rad));
  }
  return sk;
}

function tamper(r, sk) {
  const t = structuredClone(sk);
  const k = r();
  const e = t.entities.length ? pick(r, t.entities) : null;
  const c = t.constraints.length ? pick(r, t.constraints) : null;
  if (k < 0.08) t.entities.push(null);
  else if (k < 0.14 && e) e.id = pick(r, ['', '1x', 'O', t.entities[0].id, 'a'.repeat(50), 5]);
  else if (k < 0.2 && e) e.type = pick(r, ['ellipse', 'POINT', 5]);
  else if (k < 0.26 && e && e.type === 'point') e.x = pick(r, ['1', 1e10, null]);
  else if (k < 0.32 && e && e.type === 'line') e.p2 = pick(r, ['O', 'zz', e.id]);
  else if (k < 0.36 && e && e.type === 'circle') e.r = pick(r, [0, -2, 'x']);
  else if (k < 0.4 && e && e.type === 'arc') e.s = 'O';
  else if (k < 0.46 && e && e.type === 'spline') { const v = r(); if (v < 0.3) e.kind = 'bezier'; else if (v < 0.5) e.pts = [e.pts[0]]; else if (v < 0.7) e.a0 = 'x'; else e.knots = [0, 0, 1]; }
  else if (k < 0.52 && c) c.type = pick(r, ['nope', 'toString', 'constructor', 'distance']);
  else if (k < 0.58 && c) c.refs = pick(r, [['O'], ['zz', 'O'], 'x', c.refs.slice().reverse(), [1, 2]]);
  else if (k < 0.64 && c) c.value = pick(r, [-1, 'x', [1, 'y'], 1e10, null]);
  else if (k < 0.68 && c) { c.sign = pick(r, [2, -1, 1]); c.kind = pick(r, ['ext', 'int', 'x']); c.at = [1, 2]; c.off = [1, 'x']; }
  else if (k < 0.72) t.constraints.push({ id: 'k900', type: 'offsetlink', refs: [], of: 'k1' });
  else if (k < 0.76) t.seq = pick(r, [0, -3, 2.5, 'x', 1e6]);
  else if (k < 0.8) delete t.constraints;
  else if (k < 0.83) t.entities.push({ id: 'p999', type: 'point', x: 1, y: 2, construction: 'yes' });
  return t;
}

function genSketch(r) {
  const scripts = [];
  for (let s = 0; s < 10; s++) {
    const calls = [];
    for (let k = 0; k < 3; k++) {
      const sk = buildSketch(r, calls, { big: chance(r, 0.3), clean: k !== 1 });
      calls.push({ op: 'sketch.read', args: [sk] });
      calls.push({ op: 'sketch.residuals', args: [sk] });
      calls.push({ op: 'sketch.solve', args: [sk, null, null] });
      calls.push({ op: 'sketch.analyze', args: [sk, chance(r, 0.5) ? null : 50] });
      const pts = sk.entities.filter((e) => e.type === 'point');
      if (pts.length) {
        const p = pick(r, pts);
        calls.push({ op: 'sketch.solve', args: [sk, { [p.id]: [round(p.x + (r() - 0.5) * 10, 2), round(p.y + (r() - 0.5) * 10, 2)] }, null] });
      }
      const circ = sk.entities.filter((e) => e.type === 'circle');
      if (circ.length) calls.push({ op: 'sketch.solve', args: [sk, null, { [pick(r, circ).id]: round(r() * 20, 2) }] });
      let solved = structuredClone(sk);
      try { D.SV.solveSketch(solved); } catch { solved = sk; }
      for (const seg of [64, 16, 7]) calls.push({ op: 'sketch.profile', args: [solved, seg] });
      const pr = S.profile(solved, 32);
      if (pr.loops) {
        for (let i = 0; i < 6; i++) calls.push({ op: 'sketch.insideLoops', args: [pr.loops, [round((r() - 0.5) * 100, 2), round((r() - 0.5) * 100, 2)]] });
        for (const l of pr.loops) calls.push({ op: 'sketch.loopArea', args: [l] });
      }
      const ids = ['O', ...sk.entities.map((e) => e.id)];
      for (let i = 0; i < 8; i++) {
        const refs = [pick(r, ids), pick(r, ids)].slice(0, ri(r, 1, 2));
        calls.push({ op: 'sketch.applicable', args: [sk, refs] });
        const type = pick(r, RELS);
        calls.push({ op: 'sketch.matchSig', args: [sk, type, refs] });
        const m = S.matchSig(sk, type, refs);
        if (m) calls.push({ op: 'sketch.measure', args: [sk, type, m.refs] });
      }
      for (const c of sk.constraints) calls.push({ op: 'sketch.describe', args: [c] });
      calls.push({ op: 'sketch.bounds', args: [sk] }, { op: 'sketch.extent', args: [sk] }, { op: 'sketch.loosePoints', args: [sk] });
      for (const e of sk.entities) {
        if (e.type === 'arc') calls.push({ op: 'sketch.tessArc', args: [sk, e.id, pick(r, [64, 32, 5, 1])] });
        if (e.type === 'spline') calls.push({ op: 'sketch.tessSpline', args: [sk, e.id, pick(r, [16, 4, 1])] });
      }
      const curves = sk.entities.filter((e) => e.type !== 'point');
      for (let i = 0; i < 4 && curves.length > 1; i++) calls.push({ op: 'sketch.endMeet', args: [sk, pick(r, curves).id, pick(r, curves).id] });
      if (ids.length > 1) {
        calls.push({ op: 'sketch.removeEntities', args: [sk, ids.filter(() => chance(r, 0.2))] });
        calls.push({ op: 'sketch.removeConstraints', args: [sk, sk.constraints.filter(() => chance(r, 0.3)).map((c) => c.id)] });
      }
      for (let i = 0; i < 4; i++) calls.push({ op: 'sketch.read', args: [tamper(r, sk)] });
    }
    scripts.push({ calls });
  }
  return scripts;
}

export const GENS = { sketch: genSketch };
