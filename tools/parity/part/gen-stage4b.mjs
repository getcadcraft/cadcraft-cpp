// SPDX-License-Identifier: Apache-2.0
// pcdiff/gen-stage4b.mjs — stage-4 generators, part 2: standard part tables (shapes/parts.js) and the part templates
// built from them (stdparts.js), and the clipboard (clipboard.js): the frozen "clipboard" section of
// test-partcraft.mjs as written (recorded), then random sketches / parts / assemblies copied, pasted and tampered.
import * as D from './driver.mjs';
import { ri, pick, chance } from './gen-core.mjs';
import { buildSketch } from './gen-sketch.mjs';
import { flange, blockSketch, randomPart } from './gen-part.mjs';
import { A } from './ops-stage4.mjs';

const { S, P, SV } = D;
const SP = await import(D.DEV + 'shapes/parts.js');
const STD = await import(D.DEV + 'stdparts.js');
const C = await import(D.DEV + 'clipboard.js');
const V = await import(D.DEV + 'vec.js');
const clone = (v) => structuredClone(v);
const round = (x, d = 3) => Math.round(x * 10 ** d) / 10 ** d;

async function genParts(r) {
  const calls = [{ op: 'parts.families', args: [] }, { op: 'std.templates', args: [] }];
  for (const f of SP.PART_FAMILIES) {
    calls.push({ op: 'parts.sizes', args: [f.id] });
    for (const z of SP.partSizes(f.id)) {
      calls.push({ op: 'parts.options', args: [f.id, z.size] }, { op: 'parts.make', args: [f.id, z.size, null] });
      if (chance(r, 0.4)) {
        const o = {};
        for (const spec of f.options) o[spec.key] = pick(r, [round(spec.min + r() * (spec.max - spec.min), 2), String(round(spec.min + r() * 10, 1)), '', null, spec.max + 1, spec.min - 1, 7.5, 'x', true, [12]]);
        calls.push({ op: 'parts.make', args: [f.id, pick(r, [z.size, Number(z.size) || z.size]), o] });
      }
    }
    calls.push({ op: 'parts.make', args: [f.id, 'M99', {}] }, { op: 'parts.options', args: [f.id, 'zz'] });
  }
  calls.push({ op: 'parts.make', args: ['nope', 'M6', {}] }, { op: 'parts.sizes', args: ['nope'] });
  return [{ calls }];
}

async function genTemplates(r) {
  const scripts = [];
  for (const tpl of STD.TEMPLATES) {
    const calls = [{ op: 'std.sizes', args: [tpl.id] }];
    const sizes = STD.templateSizes(tpl.id);
    for (const z of [sizes[0], pick(r, sizes), pick(r, sizes)]) calls.push({ op: 'std.make', args: [tpl.id, z.size] });
    calls.push({ op: 'std.make', args: [tpl.id, null] }, { op: 'std.make', args: [tpl.id, 'zz'] });
    scripts.push({ calls });
  }
  scripts.push({ calls: [{ op: 'std.make', args: ['spur-gear', null] }, { op: 'std.sizes', args: ['spur-gear'] }] });
  return scripts;
}

/** The frozen clipboard section, its calls recorded (kernel builds and solver analysis left out). */
function frozenClip() {
  const calls = [];
  const b = blockSketch(100, 50);
  const sk = b.sk;
  SV.solveSketch(sk);
  S.addCircle(sk, [150, 20], 8);
  calls.push({ op: 'clip.copySketch', args: [clone(sk), [...b.lines]] });
  const clip = C.copySketch(sk, [...b.lines]);
  calls.push({ op: 'clip.copySketch', args: [clone(sk), ['O']] }, { op: 'clip.copySketch', args: [clone(sk), [sk.constraints[0].id]] });
  calls.push({ op: 'clip.pasteSketch', args: [clone(sk), clone(clip), [200, 100]] });
  C.pasteSketch(sk, clip, [200, 100]);
  const sk2 = S.newSketch();
  const pt = S.addPoint(sk2, 10, 20);
  S.addConstraint(sk2, 'fix', [pt]);
  const ln = S.addLine(sk2, [0, 0], [30, 0]);
  const dimc = S.addConstraint(sk2, 'distance', [ln.id], 30); dimc.at = [15, -10];
  calls.push({ op: 'clip.copySketch', args: [clone(sk2), [pt, ln.id]] });
  const c2 = C.copySketch(sk2, [pt, ln.id]);
  calls.push({ op: 'clip.pasteSketch', args: [clone(sk2), clone(c2), [5, 7]] });
  calls.push({ op: 'clip.read', args: [JSON.stringify(clip)] });
  const bad = (mut) => { const j = JSON.parse(JSON.stringify(clip)); mut(j); calls.push({ op: 'clip.read', args: [JSON.stringify(j)] }); };
  bad((j) => { j.sketch.constraints[0].refs = ['p999', 'p998']; });
  bad((j) => { j.sketch.entities[0].x = 'x'; });
  bad((j) => { j.sketch.entities.find((e) => e.type === 'line').p1 = 'O'; });
  bad((j) => { j.sketch.constraints.push({ id: 'k900', type: 'coincident', refs: [j.sketch.entities[0].id, 'O'] }); });
  bad((j) => { j.sketch.entities[1].id = j.sketch.entities[0].id; });
  bad((j) => { j.format = 'evil'; });
  bad((j) => { j.kind = 'script'; });
  bad((j) => { j.anchor = [0]; });
  calls.push({ op: 'clip.read', args: ['{not json'] });
  const fl = flange();
  calls.push({ op: 'clip.copyFeature', args: [clone(fl.part), fl.ids.pattern] }, { op: 'clip.copyFeature', args: [clone(fl.part), fl.ids.hole] });
  const hc = C.copyFeature(fl.part, fl.ids.hole);
  calls.push({ op: 'clip.read', args: [JSON.stringify(hc)] });
  const hcBack = C.readClip(JSON.stringify(hc));
  calls.push({ op: 'clip.pasteFeatures', args: [clone(fl.part), clone(hcBack)] });
  const other = P.newPart('Other');
  const bp = blockSketch(100, 50);
  const s1 = P.addFeature(other, 'sketch', { plane: { kind: 'base', name: 'front' }, sketch: bp.sk });
  const e1 = P.addFeature(other, 'extrude', { sketch: s1.id, depth: 20 });
  calls.push({ op: 'clip.copyFeature', args: [clone(other), e1.id] });
  const ec = C.readClip(JSON.stringify(C.copyFeature(other, e1.id)));
  calls.push({ op: 'clip.pasteFeatures', args: [P.newPart('Fresh'), clone(ec)] }, { op: 'clip.pasteFeatures', args: [P.newPart('Empty'), clone(hcBack)] });
  calls.push({ op: 'clip.copyFeature', args: [clone(other), s1.id] });
  calls.push({ op: 'clip.pasteFeatures', args: [clone(other), C.copyFeature(other, s1.id)] });
  const bf = (mut) => { const j = JSON.parse(JSON.stringify(hc)); mut(j); calls.push({ op: 'clip.read', args: [JSON.stringify(j)] }); };
  bf((j) => { j.features[1].sketch = 'f77'; });
  bf((j) => { j.features[1].d = -3; });
  bf((j) => { j.features[0].plane = { kind: 'face', feature: 'f1', face: 'named', name: '<script>' }; });
  bf((j) => { j.features.push(j.features[1]); });
  const asm = A.newAssembly('Pair');
  const i1 = A.insertPart(asm, fl.part);
  calls.push({ op: 'clip.copyInstance', args: [clone(asm), i1.id] }, { op: 'clip.copyInstance', args: [clone(asm), 'i99'] });
  const ic = C.readClip(JSON.stringify(C.copyInstance(asm, i1.id)));
  const place = V.matMul(V.translation([250, 0, 0]), ic.place);
  calls.push({ op: 'clip.pasteInstance', args: [clone(asm), clone(ic), place] });
  const asm2 = A.newAssembly('Other');
  A.insertPart(asm2, other);
  calls.push({ op: 'clip.pasteInstance', args: [clone(asm2), clone(ic), ic.place] });
  const bi = (mut) => { const j = JSON.parse(JSON.stringify(ic)); mut(j); calls.push({ op: 'clip.read', args: [JSON.stringify(j)] }); };
  bi((j) => { j.place = j.place.slice(0, 15); });
  bi((j) => { j.place[12] = 1e10; });
  bi((j) => { j.part.features[1].sketch = 'f99'; });
  bi((j) => { j.part.version = 99; });
  for (const c of [clip, hc, ic, C.copySketch(sk2, [pt]), null]) calls.push({ op: 'clip.describe', args: [c] });
  return calls;
}

async function genClip(r) {
  const scripts = [{ calls: frozenClip() }];
  for (let s = 0; s < 3; s++) {
    const calls = [];
    for (let k = 0; k < 3; k++) {
      const sk = buildSketch(r, [], { clean: chance(r, 0.5) }) || S.newSketch();
      const ids = ['O', ...sk.entities.map((e) => e.id), ...sk.constraints.map((c) => c.id)].filter(() => chance(r, 0.4));
      calls.push({ op: 'clip.copySketch', args: [clone(sk), ids] });
      const clip = C.copySketch(sk, ids);
      if (!clip) continue;
      calls.push({ op: 'clip.pasteSketch', args: [clone(sk), clone(clip), [round(r() * 50, 2), round(r() * 50, 2)]] }, { op: 'clip.pasteSketch', args: [S.newSketch(), clone(clip), null] });
      calls.push({ op: 'clip.read', args: [JSON.stringify(clip)] }, { op: 'clip.describe', args: [clip] });
      for (let i = 0; i < 4; i++) {
        const j = clone(clip);
        const m = r();
        if (m < 0.25) j.version = pick(r, [2, '1', null]);
        else if (m < 0.5) j.dropped = pick(r, [-1, 1.5, 1e7, 'x', 3]);
        else if (m < 0.75) j.anchor = pick(r, [[1, 2, 3], [1e10, 0], ['a', 0], [5, 6]]);
        else if (j.sketch.entities.length) j.sketch.entities[ri(r, 0, j.sketch.entities.length - 1)].id = pick(r, ['O', 'zz', '']);
        calls.push({ op: 'clip.read', args: [chance(r, 0.5) ? JSON.stringify(j) : j] });
      }
    }
    const pt = randomPart(r);
    for (const f of pt.features.slice(0, 6)) {
      calls.push({ op: 'clip.copyFeature', args: [clone(pt), f.id] });
      let c = null; try { c = C.readClip(JSON.stringify(C.copyFeature(pt, f.id))); } catch { c = null; }
      if (c) { calls.push({ op: 'clip.pasteFeatures', args: [clone(pt), clone(c)] }, { op: 'clip.pasteFeatures', args: [P.newPart('X'), clone(c)] }, { op: 'clip.describe', args: [c] }); }
    }
    calls.push({ op: 'clip.copyFeature', args: [clone(pt), 'f99'] });
    calls.push({ op: 'clip.read', args: [pick(r, ['', 'null', '{"format":"partcraft-clip","version":1,"kind":"features","features":[]}', JSON.stringify({ format: 'partcraft-clip', version: 1, kind: 'instance', part: P.toJSON(pt), place: V.translation([1, 2, 3]) })])] });
    scripts.push({ calls });
  }
  return scripts;
}

export const GENS = { parts: genParts, stdtpl: genTemplates, clip: genClip };
