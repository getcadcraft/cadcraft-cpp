// SPDX-License-Identifier: Apache-2.0
// pcdiff/gen-stage2.mjs — stage-2 generators: .pcscript both ways (random parts written, read back, round-tripped,
// then broken line by line), hand-made scripts covering every statement and setting, parameter uses, the sketch tools
// on random sketches, curve intersection and vec.js.
import * as D from './driver.mjs';
import { ri, pick, chance, randExpr } from './gen-core.mjs';
import { buildSketch } from './gen-sketch.mjs';
import { randomPart, flange } from './gen-part.mjs';

const { S, P, PR, PP } = D;
const SC = await import(D.DEV + 'script.js');
const round = (x, d = 3) => Math.round(x * 10 ** d) / 10 ** d;
const rp = (r, s = 100) => [round((r() - 0.5) * s, 2), round((r() - 0.5) * s, 2)];
const rv3 = (r) => [round(r() * 2 - 1, 3), round(r() * 2 - 1, 3), round(r() * 2 - 1, 3)];

// ---------------------------------------------------------------- scripts written by hand
const HAND = [
  `pcscript 1\npart "Bracket"\n  resolution 48\n\nparam w = 80\nparam h = w / 2\n  note "half of w"\n\nsketch Base\n  on top\n  point p1 0, 0\n  point p2 80, 0\n  point p3 80, 40\n  point p4 0, 40\n  line l1 p1 p2\n  line l2 p2 p3\n  line l3 p3 p4\n  line l4 p4 p1\n  coincident p1 O\n  horizontal l1\n  vertical l2\n  horizontal l3\n  vertical l4\n  distance l1 = w\n  distance l2 = h\n\nextrude Pad\n  sketch Base\n  depth = h / 4\n  end blind\n\nfillet Round\n  edge Pad.side.l1 Pad.end\n  edge Pad.side.l2 Pad.end [at 80, 10, 0]\n  r 2\n\nrollback Pad\n`,
  `pcscript 1\npart Disc\nsketch S1\n  on front\n  point c 0, 0\n  circle k c 25\n  coincident c O\n  diameter k 50 [label 10, 10]\nrevolve R1\n  sketch S1\n  axis y\n  angle 270\n`,
  `pcscript 1\npart P\nsketch S\n  on fixed 0, 0, 10 normal 0, 0, 1\n  point a 0, 0\n  point b 10, 0\n  point c 10, 10\n  arc r1 a b c [construction]\n  spline s1 fit a b c [closed]\n  d1: distance a b 10\n  offsetlink a b [of d1]\nextrude E\n  sketch S\n  depth 5\n  reverse\n  suppressed\n`,
  `pcscript 1\npart "Holes"\nsketch Plate\n  on right\n  point a -20, -10\n  point b 20, 10\n  line l1 a b\nextrude Body\n  sketch Plate\n  depth 10\nsketch HS\n  on face Body end\n  point h1 5, 5\nhole H\n  sketch HS\n  kind tapped\n  size M6\n  through yes\nlpattern LP\n  seeds H\n  dir line Plate l1\n  count 3\n  spacing 12\ncpattern CP\n  seeds H, LP\n  axis x\n  count 4\n  angle 360\nmirror M\n  seeds body\n  plane face Body side l1\nmaterial json {"name":"Steel","density":7.85}\n`,
  `pcscript 1\npart X\nimport Imp\n  step-file "parts/bolt.step"\nimport Imp2\n  file "keep.step"\n  step-file other.step\n`,
  `pcscript 2\npart X\n`,
  `pcscript 1\npart X\nparam 1a = 3\nparam w = 3 +\nparam w = 4\nparam w = 5\nwidget W\n  depth 3\n`,
  `  depth 3\npcscript 1\npart X\n  units in\n  resolution 7\n  colour red\n`,
  `pcscript 1\npart X\nextrude E\n  sketch Nope\n  depth = q + 1\n  depth 4\n  hello world\n  edge a\n  face a b\n  face x [at 1, 2]\n  face y [bad 1]\n`,
  `pcscript 1\npart X\nsketch S\n  on top\n  point p1 0, 0\n  point p1 1, 1\n  line l1 p1 p9\n  circle c1 p1\n  arc a1 p1\n  spline s1 wrong p1\n  bogus p1\n  horizontal p1 = 3\n  distance p1 -4\n  angle p1 p1 200\n  radius c1 0\n  fix p1 [value 1, 2]\n  coincident p1 O [side 2]\n  point "q" 1, 2\n  x: horizontal p1 [ext]\n`,
  `pcscript 1\npart X\nsketch S\n  on top\n  point p1 0, 0\n  point p2 10, 0\n  line l1 p1 p2\n  distance l1 10\n  distance l1 20\n  horizontal l1\n  vertical l1\n`,
  `pcscript 1\npart "x"   # comment\n# full comment\nrollback end\nrollback first\ndrawing json {"sheets":[]}\nconfigs json {"rows":[]}\nconfigs json {bad\n`,
  `﻿pcscript 1\r\npart "Win"\r\nsketch S\r\n  on top\r\n  point p 1, 2\r\n`,
  `pcscript 1\npart X\nplane P1\n  plane json {"kind":"base","name":"top"}\n  offset = 3 * 2\naxis A1\n  ref P1\npoint Pt\n  at 1, 2, 3\nthread T\n  form metric\n  size M8\nshell Sh\n  face top [at 0, 1, 0]\n  t 1\n`,
  `pcscript 1\npart X\nsketch S\n  on face named Pad.end\n  on plane Nope\n  point a 0, 0 [construction; weird]\n  line l a a [construction]\n`,
  'part X\nextrude "E 1"\n  id f7\nextrude E2\n  id f7\nextrude E3\n  id g1\nchamfer C\n  edge a b\n  d 1\n',
  'pcscript 1\npart X\nparam a = b\nparam b = a\n',
  '',
  'pcscript one\npart',
  '{"format":"partcraft"}',
  'pcscript 1\npart X\nsketch S\n  on top\n  "unclosed\n',
  'pcscript 1\npart X\nsketch S\n  on top\n  point p 1, 2 [construction\n',
];

function mutateScript(r, text) {
  const lines = text.split('\n');
  const n = ri(r, 1, 3);
  for (let k = 0; k < n; k++) {
    const i = ri(r, 0, Math.max(0, lines.length - 1));
    const m = r();
    if (m < 0.15) lines.splice(i, 1);
    else if (m < 0.3) lines.splice(i, 0, lines[ri(r, 0, lines.length - 1)] || '');
    else if (m < 0.42) lines[i] = (lines[i] || '').replace(/\d+(\.\d+)?/, () => pick(r, ['-1', '0', '1e400', '(2+3)', 'w*2', 'NaN', '"7"', '= 3 +']));
    else if (m < 0.52) lines[i] = (lines[i] || '').replace(/^  /, '');
    else if (m < 0.62) lines[i] = '  ' + (lines[i] || '');
    else if (m < 0.7) lines[i] = (lines[i] || '') + pick(r, [' extra', ' [at 1, 2, 3]', ' # c', ' "q"', ' =', '\u0001', ' json {']);
    else if (m < 0.78) lines[i] = (lines[i] || '').split(' ').reverse().join(' ');
    else if (m < 0.86) { const j = ri(r, 0, lines.length - 1); [lines[i], lines[j]] = [lines[j], lines[i]]; }
    else lines[i] = (lines[i] || '').replace(/[A-Za-z]+/, () => pick(r, ['sketch', 'extrude', 'id', 'on', 'point', 'line', 'distance', 'Q', 'k1', 'O']));
  }
  return lines.join('\n');
}

function withParams(r, part) {
  part = structuredClone(part);
  if (chance(r, 0.6) && !part.params) {
    const names = ['w', 'h', 'n'];
    part.params = { vars: names.map((nm, i) => ({ name: nm, expr: i ? `w / ${i + 1}` : String(ri(r, 10, 90)) })), links: [] };
    const keys = [...PP.partAdapter.targets(part).keys()];
    for (let i = 0; i < Math.min(3, keys.length); i++) {
      try { PR.setLink(part.params, PP.partAdapter, part, pick(r, keys), pick(r, ['w', 'h + 1', 'n * 2', '3'])); } catch { /* */ }
    }
    try { PP.applyPartParams(part); } catch { /* */ }
  }
  return part;
}

async function genPcscript(r) {
  const scripts = [];
  for (let s = 0; s < 4; s++) {
    const calls = [];
    for (let k = 0; k < 3; k++) {
      let part = s === 0 && k === 0 ? flange({ holes: ri(r, 1, 8) }).part : randomPart(r);
      try { part = P.readPart(P.toJSON(part)); } catch { /* keep */ }
      part = withParams(r, part);
      for (const o of [{}, { exact: true }, { comment: false }, { inlineStep: true }]) calls.push({ op: 'pcscript.write', args: [part, o] });
      let w;
      try { w = SC.partToScript(part, { exact: chance(r, 0.5) }); } catch { continue; }
      const files = Object.fromEntries(w.files.map((f) => [f.name, f.text]));
      calls.push({ op: 'pcscript.looks', args: [w.text] });
      calls.push({ op: 'pcscript.read', args: [w.text, files] });
      calls.push({ op: 'pcscript.read', args: [w.text, null] });
      let back = null;
      try { back = SC.scriptToPart(w.text, { readFile: (p) => files[p] }).part; } catch { back = null; }
      if (back) calls.push({ op: 'pcscript.write', args: [back, {}] });
      for (let i = 0; i < 8; i++) calls.push({ op: 'pcscript.read', args: [mutateScript(r, w.text), files] });
    }
    scripts.push({ calls });
  }
  const calls = [];
  const files = { 'parts/bolt.step': 'ISO-10303-21;\nEND-ISO-10303-21;\n', 'other.step': 'X' };
  for (const h of HAND) {
    calls.push({ op: 'pcscript.looks', args: [h] }, { op: 'pcscript.read', args: [h, files] }, { op: 'pcscript.read', args: [h, null] });
    for (let i = 0; i < 4; i++) calls.push({ op: 'pcscript.read', args: [mutateScript(r, h), files] });
    let back = null;
    try { back = SC.scriptToPart(h, { readFile: (p) => files[p] }).part; } catch { back = null; }
    if (back) calls.push({ op: 'pcscript.write', args: [back, {}] }, { op: 'pcscript.write', args: [back, { exact: true, inlineStep: true }] });
  }
  for (const t of ['  # x\n\nsketch', '{', ' { ', '#a\nfoo', 'param', 'constructor', '﻿\r\npart', 'a\rpart']) calls.push({ op: 'pcscript.looks', args: [t] });
  scripts.push({ calls });
  return scripts;
}

async function genUses(r) {
  const calls = [];
  for (let k = 0; k < 6; k++) {
    let part = withParams(r, randomPart(r));
    if (!part.params) part.params = { vars: [], links: [] };
    const names = ['w', 'h', 'n', 'a1', 'gap'];
    for (const nm of names) if (chance(r, 0.6) && !part.params.vars.some((v) => v.name === nm)) part.params.vars.push({ name: nm, expr: randExpr(r, names) });
    part.params.links.push({ target: 'f1.nope', expr: pick(r, names) + ' + ' + pick(r, names) });
    if (chance(r, 0.6)) part.configs = { rows: [{ name: 'A', values: { w: 3, zz: 1 } }, { name: 'B', values: { h: 2, n: 1 } }] };
    if (chance(r, 0.6)) part.studies = { script: { scripts: [{ name: 's1', text: "x = param('w') + param(\"h\")\nparam('w')" }, { name: 's2', text: 'params()' }, { name: 's3', text: "xparam('n') param ( 'gap' ) param('a1\")" }] } };
    calls.push({ op: 'uses.paramUses', args: [part] });
  }
  for (const t of ["param('w')", 'params ( )', "param( 'a_b' )", "_param('x')", "param('" + 'a'.repeat(41) + "')", "param('" + 'a'.repeat(40) + "')", "param(\"x')", "cs.param('y') params()", 'paramsx()', "param('1x')", "param('é')", '']) calls.push({ op: 'uses.scriptParams', args: [t] });
  return [{ calls }];
}

async function genTools(r) {
  const scripts = [];
  for (let s = 0; s < 4; s++) {
    const calls = [];
    let sk = buildSketch(r, [], { clean: chance(r, 0.5) }) || S.newSketch();
    for (let step = 0; step < 6; step++) {
      const curves = sk.entities.filter((e) => e.type !== 'point');
      const lines = curves.filter((e) => e.type === 'line');
      if (!curves.length) {
        try { S.addRect(sk, rp(r), rp(r)); } catch { /* */ }
        continue;
      }
      const c = pick(r, curves);
      calls.push({ op: 'tools.crossings', args: [sk, c.id] });
      for (let i = 0; i < 3; i++) calls.push({ op: 'tools.trimPiece', args: [sk, c.id, rp(r)] });
      calls.push({ op: 'tools.chainOf', args: [sk, c.id, chance(r, 0.3) ? curves.map((e) => e.id).filter(() => chance(r, 0.6)) : null] });
      let chain = null;
      try { chain = D.EXTOPS['tools.chainOf'](sk, c.id, null); } catch { chain = null; }
      if (chain) {
        calls.push({ op: 'tools.offsetSide', args: [sk, chain, rp(r)] });
        calls.push({ op: 'tools.offset', args: [sk, chain, pick(r, [round(r() * 10, 2), -round(r() * 5, 2), 0, 1000]), null] });
      }
      const ids = curves.map((e) => e.id).filter(() => chance(r, 0.5));
      if (lines.length) calls.push({ op: 'tools.mirror', args: [sk, ids, pick(r, lines).id] });
      calls.push({ op: 'tools.patternLinear', args: [sk, ids, rp(r, 40), pick(r, [2, 3, 1, 2.5, 201])] });
      calls.push({ op: 'tools.patternCircular', args: [sk, ids, rp(r), pick(r, [2, 4, 6]), pick(r, [360, 90, 0, null])] });
      calls.push({ op: 'tools.addPolygon', args: [sk, rp(r), rp(r), pick(r, [3, 6, 2, 65, 8])] });
      calls.push({ op: 'tools.addSlot', args: [sk, rp(r), rp(r), pick(r, [4, 10, 0])] });
      const frame = pick(r, [{ o: [0, 0, 0], x: [1, 0, 0], y: [0, 1, 0], n: [0, 0, 1] }, { o: [0, 5, 0], x: [1, 0, 0], y: [0, 0, -1], n: [0, 1, 0] }]);
      const edge = pick(r, [
        { type: 'line', a: [...rp(r), 0], b: [...rp(r), 3] },
        { type: 'line', a: [1, 1, 1], b: [1, 1, 1] },
        { type: 'circle', a: [10, 0, 0], b: [10, 0, 0], circ: { c: [0, 0, 0], n: [0, 0, 1], r: 10 } },
        { type: 'circle', a: [10, 0, 0], b: [0, 10, 0], circ: { c: [0, 0, 0], n: pick(r, [[0, 0, 1], [0, 0, -1], [0, 1, 0]]), r: 10 } },
        { type: 'curve', a: [0, 0, 0], b: [1, 1, 1] },
        null,
      ]);
      calls.push({ op: 'tools.projectEdge', args: [sk, frame, edge] });
      for (let i = 0; i < 4 && curves.length > 1; i++) {
        const a = pick(r, curves).id, b = pick(r, curves).id;
        calls.push({ op: 'isect.intersect', args: [sk, a, b, chance(r, 0.3), chance(r, 0.3)] });
      }
      calls.push({ op: 'isect.probe', args: [sk, c.id, [0, 0.25, 0.5, 1, round(r() * 7, 3)], [rp(r), rp(r)]] });
      // apply one edit and go on
      const k = r();
      const at = rp(r);
      const op = k < 0.35 ? 'tools.trim' : k < 0.55 ? 'tools.extend' : k < 0.7 && chain ? 'tools.offset' : k < 0.85 && lines.length ? 'tools.mirror' : 'tools.patternLinear';
      const args = op === 'tools.trim' || op === 'tools.extend' ? [sk, c.id, at] : op === 'tools.offset' ? [sk, chain, round(1 + r() * 5, 2), null] : op === 'tools.mirror' ? [sk, ids, pick(r, lines).id] : [sk, ids, rp(r, 30), 2];
      calls.push({ op, args });
      try { const res = D.EXTOPS[op](...args.map((x) => structuredClone(x))); sk = res.sk; } catch { /* unchanged */ }
      if (sk.entities.length > 400) break;
    }
    scripts.push({ calls });
  }
  return scripts;
}

async function genVec(r) {
  const calls = [{ op: 'vec.basePlanes', args: [] }];
  for (let i = 0; i < 30; i++) {
    const n = pick(r, [rv3(r), [0, 1, 0], [0, -1, 0], [0, 0, 0], [1e-200, 0, 0], [0.1, 0.95, 0]]);
    const p = rv3(r);
    calls.push({ op: 'vec.frameFor', args: [n, p] }, { op: 'vec.norm', args: [n] });
    const f = D.EXTOPS['vec.frameFor'](n, p);
    calls.push({ op: 'vec.frameMatrix', args: [f] }, { op: 'vec.toPlane', args: [f, rv3(r)] }, { op: 'vec.toWorld', args: [f, r() * 9, r() * 9] }, { op: 'vec.flipFrame', args: [f] });
    const m1 = D.EXTOPS['vec.rotationAbout'](p, n, r() * 360), m2 = D.EXTOPS['vec.reflection'](rv3(r), rv3(r));
    calls.push({ op: 'vec.rotationAbout', args: [p, n, round(r() * 720 - 360, 2)] }, { op: 'vec.reflection', args: [p, n] });
    calls.push({ op: 'vec.matMul', args: [m1, m2] }, { op: 'vec.applyMat', args: [m1, rv3(r)] }, { op: 'vec.applyDir', args: [m2, rv3(r)] });
    const k = ri(r, 1, 5);
    const A = Array.from({ length: k }, () => Array.from({ length: k }, () => (chance(r, 0.15) ? 0 : round(r() * 10 - 5, 2))));
    calls.push({ op: 'vec.solveDense', args: [A, Array.from({ length: k }, () => round(r() * 10, 2))] });
    calls.push({ op: 'vec.fmt', args: [pick(r, [r() * 1e3, -1e-10, 1.23456789, 1e21, '$NaN', 0.5]), pick(r, [null, 0, 2, 9])] });
    calls.push({ op: 'isect.normAng', args: [round(r() * 40 - 20, 4)] });
  }
  return [{ calls }];
}

async function genJson(r) {
  const calls = [];
  const atoms = ['{', '}', '[', ']', ',', ':', '"a"', '"', '1', '-', '-0', '01', '1.', '.5', '1e', '1e+', '2E-3', 'true', 'tru', 'fals', 'null', 'nul', 'x', ' ', '\n', '\r\n', '\t', '"\\u12g4"', '"\\q"', '"\u0001"', '"é"', '€', '\\', '/', 'NaN', 'undefined', '0x1'];
  const base = ['{"a":1,"b":[1,2,{"c":null}]}', '[1,2,3]', '{"name":"Steel","density":7.85}', '"str"', '{"sheets":[{"size":"A3","views":[]}],"x":true}'];
  for (const s of ['', ' ', 'NaN', 'undefined', 'Infinity', '[object Object]', '{', '[', '[1', '{"a"', '{"a":', '{"a":1', '{"a":1,', '[1,]', '{,}', '"abc', '"\\', '-', '1.', '1e', '01', '[01]', '{a:1}', 'tru', 'nulx', 'true false']) calls.push({ op: 'json.parseError', args: [s] });
  for (let i = 0; i < 400; i++) {
    let s;
    if (chance(r, 0.5)) { s = Array.from({ length: ri(r, 1, 12) }, () => pick(r, atoms)).join(''); }
    else {
      s = pick(r, base);
      for (let k = 0; k < ri(r, 1, 3); k++) { const at = ri(r, 0, s.length); const m = r(); s = m < 0.4 ? s.slice(0, at) + s.slice(at + 1) : m < 0.8 ? s.slice(0, at) + pick(r, atoms) + s.slice(at) : s.slice(0, at); }
    }
    calls.push({ op: 'json.parseError', args: [s] });
  }
  return [{ calls }];
}

export const GENS = { pcscript: genPcscript, uses: genUses, tools: genTools, vec: genVec, json: genJson };
