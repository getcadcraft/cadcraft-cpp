// SPDX-License-Identifier: Apache-2.0
// pcdiff/gen-tests.mjs — the frozen Node tests' sections that need only ported modules, run as they are written but
// on recording wrappers of the frozen modules: every call to a ported function becomes a parity case (its arguments
// as they were before the call). Sections: test-partcraft.mjs (sketch rectangle / every relation / dragging, fillet,
// profiles / sketch files / history (no kernel calls) / part files), test-partcraft-params.mjs (expressions, engine,
// adapter, the sketch that cannot solve, the params key of the file), tools/partcraft-test/test-configs.mjs.
import * as D from './driver.mjs';
import { flange, flangeSketch, blockSketch } from './gen-part.mjs';

const clone = (v) => structuredClone(v);

/** Wrap the frozen modules so each call of a mapped function is recorded as `op`. */
function recorder(calls) {
  const wrap = (mod, map) => new Proxy(mod, {
    get(t, k) {
      const f = t[k];
      const op = map[k];
      if (!op || typeof f !== 'function') return f;
      return (...a) => { calls.push({ op, args: a.map((x) => (x === undefined ? null : clone(x))) }); return f(...a); };
    },
  });
  const S = wrap(D.S, {
    addPoint: 'sketch.addPoint', addLine: 'sketch.addLine', addCircle: 'sketch.addCircle', addArc: 'sketch.addArc', addRect: 'sketch.addRect', addSpline: 'sketch.addSpline',
    addConstraint: 'sketch.addConstraint', removeConstraints: 'sketch.removeConstraints', removeEntities: 'sketch.removeEntities', filletCorner: 'sketch.filletCorner',
    profile: 'sketch.profile', bounds: 'sketch.bounds', applicable: 'sketch.applicable', readSketch: 'sketch.read', loopArea: 'sketch.loopArea', extent: 'sketch.extent', describe: 'sketch.describe',
  });
  const solveSketch = (sk, o = {}) => {
    calls.push({ op: 'sketch.solve', args: [clone(sk), o.pin ? Object.fromEntries(o.pin) : null, o.pinRadius ? Object.fromEntries(o.pinRadius) : null] });
    return D.SV.solveSketch(sk, o);
  };
  const P = wrap(D.P, { newPart: 'part.new', addFeature: 'part.addFeature', readPart: 'part.read', toJSON: 'part.toJSON', canMove: 'part.canMove', moveFeature: 'part.moveFeature', deleteFeatures: 'part.deleteFeatures', fileVersion: 'part.fileVersion' });
  const E = wrap(D.E, { parse: 'expr.parse', compute: 'expr.compute', literal: 'expr.literal', renameIn: 'expr.renameIn', formatNumber: 'expr.formatNumber' });
  const PR = new Proxy(D.PR, {
    get(t, k) {
      const rec = (op, args) => calls.push({ op, args: args.map((x) => (x === undefined ? null : clone(x))) });
      switch (k) {
        case 'setVar': return (p, name, expr, o) => { rec('params.setVar', [p, name, expr, o || {}]); return t.setVar(p, name, expr, o); };
        case 'evaluateVars': return (p, ov) => { rec('params.evaluateVars', [p, ov ? Object.fromEntries(ov) : null]); return t.evaluateVars(p, ov); };
        case 'driveVar': return (p, name, v, o = {}) => { rec('params.driveVar', [p, name, v, o.prefer ?? null]); return t.driveVar(p, name, v, o); };
        case 'removeVar': return (p, name, o = {}) => { rec('params.removeVar', [p, name, !!o.inline]); return t.removeVar(p, name, o); };
        case 'readDesignTable': return (csv, p) => { rec('params.readDesignTable', [csv, p]); return t.readDesignTable(csv, p); };
        case 'setLink': return (p, ad, doc, key, expr) => { if (doc.params === p) rec('pp.setLink', [doc, key, expr]); return t.setLink(p, ad, doc, key, expr); };
        case 'driveBack': return (p, ad, doc, key, v, o = {}) => { if (doc.params === p) rec('pp.driveBack', [doc, key, v, o.prefer ?? null]); return t.driveBack(p, ad, doc, key, v, o); };
        default: return t[k];
      }
    },
  });
  const PP = new Proxy(D.PP, {
    get(t, k) {
      const rec = (op, args) => calls.push({ op, args: args.map((x) => (x === undefined ? null : clone(x))) });
      switch (k) {
        case 'applyPartParams': return (part, o = {}) => { rec('pp.apply', [part, o.overrides ? Object.fromEntries(o.overrides) : null]); return t.applyPartParams(part, o); };
        case 'resolveTarget': return (part, s) => { rec('pp.resolveTarget', [part, s]); return t.resolveTarget(part, s); };
        case 'stepRollback': return (part, d) => { rec('pp.stepRollback', [part, d]); return t.stepRollback(part, d); };
        case 'rollTo': return (part, id) => { rec('pp.rollTo', [part, id]); return t.rollTo(part, id); };
        case 'rollbackWords': return (part) => { rec('pp.rollbackWords', [part]); return t.rollbackWords(part); };
        case 'partAdapter': return { ...t.partAdapter, targets: (part) => { rec('pp.targets', [part]); return t.partAdapter.targets(part); } };
        default: return t[k];
      }
    },
  });
  const CF = wrap(D.CF, { readConfigs: 'configs.read', rowFromCurrent: 'configs.rowFromCurrent', saveRow: 'configs.saveRow', applyRow: 'configs.applyRow', rowMatches: 'configs.rowMatches', renameRow: 'configs.renameRow', deleteRow: 'configs.deleteRow', followParams: 'configs.followParams', rowsFromTable: 'configs.rowsFromTable', configsCsv: 'configs.csv' });
  return { S, P, E, PR, PP, CF, solveSketch };
}

const quiet = (fn) => { try { return fn(); } catch { return undefined; } };

/** test-partcraft.mjs: the sketch sections and part files / history without the kernel. */
function sketchSections(R) {
  const { S, P, solveSketch } = R;
  const P2 = (sk, id) => { const p = D.S.byId(sk, id); return [p.x, p.y]; };
  void P2;
  { // rectangle, dimensions, degrees of freedom
    const sk = S.newSketch();
    const r = S.addRect(sk, [3, 4], [40, 30]);
    solveSketch(sk);
    const l0 = D.S.byId(sk, r.lines[0]);
    S.addConstraint(sk, 'distance', [r.lines[0]], 100);
    S.addConstraint(sk, 'distance', [r.lines[1]], 50);
    solveSketch(sk); S.bounds(sk);
    S.addConstraint(sk, 'coincident', [l0.p1, 'O']);
    solveSketch(sk);
    const c90 = S.addConstraint(sk, 'distance', [r.lines[2]], 90);
    solveSketch(sk);
    S.removeConstraints(sk, [c90.id]);
    const c100 = S.addConstraint(sk, 'distance', [r.lines[2]]);
    solveSketch(sk);
    S.removeConstraints(sk, [c100.id]);
    solveSketch(sk);
  }
  {
    const sk = S.newSketch();
    const l = S.addLine(sk, [0, 0], [10, 0]);
    const c = S.addConstraint(sk, 'distance', [l.id], 10);
    sk.constraints.find((k) => k.id === c.id).value = 14;
    solveSketch(sk);
  }
  { // every relation and dimension
    const mk = () => S.newSketch();
    let sk, L, q, C;
    sk = mk(); const pa = S.addPoint(sk, 1, 2), pb = S.addPoint(sk, 4, 6); S.addConstraint(sk, 'coincident', [pa, pb]); solveSketch(sk);
    sk = mk(); L = S.addLine(sk, [0, 0], [10, 0]); q = S.addPoint(sk, 5, 3); S.addConstraint(sk, 'coincident', [q, L.id]); solveSketch(sk);
    sk = mk(); C = S.addCircle(sk, [0, 0], 5); q = S.addPoint(sk, 8, 1); S.addConstraint(sk, 'coincident', [q, C.id]); solveSketch(sk);
    sk = mk(); L = S.addLine(sk, [0, 0], [10, 2]); S.addConstraint(sk, 'horizontal', [L.id]); solveSketch(sk);
    sk = mk(); L = S.addLine(sk, [0, 0], [1, 10]); S.addConstraint(sk, 'vertical', [L.id]); solveSketch(sk);
    sk = mk(); const v1 = S.addPoint(sk, 0, 0), v2 = S.addPoint(sk, 3, 7); S.addConstraint(sk, 'vertical', [v1, v2]); S.addConstraint(sk, 'horizontal', [S.addPoint(sk, 9, 9), v2]); solveSketch(sk);
    sk = mk(); const A = S.addLine(sk, [0, 0], [10, 1]), B = S.addLine(sk, [0, 5], [8, 9]); S.addConstraint(sk, 'parallel', [A.id, B.id]); solveSketch(sk);
    sk = mk(); const A2 = S.addLine(sk, [0, 0], [10, 1]), B2 = S.addLine(sk, [0, 5], [3, 9]); S.addConstraint(sk, 'perpendicular', [A2.id, B2.id]); solveSketch(sk);
    S.addConstraint(sk, 'equal', [A2.id, B2.id]); solveSketch(sk);
    sk = mk(); const A3 = S.addLine(sk, [0, 0], [10, 0]), B3 = S.addLine(sk, [0, 0], [10, 4]); S.addConstraint(sk, 'horizontal', [A3.id]); S.addConstraint(sk, 'angle', [A3.id, B3.id], 30); solveSketch(sk);
    sk = mk(); L = S.addLine(sk, [-10, 3], [10, 3]); C = S.addCircle(sk, [0, 0], 5); S.addConstraint(sk, 'tangent', [L.id, C.id]); solveSketch(sk);
    sk = mk(); C = S.addCircle(sk, [0, 0], 5); const C2 = S.addCircle(sk, [12, 0], 4); S.addConstraint(sk, 'tangent', [C.id, C2.id]); solveSketch(sk);
    sk = mk(); C = S.addCircle(sk, [0, 0], 10); const C3 = S.addCircle(sk, [3, 0], 4); S.addConstraint(sk, 'tangent', [C.id, C3.id]); solveSketch(sk);
    sk = mk(); C = S.addCircle(sk, [0, 0], 10); const C4 = S.addCircle(sk, [3, 1], 4); S.addConstraint(sk, 'concentric', [C.id, C4.id]); solveSketch(sk);
    sk = mk(); L = S.addLine(sk, [0, 0], [10, 4]); q = S.addPoint(sk, 1, 1); S.addConstraint(sk, 'midpoint', [q, L.id]); solveSketch(sk);
    sk = mk(); L = S.addLine(sk, [0, 0], [10, 0]); S.addConstraint(sk, 'fix', [L.p1]); const dl = S.addConstraint(sk, 'distance', [L.id], 10); sk.constraints.find((k) => k.id === dl.id).value = 20; solveSketch(sk);
    sk = mk(); L = S.addLine(sk, [0, 0], [10, 0]); S.addConstraint(sk, 'fix', [L.id]); q = S.addPoint(sk, 5, 3); S.addConstraint(sk, 'distance', [q, L.id], 7); solveSketch(sk);
    sk = mk(); const D1 = S.addLine(sk, [0, 0], [10, 0]), D2 = S.addLine(sk, [0, 4], [10, 4]); S.addConstraint(sk, 'fix', [D1.id]); S.addConstraint(sk, 'parallel', [D1.id, D2.id]); S.addConstraint(sk, 'distance', [D1.id, D2.id], 12); solveSketch(sk);
    sk = mk(); const h1 = S.addPoint(sk, 0, 0), h2 = S.addPoint(sk, 5, 5); S.addConstraint(sk, 'hdistance', [h1, h2], 30); S.addConstraint(sk, 'vdistance', [h1, h2], 12); solveSketch(sk);
    sk = mk(); C = S.addCircle(sk, [0, 0], 3); S.addConstraint(sk, 'diameter', [C.id], 25); solveSketch(sk);
    sk = mk(); const ar = S.addArc(sk, [0, 0], [5, 0], [0, 5]); S.addConstraint(sk, 'radius', [ar.id], 9); solveSketch(sk);
    sk = mk(); L = S.addLine(sk, [0, 0], [10, 0]); C = S.addCircle(sk, [0, 5], 2); S.applicable(sk, [L.id, C.id]);
    quiet(() => S.addConstraint(sk, 'parallel', [L.id, C.id])); quiet(() => S.addConstraint(sk, 'radius', [C.id], 0));
  }
  { // dragging, fillet, profiles
    const sk = S.newSketch();
    const C = S.addCircle(sk, [0, 0], 10);
    S.addConstraint(sk, 'fix', [C.c]); S.addConstraint(sk, 'radius', [C.id], 10);
    const q = S.addPoint(sk, 10, 0); S.addConstraint(sk, 'coincident', [q, C.id]);
    solveSketch(sk, { pin: new Map([[q, [20, 20]]]) });
    const free = S.addPoint(sk, 30, 30);
    solveSketch(sk, { pin: new Map([[free, [33, 34]]]) });
  }
  {
    const sk = S.newSketch();
    const r = S.addRect(sk, [0, 0], [100, 50]);
    const corner = D.S.byId(sk, r.lines[0]).p1;
    solveSketch(sk);
    S.filletCorner(sk, corner, 10);
    solveSketch(sk);
    const pr = S.profile(sk, 64);
    if (pr.loops) S.loopArea(pr.loops[0]);
    quiet(() => S.filletCorner(sk, D.S.byId(sk, r.lines[1]).p2, 60));
    sk.constraints.find((c) => c.type === 'radius').value = 15;
    solveSketch(sk);
  }
  {
    const sk = S.newSketch(); S.addLine(sk, [0, 0], [10, 0]); S.addLine(sk, [10, 0], [10, 10]); S.profile(sk);
    const sk2 = S.newSketch(); S.addRect(sk2, [0, 0], [10, 10]); S.addLine(sk2, [10, 10], [20, 20]); S.profile(sk2);
    const sk3 = S.newSketch(); S.addRect(sk3, [0, 0], [40, 40]); S.addCircle(sk3, [20, 20], 5); S.profile(sk3, 64);
  }
  { // sketch files
    const fl = flangeSketch();
    S.readSketch(JSON.parse(JSON.stringify(fl.sk)));
    const bad = JSON.parse(JSON.stringify(fl.sk)); bad.entities.push({ id: 'l999', type: 'line', p1: 'p1', p2: 'nope' }); quiet(() => S.readSketch(bad));
    const bad2 = JSON.parse(JSON.stringify(fl.sk)); bad2.constraints[0].type = 'eval'; quiet(() => S.readSketch(bad2));
    const bad3 = JSON.parse(JSON.stringify(fl.sk)); bad3.constraints.find((c) => c.type === 'hdistance').refs = [fl.lines[0], fl.lines[1]]; quiet(() => S.readSketch(bad3));
  }
  { // history (the part edits; the kernel's part is stage 3)
    const part = P.newPart('Block');
    const b = blockSketch();
    const s1 = P.addFeature(part, 'sketch', { plane: { kind: 'base', name: 'top' }, sketch: b.sk });
    const e1 = P.addFeature(part, 'extrude', { sketch: s1.id, depth: 20 });
    const sk = D.S.newSketch(); D.S.addLine(sk, [0, 0], [10, 0]); D.S.addLine(sk, [10, 0], [10, 10]);
    const s2 = P.addFeature(part, 'sketch', { plane: { kind: 'face', feature: e1.id, face: 'end' }, sketch: sk });
    const bad = P.addFeature(part, 'extrude', { sketch: s2.id, op: 'cut', end: 'blind', depth: 3 });
    P.addFeature(part, 'lpattern', { seeds: [bad.id] });
    part.rollback = 1;
    const added = P.addFeature(part, 'extrude', { sketch: s1.id, depth: 5 });
    part.rollback = null;
    P.canMove(part, bad.id, 0); P.canMove(part, added.id, 2);
    P.moveFeature(part, added.id, 2);
    P.deleteFeatures(part, [e1.id]);
  }
  { // part files
    const fl = flange();
    const json = P.toJSON(fl.part);
    P.readPart(json);
    const tamper = (fn) => { const t = JSON.parse(json); fn(t); quiet(() => P.readPart(JSON.stringify(t))); };
    tamper((t) => { t.version = D.P.VERSION + 1; });
    tamper((t) => { t.format = 'other'; });
    tamper((t) => { t.features[1].type = 'script'; });
    tamper((t) => { t.features[1].sketch = 'f99'; });
    tamper((t) => { t.features[4].count = 1e6; });
    tamper((t) => { t.features[2].plane = { kind: 'face', feature: 'f9' }; });
    tamper((t) => { t.features[1].angle = 'Infinity'; });
    quiet(() => P.readPart('{nope'));
    const named = JSON.parse(json); named.name = 'A\u0000B<script>'; named.features[0].name = 'x\u0007y';
    P.readPart(JSON.stringify(named));
  }
}

/** test-partcraft-params.mjs: expressions, engine, adapter, the unsolvable sketch, the params key. */
function paramsSections(R) {
  const { E, PR, PP, P, S } = R;
  const val = (src, v = {}) => E.compute(src, v);
  for (const s of ['1in', "2'", '3 "', '1.5cm', '2m', '1ft + 1in', '0.5rad', '90°', '45deg', '2 + 3 * 4', '(2 + 3) * 4', '2^3^2', '-2^2', '10 % 4', '-7 % 3', 'sin(30)', 'cos(60)', 'atan2(1, 1)', 'sqrt(16)', 'max(1, 7, 3)', 'pi', 'round(2.5)', 'round(-2.5)', 'round(1.005, 2)', 'ceil(0.1 * 3 / 0.3)', 'floor(0.7 + 0.1 + 0.2)', 'constructor', '__proto__', 'toString(1)', 'constructor(1)', '1 / 0', 'sqrt(-1)', 'ln(0)', 'tan(90)', '2 +', '(1', '1)', 'x;1', '`1`', '1+'.repeat(600) + '1', '('.repeat(150) + '1' + ')'.repeat(150)]) val(s);
  val('if(w > 10, 1, 2)', { w: 12 }); val('if(w > 10, 1, 2)', { w: 10 }); val('w == 10', { w: 10 }); val('a b', { a: 1, b: 2 });
  for (const s of ['40', '-2in', 'w', '2*3', 'pi']) E.literal(s);
  E.renameIn('w*2 + 25mm + width', 'w', 'x'); E.renameIn('2m + m', 'm', 'q');
  { // engine
    const p = PR.emptyParams();
    PR.setVar(p, 'width', '100'); PR.setVar(p, 'half', 'width / 2'); PR.setVar(p, 'q', 'half / 2 + 1');
    PR.evaluateVars(p);
    PR.setVar(p, 'width', 'q * 2');
    for (const n of ['sin', 'mm', 'pi', '2x', 'a-b']) PR.setVar(p, n, '3');
    { const z = PR.emptyParams(); for (const n of ['__proto__', 'constructor', 'prototype']) PR.setVar(z, n, '7'); }
    PR.driveVar(p, 'q', 51); PR.evaluateVars(p);
    PR.driveVar(p, 'q', 1);
    PR.setVar(p, 'width', '100'); PR.setVar(p, 'b', '3'); PR.setVar(p, 'both', 'width + b');
    PR.driveVar(p, 'both', 110); PR.driveVar(p, 'both', 110, { prefer: 'b' });
    PR.setVar(p, 'sq', 'width ^ 2');
    PR.driveVar(p, 'sq', 14400); PR.driveVar(p, 'sq', 14401);
    PR.removeVar(p, 'width'); PR.removeVar(p, 'width', { inline: true });
  }
  { // adapter
    const fl0 = flange();
    PP.partAdapter.targets(fl0.part);
    for (const s of ['Hole1.d', 'Hole1 diameter', 'Sketch1.D4', 'Nope.d']) PP.resolveTarget(fl0.part, s);
  }
  { // the flange's equations (the exact-kernel volumes are stage 3; the parameter side runs here)
    const fl = flange();
    const part = fl.part;
    const heightKey = fl.ids.profile + '.' + fl.dims.height, discKey = fl.ids.profile + '.' + fl.dims.disc;
    const p = D.PP.paramsOf(part);
    PR.setVar(p, 'bolt_d', '14'); PR.setVar(p, 'disc_t', '30'); PR.setVar(p, 'hub_h', 'disc_t + 50');
    PR.setLink(p, D.PP.partAdapter, part, fl.ids.hole + '.d', 'bolt_d');
    PR.setLink(p, D.PP.partAdapter, part, fl.ids.pattern + '.count', 'if(bolt_d >= 16, 6, 8)');
    PR.setLink(p, D.PP.partAdapter, part, discKey, 'disc_t'); PR.setLink(p, D.PP.partAdapter, part, heightKey, 'hub_h');
    PP.applyPartParams(part);
    PR.setVar(p, 'bolt_d', '16'); PP.applyPartParams(part);
    PR.driveBack(p, D.PP.partAdapter, part, fl.ids.hole + '.d', 12); PP.applyPartParams(part);
    PR.driveBack(p, D.PP.partAdapter, part, heightKey, 90); PP.applyPartParams(part);
    PR.driveBack(p, D.PP.partAdapter, part, fl.ids.pattern + '.count', 7.5);
    PR.setVar(p, 'bolt_d', '-3'); PP.applyPartParams(part);
    PR.setVar(p, 'bolt_d', '12'); PP.applyPartParams(part);
    // the part file's params key
    const json = P.toJSON(part);
    P.readPart(json);
    const plain = flange().part; D.PP.paramsOf(plain); D.PP.tidyParams(plain); P.toJSON(plain); P.fileVersion(plain);
    const empty = flange().part; empty.params = D.PR.emptyParams(); P.toJSON(empty); P.fileVersion(empty);
    const tamper = (fn) => { const t = JSON.parse(json); fn(t); quiet(() => P.readPart(JSON.stringify(t))); };
    tamper((t) => { t.params.links[0].target = 'f99.depth'; });
    tamper((t) => { t.params.links[0].target = fl.ids.hole + '.colour'; });
    tamper((t) => { t.params.vars[0].name = 'sin'; });
    tamper((t) => { t.params.vars.push({ ...t.params.vars[0] }); });
    tamper((t) => { t.params.vars[0].expr = 'bolt_d +'; });
    tamper((t) => { t.params.vars[0].expr = 'x'.repeat(1001); });
    tamper((t) => { t.params.links.push({ ...t.params.links[0] }); });
    tamper((t) => { t.params.links[0].target = '../../etc'; });
    tamper((t) => { t.params.vars = Array.from({ length: 501 }, (_, i) => ({ name: 'v' + i, expr: '1' })); });
    tamper((t) => { t.params = [1, 2]; });
  }
  { // a sketch that cannot solve is put back
    const pt = P.newPart('Ring');
    const sk = S.newSketch();
    const c = S.addCircle(sk, [0, 0], 10);
    S.addConstraint(sk, 'coincident', [c.c, 'O']);
    const rad = S.addConstraint(sk, 'radius', [c.id], 10);
    const q = S.addPoint(sk, 10, 0);
    S.addConstraint(sk, 'coincident', [q, c.id]);
    S.addConstraint(sk, 'horizontal', ['O', q]);
    const dq = S.addConstraint(sk, 'distance', ['O', q], 10);
    const sf = P.addFeature(pt, 'sketch', { plane: { kind: 'base', name: 'front' }, sketch: sk });
    const p = D.PP.paramsOf(pt);
    PR.setVar(p, 'r', '10'); PR.setVar(p, 'reach', '10');
    PR.setLink(p, D.PP.partAdapter, pt, sf.id + '.' + rad.id, 'r');
    PR.setLink(p, D.PP.partAdapter, pt, sf.id + '.' + dq.id, 'reach');
    PR.setVar(p, 'reach', '5');
    PP.applyPartParams(pt);
    PR.setVar(p, 'reach', '20'); PR.setVar(p, 'r', '20');
    PP.applyPartParams(pt);
  }
}

/** tools/partcraft-test/test-configs.mjs (the script lines are stage 2's). */
function configSections(R) {
  const { S, P, PP, CF } = R;
  const make = () => {
    const part = P.newPart('Plate');
    const sk = S.newSketch(); S.addRect(sk, [0, 0], [100, 60]);
    const s = P.addFeature(part, 'sketch', { plane: { kind: 'base', name: 'front' }, sketch: sk });
    const ex = P.addFeature(part, 'extrude', { sketch: s.id, depth: 10 });
    part.params = { vars: [{ name: 'w', expr: '100' }, { name: 'd', expr: '10' }, { name: 'area', expr: 'w*d' }], links: [{ target: `${ex.id}.depth`, expr: 'd' }] };
    return { part, ex };
  };
  {
    const { part } = make();
    const small = CF.rowFromCurrent(part.params, '  Small\u0007 ');
    let cfg = CF.saveRow(null, small);
    part.params.vars[1].expr = '25';
    cfg = CF.saveRow(cfg, CF.rowFromCurrent(part.params, 'Thick'));
    cfg = CF.saveRow(cfg, { name: 'small', values: { w: 120, d: 8 } });
    CF.applyRow(part.params, cfg.rows[0]);
    PP.applyPartParams(part);
    CF.rowMatches(part.params, cfg.rows[0]); CF.rowMatches(part.params, cfg.rows[1]);
    part.params.vars[1].expr = '9';
    CF.rowMatches(part.params, cfg.rows[0]);
    CF.renameRow({ ...cfg, active: 'Small' }, 'Small', 'Light');
    quiet(() => CF.renameRow(cfg, 'Small', 'thick'));
    const d1 = CF.deleteRow({ ...cfg, active: 'Thick' }, 'Thick');
    CF.deleteRow(d1, 'Small');
    const before = JSON.parse(JSON.stringify(part.params));
    const after = JSON.parse(JSON.stringify(part.params)); after.vars[0].name = 'width';
    let f = CF.followParams(cfg, before, after);
    const gone = JSON.parse(JSON.stringify(after)); gone.vars.splice(1, 1);
    f = CF.followParams(f, after, gone);
  }
  {
    const { part } = make();
    part.configs = { rows: [{ name: 'Small', values: { w: 100, d: 10 } }, { name: 'Large', values: { w: 200, d: 20 } }], active: 'Large' };
    part.material = { name: 'Steel', density: 7.85 };
    const json = P.toJSON(part);
    P.readPart(json);
    const bad = (mut) => { const j = JSON.parse(json); mut(j); quiet(() => P.readPart(JSON.stringify(j))); };
    bad((j) => { j.configs.rows[0].values.size = 3; });
    bad((j) => { j.configs.rows[0].values.w = '1e999'; });
    bad((j) => { j.configs.rows[1].name = 'SMALL'; }); bad((j) => { j.configs.rows[1].name = ' '; });
    bad((j) => { j.configs.rows = Array.from({ length: 501 }, (_, i) => ({ name: 'r' + i, values: {} })); });
    P.readPart(JSON.stringify({ ...JSON.parse(json), configs: { rows: [{ name: 'A', values: {} }], active: 'Nope' } }));
    const noCfg = JSON.parse(json); delete noCfg.configs; delete noCfg.material;
    P.toJSON(D.P.readPart(JSON.stringify({ ...noCfg, configs: { rows: [] } })));
  }
  {
    const { part } = make();
    const rows = CF.rowsFromTable('name,w,d\r\nSmall,100,10\r\n"Large, wide",250,12\r\nKeep d,90,\r\n', part.params);
    let cfg = null; for (const r of rows) cfg = CF.saveRow(cfg, r);
    cfg = CF.saveRow(cfg, { name: '=cmd|calc', values: { w: 1 } });
    const csv = CF.configsCsv(cfg);
    CF.rowsFromTable(csv, part.params);
    quiet(() => CF.rowsFromTable('name,size\r\nA,3\r\n', part.params));
  }
}

function genTests() {
  const out = [];
  for (const fn of [sketchSections, paramsSections, configSections]) {
    const calls = [];
    fn(recorder(calls));
    out.push({ calls });
  }
  return out;
}

export const GENS = { tests: genTests };
