// SPDX-License-Identifier: Apache-2.0
// pcdiff/gen-part.mjs — random parts built through the frozen part.js / sketch.js / params.js API (the Catalog flange
// and the test block from tools/partcraft-test/fixtures.mjs, plus random feature trees with references, parameters,
// configurations, drawings and materials), then the part file both ways, feature editing, the parameter adapter, the
// rollback helpers, and tampered files.
import * as D from './driver.mjs';
import { ri, pick, chance, randExpr, NAMES } from './gen-core.mjs';
import { buildSketch } from './gen-sketch.mjs';

const { S, P, PR, PP } = D;
const round = (x, d = 3) => Math.round(x * 10 ** d) / 10 ** d;

// ---------------------------------------------------------------- fixtures.mjs (frozen 594ab3e)
export function flangeSketch() {
  const sk = S.newSketch();
  const ax = S.addLine(sk, [0, -10], [0, 100], { construction: true });
  S.addConstraint(sk, 'coincident', [ax.p1, 'O']);
  sk.entities.find((e) => e.id === ax.p1).y = 0;
  S.addConstraint(sk, 'vertical', [ax.id]);
  S.addConstraint(sk, 'distance', [ax.id], 100);
  const pts = [[35, 1], [99, -1], [101, 29], [53, 31], [41, 79], [33, 81]];
  const L = [];
  let prev = null, first = null;
  for (let i = 0; i < 6; i++) {
    const a = prev ? prev : pts[i];
    const l = S.addLine(sk, a, pts[(i + 1) % 6]);
    if (!first) first = l;
    L.push(l);
    prev = l.p2;
  }
  S.addConstraint(sk, 'coincident', [L[5].p2, L[0].p1]);
  S.addConstraint(sk, 'horizontal', [L[0].id]);
  S.addConstraint(sk, 'vertical', [L[1].id]);
  S.addConstraint(sk, 'horizontal', [L[2].id]);
  S.addConstraint(sk, 'horizontal', [L[4].id]);
  S.addConstraint(sk, 'vertical', [L[5].id]);
  S.addConstraint(sk, 'coincident', ['O', L[0].id]);
  const dims = {};
  dims.bore = S.addConstraint(sk, 'hdistance', ['O', L[0].p1], 34).id;
  dims.outer = S.addConstraint(sk, 'hdistance', ['O', L[0].p2], 100).id;
  dims.disc = S.addConstraint(sk, 'distance', [L[1].id], 30).id;
  dims.hubBase = S.addConstraint(sk, 'hdistance', ['O', L[2].p2], 52).id;
  dims.hubTop = S.addConstraint(sk, 'hdistance', ['O', L[4].p1], 40).id;
  dims.height = S.addConstraint(sk, 'vdistance', ['O', L[4].p1], 80).id;
  return { sk, axis: ax.id, lines: L.map((l) => l.id), dims };
}
export function flange({ resolution = 64, holes = 8, holeD = 14, boltR = 75 } = {}) {
  const part = P.newPart('Flange');
  part.resolution = resolution;
  const fs = flangeSketch();
  const sk1 = P.addFeature(part, 'sketch', { plane: { kind: 'base', name: 'front' }, sketch: fs.sk });
  const rev = P.addFeature(part, 'revolve', { sketch: sk1.id, axis: fs.axis, op: 'add', angle: 360 });
  const hs = S.newSketch();
  const p = S.addPoint(hs, boltR + 2, 1);
  S.addConstraint(hs, 'hdistance', ['O', p], boltR);
  S.addConstraint(hs, 'vdistance', ['O', p], 0);
  const sk2 = P.addFeature(part, 'sketch', { plane: { kind: 'face', feature: rev.id, face: 'side', entity: fs.lines[2] }, sketch: hs });
  const hole = P.addFeature(part, 'hole', { sketch: sk2.id, kind: 'simple', d: holeD, through: true });
  const pat = P.addFeature(part, 'cpattern', { seeds: [hole.id], axis: { kind: 'axis', feature: rev.id }, count: holes, angle: 360 });
  return { part, ids: { profile: sk1.id, revolve: rev.id, holeSketch: sk2.id, hole: hole.id, pattern: pat.id }, dims: fs.dims, lines: fs.lines };
}
export function blockSketch(w = 100, h = 50) {
  const sk = S.newSketch();
  const r = S.addRect(sk, [3, 4], [w - 5, h + 3]);
  const l0 = sk.entities.find((e) => e.id === r.lines[0]);
  S.addConstraint(sk, 'coincident', [l0.p1, 'O']);
  const dw = S.addConstraint(sk, 'distance', [r.lines[0]], w).id;
  const dh = S.addConstraint(sk, 'distance', [r.lines[1]], h).id;
  return { sk, lines: r.lines, dw, dh };
}

// ---------------------------------------------------------------- random parts
const faceName = (r, ids, lines) => pick(r, ids) + pick(r, ['.start', '.end', '.side:' + pick(r, lines.length ? lines : ['l1']), '.wall:p3', '.blend:0', '.face:2']) + (chance(r, 0.2) ? '@' + pick(r, ids) + '#' + ri(r, 1, 5) : '') + (chance(r, 0.1) ? '~' + pick(r, ids) : '');
export function randomPart(r) {
  const k = r();
  let part, lines = [];
  if (k < 0.25) ({ part, lines } = flange({ resolution: pick(r, [64, 32, 48, 96]), holes: ri(r, 1, 9), holeD: round(4 + r() * 12, 2) }));
  else {
    part = P.newPart(pick(r, ['Bracket', 'Part1', '  \u0001x', '', 'Blöck', 'x'.repeat(130)]));
    part.resolution = pick(r, [64, 32, 8, 100, 512]);
    const b = blockSketch(round(50 + r() * 100, 1), round(20 + r() * 60, 1));
    lines = b.lines;
    const sk1 = P.addFeature(part, 'sketch', { plane: { kind: 'base', name: pick(r, ['top', 'front', 'right']) }, sketch: b.sk });
    const ex = P.addFeature(part, 'extrude', { sketch: sk1.id, depth: round(5 + r() * 40, 2), end: pick(r, ['blind', 'blind', 'mid']), reverse: chance(r, 0.2) });
    if (chance(r, 0.6)) {
      const hs = S.newSketch();
      for (let i = 0; i < ri(r, 1, 3); i++) S.addPoint(hs, round(10 + r() * 30, 2), round(5 + r() * 20, 2));
      const sk2 = P.addFeature(part, 'sketch', { plane: { kind: 'face', feature: ex.id, face: pick(r, ['end', 'start', 'side']), ...(chance(r, 0.5) ? { entity: pick(r, lines) } : {}) }, sketch: hs });
      const hk = pick(r, ['simple', 'cbore', 'csink', 'tapped']);
      const hp = { sketch: sk2.id, kind: hk, d: round(3 + r() * 6, 2), through: chance(r, 0.6), depth: round(2 + r() * 10, 2) };
      if (hk === 'tapped') Object.assign(hp, { size: pick(r, ['M3', 'M6', 'M10']), threadDepth: chance(r, 0.5) ? round(r() * 8 + 1, 1) : null, ...(chance(r, 0.4) ? { modelled: true, clearance: round(r() * 0.3, 2) } : {}) });
      if (hk === 'csink') Object.assign(hp, { csinkD: round(8 + r() * 6, 2), csinkAngle: pick(r, [90, 82]) });
      const hole = P.addFeature(part, 'hole', hp);
      if (chance(r, 0.5)) P.addFeature(part, pick(r, ['lpattern', 'cpattern']), { seeds: [hole.id], count: ri(r, 1, 6), spacing: round(r() * 30, 1), angle: round(r() * 360, 1) || 360, ...(chance(r, 0.2) ? { dir: { kind: 'line', sketch: sk1.id, entity: lines[0] } } : {}) });
    }
    const ids = part.features.map((f) => f.id);
    const extra = ri(r, 0, 4);
    for (let i = 0; i < extra; i++) {
      const t = pick(r, ['mirror', 'fillet', 'chamfer', 'shell', 'plane', 'axis', 'point', 'thread', 'revolve', 'extrude', 'sketch', 'rib', 'sheetbase', 'combine', 'draft', 'loft', 'sweep']);
      const props = {};
      if (t === 'mirror') Object.assign(props, { seeds: chance(r, 0.4) ? 'body' : [pick(r, ids)], plane: pick(r, [{ kind: 'base', name: 'right' }, { kind: 'face', feature: ex.id, face: 'end' }, { kind: 'fixed', o: [1, 2, 3], n: [0, 0, 1] }]) });
      if (t === 'fillet' || t === 'chamfer') {
        Object.assign(props, { edges: [{ faces: [faceName(r, ids, lines), faceName(r, ids, lines)], ...(chance(r, 0.7) ? { at: [round(r() * 50, 2), round(r() * 50, 2), 0] } : {}) }] });
        if (t === 'fillet') { props.r = round(0.5 + r() * 4, 2); if (chance(r, 0.2)) props.r2 = round(r() * 5 + 0.1, 2); }
        else { props.d = round(0.5 + r() * 3, 2); if (chance(r, 0.3)) Object.assign(props, { mode: pick(r, ['two', 'angle', 'equal']), d2: round(r() * 2 + 0.1, 2), angle: round(r() * 80 + 1, 1) }); }
      }
      if (t === 'shell') Object.assign(props, { faces: [{ name: faceName(r, ids, lines) }], t: round(0.5 + r() * 3, 2), outward: chance(r, 0.3) });
      if (t === 'plane') Object.assign(props, { mode: pick(r, ['offset', 'angle', 'mid']), planes: [{ kind: 'base', name: 'top' }, ...(chance(r, 0.5) ? [{ kind: 'face', feature: ex.id, face: 'end' }] : [])], d: round(r() * 50, 2) });
      if (t === 'axis') Object.assign(props, { mode: pick(r, ['planes', '2pt']), points: chance(r, 0.5) ? [{ kind: 'fixed', p: [0, 0, 0] }, { kind: 'sketch', sketch: sk1.id, entity: 'O' }] : [] });
      if (t === 'point') Object.assign(props, { mode: pick(r, ['xyz', 'at', 'mid']), p: [round(r() * 9, 1), 1, 2], ...(chance(r, 0.3) ? { planes: [{ kind: 'base', name: 'front' }] } : {}) });
      if (t === 'thread') Object.assign(props, { faces: [{ name: faceName(r, ids, lines) }], form: pick(r, ['iso', 'tr']), size: pick(r, ['auto', 'M10', 'Tr16x4']), length: chance(r, 0.5) ? null : round(r() * 20 + 1, 1), modelled: chance(r, 0.3) });
      if (t === 'revolve') Object.assign(props, { sketch: sk1.id, axis: pick(r, [null, lines[0], { kind: 'world', axis: 'y' }, { kind: 'fixed', o: [0, 0, 0], d: [0, 1, 0] }]), angle: round(r() * 359 + 1, 1), op: pick(r, ['add', 'cut', 'new', 'intersect']) });
      if (t === 'extrude') Object.assign(props, { sketch: sk1.id, op: pick(r, ['cut', 'add', 'new']), end: pick(r, ['through', 'blind', 'upto', 'offset']), depth: round(r() * 20 + 1, 1), ...(chance(r, 0.3) ? { two: true, depth2: round(r() * 9 + 1, 1) } : {}), ...(chance(r, 0.2) ? { taper: round(r() * 10, 1) } : {}), ...(chance(r, 0.2) ? { regions: [[1, 2]] } : {}), ...(chance(r, 0.2) ? { upto: { kind: 'base', name: 'top' } } : {}) });
      if (t === 'sketch') Object.assign(props, { plane: pick(r, [{ kind: 'face', feature: ex.id, face: 'named', name: ex.id + '.end' }, { kind: 'base', name: 'right' }]), sketch: buildSketch(r, []) });
      if (t === 'rib') Object.assign(props, { sketch: sk1.id, t: round(r() * 4 + 0.5, 1), side: pick(r, ['mid', 'front', 'back']) });
      if (t === 'sheetbase') Object.assign(props, { sketch: sk1.id });
      if (t === 'combine') Object.assign(props, { target: ex.id, tools: [pick(r, ids)], op: pick(r, ['join', 'cut']) });
      if (t === 'draft') Object.assign(props, { faces: [{ name: faceName(r, ids, lines) }], angle: round(r() * 10, 1) });
      if (t === 'loft') Object.assign(props, { sections: [{ sketch: sk1.id }, { point: { kind: 'fixed', p: [0, 50, 0] } }] });
      if (t === 'sweep') Object.assign(props, { sketch: sk1.id, path: [{ sketch: sk1.id, entities: [lines[0]] }] });
      try { P.addFeature(part, t, props); } catch { /* ignore */ }
    }
    for (const f of part.features) if (chance(r, 0.08)) f.suppressed = true;
  }
  // parameters and equations
  if (chance(r, 0.6)) {
    const p = PP.paramsOf(part);
    const names = [];
    for (let i = 0; i < ri(r, 1, 4); i++) { const n = pick(r, NAMES); names.push(n); PR.setVar(p, n, chance(r, 0.6) ? String(round(r() * 50 + 1, 2)) : randExpr(r, names.slice(0, -1))); }
    const keys = [...PP.partAdapter.targets(part).keys()];
    for (let i = 0; i < ri(r, 0, 3) && keys.length; i++) PR.setLink(p, PP.partAdapter, part, pick(r, keys), pick(r, names) + (chance(r, 0.5) ? ' * ' + ri(r, 1, 3) : ''));
    PP.tidyParams(part);
    if (part.params && chance(r, 0.4)) {
      const row = D.CF.rowFromCurrent(part.params, 'Base');
      part.configs = D.CF.saveRow(D.CF.saveRow(null, row), { name: 'Alt', values: Object.fromEntries(Object.keys(row.values).map((k) => [k, round(r() * 40 + 1, 1)])) });
      if (chance(r, 0.5)) part.configs.active = 'Base';
    }
  }
  if (chance(r, 0.3)) part.drawing = { section: chance(r, 0.5) ? { of: pick(r, ['front', 'top', 'right']), ...(chance(r, 0.5) ? { at: round(r() * 20, 1) } : {}) } : null, detail: chance(r, 0.3) ? { of: 'front', at: [1, 2], r: 5, scale: pick(r, [2, 4, 5, 10]) } : null, notes: chance(r, 0.5) ? [{ id: 'g1', kind: 'datum', view: 'front', at: [0, 0], off: [12, 10], letter: 'A' }] : [], ...(chance(r, 0.4) ? { sheet: pick(r, ['a4', 'a3', 'ansi-b']) } : {}), ...(chance(r, 0.3) ? { title: { title: 'T\u0001x', date: pick(r, ['2026-10-17', '']), rev: 'B' } } : {}), ...(chance(r, 0.3) ? { revisions: [{ rev: 'A', desc: 'first', date: '2026-01-01', by: 'me' }] } : {}), ...(chance(r, 0.2) ? { holeTable: { view: 'top' } } : {}) };
  if (chance(r, 0.2)) part.material = { name: pick(r, ['Steel', '', ' Brass ']), density: pick(r, [7.85, 8.5, 1.2]) };
  if (chance(r, 0.15)) part.bodyInfo = { [part.features[0]?.id || 'f1']: { name: 'Main', hidden: chance(r, 0.5) } };
  if (chance(r, 0.25)) part.rollback = ri(r, 0, part.features.length);
  return part;
}

function mutate(r, j) {
  const t = structuredClone(j);
  const f = t.features.length ? pick(r, t.features) : null;
  const k = r();
  const junk = () => pick(r, [null, 0, -1, 1e10, 'x', '', true, [], {}, [1, 2, 3], { kind: 'base', name: 'side' }, 'f1', NaN]);
  if (k < 0.04) t.format = 'partcraft2';
  else if (k < 0.07) t.version = pick(r, [0, 4, 2.5, '3', null]);
  else if (k < 0.1) t.features = 'x';
  else if (k < 0.13) t.resolution = pick(r, [7, 513, 63.5, '64']);
  else if (k < 0.16) t.rollback = pick(r, [-1, 1.5, t.features.length, 99, 'x', 0]);
  else if (k < 0.19) t.name = junk();
  else if (k < 0.62 && f) {
    const keys = Object.keys(f);
    const key = pick(r, keys.concat(['id', 'type', 'sketch', 'plane', 'op', 'end', 'depth', 'axis', 'seeds', 'edges', 'faces', 'kind', 'size', 'count', 'bodies', 'dir']));
    if (chance(r, 0.25)) delete f[key]; else f[key] = junk();
  } else if (k < 0.66 && f) f.id = pick(r, ['f0', 'g1', 'f1234567890', t.features[0].id]);
  else if (k < 0.7 && f) f.type = pick(r, ['nope', 'constructor', 'toString', 'sketch', '']);
  else if (k < 0.74) t.params = pick(r, [[1], { vars: [{ name: 'sin', expr: '1' }] }, { links: [{ target: 'f99.depth', expr: '1' }] }, { vars: 'x' }, { vars: [{ name: 'w', expr: 'w +' }] }]);
  else if (k < 0.78) t.configs = pick(r, [{ rows: [{ name: 'A', values: { ghost: 1 } }] }, { rows: 'x' }, { rows: [] }, { rows: [{ name: '', values: {} }] }]);
  else if (k < 0.82) t.drawing = pick(r, [5, { section: { of: 'iso' } }, { detail: { of: 'front', at: [1], r: 1, scale: 3 } }, { sheet: 'a9' }, { title: 'x' }, { title: { date: '17/10/2026' } }, { revisions: [{ rev: 1 }] }, { holeTable: { view: 'iso' } }, { notes: [{ id: 'g1' }] }]);
  else if (k < 0.85) t.material = pick(r, [{ density: 0 }, 'steel', { name: 'x', density: 30 }]);
  else if (k < 0.88) t.studies = pick(r, [{ simulation: {} }, 'x', {}, Object.fromEntries(Array.from({ length: 33 }, (_, i) => ['k' + i, 1]))]);
  else if (k < 0.91) t.bodyInfo = pick(r, [{ 'f1:2': { name: 5 } }, { x: {} }, [], { f99: { name: 'gone' } }]);
  else if (k < 0.95 && f && f.sketch && typeof f.sketch === 'object') {
    const sk = f.sketch;
    if (sk.entities.length) { const e = pick(r, sk.entities); const ek = pick(r, Object.keys(e)); e[ek] = junk(); }
  } else if (f) f.name = junk();
  return t;
}

function genPart(r) {
  const scripts = [];
  for (let s = 0; s < 10; s++) {
    const calls = [];
    for (let k = 0; k < 3; k++) {
      const part = randomPart(r);
      const text = P.toJSON(part);
      const j = JSON.parse(text);
      calls.push({ op: 'part.toJSON', args: [part] }, { op: 'part.fileVersion', args: [part] }, { op: 'part.builtCount', args: [part] });
      calls.push({ op: 'part.read', args: [j] }, { op: 'part.read', args: [text] });
      let read = null; try { read = P.readPart(text); } catch { read = null; }
      const pt = read || part;
      if (read) calls.push({ op: 'part.toJSON', args: [read] });
      for (const f of pt.features) calls.push({ op: 'part.dependsOn', args: [f] });
      const ids = pt.features.map((f) => f.id);
      if (ids.length) {
        calls.push({ op: 'part.dependents', args: [pt, pick(r, ids)] });
        for (let i = 0; i < 3; i++) calls.push({ op: 'part.canMove', args: [pt, pick(r, ids.concat(['f99'])), ri(r, -1, ids.length)] });
        calls.push({ op: 'part.moveFeature', args: [pt, pick(r, ids), ri(r, 0, ids.length - 1)] });
        const del = ids.filter(() => chance(r, 0.25));
        calls.push({ op: 'part.deleteFeatures', args: [pt, del] });
        const after = structuredClone(pt); P.deleteFeatures(after, del);
        calls.push({ op: 'pp.pruneDeleted', args: [pt, after] });
      }
      const type = pick(r, ['sketch', 'extrude', 'hole', 'lpattern', 'plane', 'form', 'thread', 'sheetflange', 'gordon', 'nope', 'movebody', 'import']);
      calls.push({ op: 'part.addFeature', args: [pt, type, chance(r, 0.5) ? { depth: 7, name: 'Mine', extra: [1] } : {}] });
      // the adapter
      calls.push({ op: 'pp.targets', args: [pt] }, { op: 'pp.report', args: [pt] });
      calls.push({ op: 'pp.apply', args: [pt, null] });
      const keys = [...PP.partAdapter.targets(pt).keys()];
      const names = (pt.params?.vars || []).map((v) => v.name);
      if (keys.length) {
        const key = pick(r, keys);
        calls.push({ op: 'pp.setLink', args: [pt, key, names.length && chance(r, 0.7) ? pick(r, names) + ' + 1' : String(round(r() * 30, 1))] });
        calls.push({ op: 'pp.driveBack', args: [pt, pick(r, (pt.params?.links || []).map((l) => l.target).concat([key])), round(r() * 60 - 5, 2), null] });
        calls.push({ op: 'pp.targetName', args: [pt, key] });
        const tn = PP.targetName(pt, key);
        for (const text of [key, tn, tn.replace('.', ' '), tn.toLowerCase(), 'Nope.d', 'Sketch1.D1', 'Hole1 diameter', 'Extrude1.depth', ' f1.k5 ']) calls.push({ op: 'pp.resolveTarget', args: [pt, text] });
      }
      if (names.length) {
        calls.push({ op: 'pp.setVar', args: [pt, pick(r, names), String(round(r() * 40 + 1, 2))] });
        let p2 = structuredClone(pt); PR.setVar(PP.paramsOf(p2), pick(r, names), String(round(r() * 40 + 1, 2)));
        calls.push({ op: 'pp.apply', args: [p2, null] });
        const ov = { [pick(r, names)]: round(r() * 20, 1) };
        calls.push({ op: 'pp.apply', args: [pt, ov] });
      }
      for (const f of pt.features) for (const fld of ['depth', 'd', 'count', 'angle', 'r', 'reliefWidth', 'dx', 's', 'sx', 'depth2']) if (typeof f[fld] === 'number') calls.push({ op: 'pp.fieldInUse', args: [f, fld] });
      for (const d of [-2, -1, 1, 3]) calls.push({ op: 'pp.stepRollback', args: [pt, d] });
      calls.push({ op: 'pp.rollTo', args: [pt, ids.length ? pick(r, ids.concat([null, 'f99'])) : null] }, { op: 'pp.rollbackWords', args: [pt] });
      // tampered files
      for (let i = 0; i < 12; i++) calls.push({ op: 'part.read', args: [mutate(r, j)] });
      calls.push({ op: 'part.read', args: [pick(r, ['', '{', 'null', '[]', '{"format":"partcraft","version":3,"features":[]}', JSON.stringify({ ...j, extra: 1 })])] });
    }
    scripts.push({ calls });
  }
  return scripts;
}

export const GENS = { part: genPart };
