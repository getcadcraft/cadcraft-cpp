// SPDX-License-Identifier: Apache-2.0
// pcdiff/gen-tests2.mjs — stage-2 frozen test sections as parity cases:
//  - tools/partcraft-test/test-sketch-tools.mjs lines 111–422 (spline entities, intersections, trim / extend, offset,
//    mirror / polygon / patterns: everything before the kernel sections), run as written with recording wrappers of
//    sketch-tools.js and sketch-intersect.js;
//  - bin/test-partcraft-params.mjs "scripts" sections without the exact kernel: the fixtures it can build without
//    OpenCascade written / read / written again, the hand-written error scripts, the bracket example (rollback,
//    suppressed), design tables and the rollback helpers;
//  - tools/partcraft-test/test-param-uses.mjs (paramUses on its part and the cycle, scriptParams cases).
import { readFileSync } from 'node:fs';
import * as D from './driver.mjs';
import { flange, blockSketch } from './gen-part.mjs';

const REF = (process.env.PARTCRAFT_REF || '/var/www/CADCRAFT/forks/work/ref') + '/';
const { S, SP, SV, P, PR, PP } = D;
const SC = await import(D.DEV + 'script.js');
const PU = await import(D.DEV + 'params-uses.js');
const T0 = await import(D.DEV + 'sketch-tools.js');
const I0 = await import(D.DEV + 'sketch-intersect.js');
const clone = (v) => (v === undefined ? null : structuredClone(v));

function sketchTools() {
  const calls = [];
  const rec = (op, args) => calls.push({ op, args: args.map(clone) });
  const T = new Proxy(T0, {
    get(t, k) {
      const f = t[k];
      switch (k) {
        case 'trim': case 'extend': return (sk, id, at) => { rec('tools.' + k, [sk, id, at]); return f(sk, id, at); };
        case 'chainOf': return (sk, id, ids = null) => { rec('tools.chainOf', [sk, id, ids ? [...ids] : null]); return f(sk, id, ids); };
        case 'offsetSide': return (sk, chain, p) => { rec('tools.offsetSide', [sk, chain, p]); return f(sk, chain, p); };
        case 'offset': return (sk, chain, d, o = {}) => { rec('tools.offset', [sk, chain, d, o.link ?? null]); return f(sk, chain, d, o); };
        case 'mirror': case 'patternLinear': case 'patternCircular': case 'addPolygon': case 'addSlot':
          return (...a) => { rec('tools.' + k, a); return f(...a); };
        case 'crossings': return (sk, e) => { rec('tools.crossings', [sk, e.id]); return f(sk, e); };
        case 'trimPiece': return (sk, id, at) => { rec('tools.trimPiece', [sk, id, at]); return f(sk, id, at); };
        default: return f;
      }
    },
  });
  const I = new Proxy(I0, {
    get(t, k) {
      if (k === 'intersect') return (A, B, o = {}) => { rec('isect.intersectCurves', [A, B, !!o.infA, !!o.infB]); return t.intersect(A, B, o); };
      return t[k];
    },
  });
  const src = readFileSync(REF + 'partcraft-tests/test-sketch-tools.mjs', 'utf8').split('\n').slice(110, 422).join('\n');
  const ok = () => {}, section = () => {}, near = (a, b, tol = 1e-9) => Math.abs(a - b) <= tol;
  const dist = (a, b) => Math.hypot(b[0] - a[0], b[1] - a[1]);
  const throws = (fn, re) => { try { fn(); return false; } catch (e) { return re ? re.test(e.message) : true; } };
  const maxOver = (n, f) => { let m = 0; for (let i = 0; i <= n; i++) m = Math.max(m, f(i / n)); return m; };
  const fn = new Function('ok', 'section', 'near', 'S', 'SP', 'I', 'T', 'solveSketch', 'dist', 'throws', 'maxOver', 'PI', src);
  fn(ok, section, near, S, SP, I, T, SV.solveSketch, dist, throws, maxOver, Math.PI);
  return calls;
}

function scripts() {
  const calls = [];
  const write = (pt, o = {}) => { calls.push({ op: 'pcscript.write', args: [clone(pt), o] }); return SC.partToScript(pt, o); };
  const read = (text, files = null) => { calls.push({ op: 'pcscript.read', args: [text, files] }); try { return SC.scriptToPart(text, files ? { readFile: (n) => { if (!Object.hasOwn(files, n)) throw new Error('missing'); return files[n]; } } : {}); } catch { return null; } };
  const fixtures = [];
  // the flange with parameters and equations (test-partcraft-params "flange" section, no kernel)
  {
    const { part, ids, dims } = flange();
    const p = PP.paramsOf(part);
    PR.setVar(p, 'bolt_d', '12'); PR.setVar(p, 'disc_t', '30');
    try { PR.setLink(p, PP.partAdapter, part, ids.hole + '.d', 'bolt_d'); } catch { /* */ }
    try { PR.setLink(p, PP.partAdapter, part, ids.pattern + '.count', 'if(bolt_d >= 16, 6, 8)'); } catch { /* */ }
    try { PR.setLink(p, PP.partAdapter, part, ids.profile + '.' + dims.disc, 'disc_t'); } catch { /* */ }
    fixtures.push(part);
  }
  fixtures.push(flange().part);
  {
    const bp = P.newPart('Block');
    const b = blockSketch(100, 50);
    const s1 = P.addFeature(bp, 'sketch', { plane: { kind: 'base', name: 'top' }, sketch: b.sk });
    const e1 = P.addFeature(bp, 'extrude', { sketch: s1.id, depth: 20 });
    P.addFeature(bp, 'fillet', { edges: [{ faces: [e1.id + '.side:' + b.lines[0], e1.id + '.side:' + b.lines[1]] }, { faces: [e1.id + '.side:' + b.lines[1], e1.id + '.end'], at: [100, 10, 0] }], r: 5 });
    P.addFeature(bp, 'shell', { faces: [{ name: e1.id + '.start' }], t: 2 });
    P.addFeature(bp, 'chamfer', { edges: [{ faces: [e1.id + '.end', e1.id + '.side:' + b.lines[2]] }], d: 1 });
    const pb = PP.paramsOf(bp);
    PR.setVar(pb, 'rad', '5'); PR.setVar(pb, 'w', '100');
    PR.setLink(pb, PP.partAdapter, bp, bp.features[2].id + '.r', 'rad');
    PR.setLink(pb, PP.partAdapter, bp, s1.id + '.' + b.dw, 'w');
    fixtures.push(bp);
    const bs = structuredClone(bp); bs.features[3].suppressed = true; bs.rollback = 3; bs.name = 'Block "rolled" #2';
    fixtures.push(bs);
  }
  {
    const lp = P.newPart('Patterns');
    const lb = blockSketch(100, 50);
    const ls = P.addFeature(lp, 'sketch', { plane: { kind: 'base', name: 'top' }, sketch: lb.sk });
    const le = P.addFeature(lp, 'extrude', { sketch: ls.id, depth: 20 });
    const hs = S.newSketch(); const hp = S.addPoint(hs, 10, 10); S.addConstraint(hs, 'hdistance', ['O', hp], 10); S.addConstraint(hs, 'vdistance', ['O', hp], 10);
    const hsf = P.addFeature(lp, 'sketch', { plane: { kind: 'face', feature: le.id, face: 'end' }, sketch: hs });
    const ho = P.addFeature(lp, 'hole', { sketch: hsf.id, kind: 'cbore', d: 5, through: true, cboreD: 9, cboreDepth: 3 });
    P.addFeature(lp, 'lpattern', { seeds: [ho.id], dir: { kind: 'world', axis: 'x' }, count: 4, spacing: 25 });
    P.addFeature(lp, 'mirror', { seeds: 'body', plane: { kind: 'fixed', o: [0, 0, 0], n: [0, 0, 1] } });
    const cs = S.newSketch(); S.addCircle(cs, [50, 25], 8);
    const csf = P.addFeature(lp, 'sketch', { plane: { kind: 'base', name: 'top' }, sketch: cs });
    P.addFeature(lp, 'extrude', { sketch: csf.id, op: 'cut', end: 'through', depth: 10 });
    fixtures.push(lp);
  }
  {
    const rp = P.newPart('Ring');
    const rs = S.newSketch();
    const ax = S.addLine(rs, [0, 0], [0, 10], { construction: true });
    S.addRect(rs, [10, 0], [20, 5]);
    const arc = S.addCircle(rs, [30, 3], 2);
    const rsf = P.addFeature(rp, 'sketch', { plane: { kind: 'base', name: 'front' }, sketch: rs });
    P.addFeature(rp, 'revolve', { sketch: rsf.id, axis: ax.id, angle: 90 });
    S.addConstraint(rs, 'radius', [arc.id], 2);
    fixtures.push(rp);
  }
  {
    const dp = P.newPart('Design');
    const ds = S.newSketch(); S.addSpline(ds, [[0, 0], [10, 5], [20, 0], [10, -8]], { closed: true });
    const dsf = P.addFeature(dp, 'sketch', { plane: { kind: 'base', name: 'top' }, sketch: ds });
    P.addFeature(dp, 'extrude', { sketch: dsf.id, depth: 7 });
    P.addFeature(dp, 'plane', { mode: 'offset', planes: [{ kind: 'base', name: 'top' }], d: 15 });
    fixtures.push(dp);
  }
  {
    const ip = P.newPart('Imported');
    ip.features.push({ id: 'f1', type: 'import', name: 'Imported1', file: 'block.step', step: 'ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\nENDSEC;\nEND-ISO-10303-21;\n', up: 'y' }); ip.seq = 2;
    fixtures.push(ip);
  }
  for (const pt of fixtures) {
    let src;
    try { src = P.readPart(P.toJSON(pt)); } catch { continue; }
    for (const f of src.features) if (f.type === 'sketch') SV.solveSketch(f.sketch);
    const s = write(src);
    write(src, { exact: true });
    const files = Object.fromEntries(s.files.map((f) => [f.name, f.text]));
    const back = read(s.text, files);
    if (back) { write(back.part); write(back.part, { inlineStep: true }); }
  }
  // by hand: the bracket and the error scripts
  const text = readFileSync(REF + 'partcraft-tests/examples/bracket.pcscript', 'utf8');
  const br = read(text);
  for (const src of [
    'extrude E1\n  sketch Nope\n',
    'param w = 10\nsketch S\n  on top\n  point a 0, 0\n  distance a = w\n',
    'param w = 10\n\n# comment\nsketch S\n  on top\n  point a 0, 0\n  point b 1, 0\n  line l a b\n  distance l = q\n',
    'param a = b\nparam b = a\n',
    'extrud E1\n', '  depth 3\n', 'extrude E\n  depth\n',
    'param w = 10\nextrude E\n  depth = -w\n',
    'pcscript 2\n',
    'extrude E\n  __proto__ 1\n', 'extrude E\n  op json {"a":\n',
    'sketch S\n' + '  point p1 0, 0\n'.repeat(2),
    'import I\n  step-file "x.step"\n',
    text + '\nrollback Bolts\n',
    text.replace('  through\n', '  through\n  suppressed\n'),
  ]) read(src);
  calls.push({ op: 'pcscript.read', args: ['x'.repeat(SC.SCRIPT_LIMITS.bytes + 1), null] });
  calls.push({ op: 'pcscript.looks', args: [text] }, { op: 'pcscript.looks', args: [P.toJSON(fixtures[0])] });
  // design tables and rollback helpers
  if (br) {
    const csv = 'name,width,thick,bolt\nsmall,100,6,6.6\nmedium,140,8,8.5\nlarge,8in,0.5in,13\n';
    calls.push({ op: 'params.readDesignTable', args: [csv, clone(br.part.params)] });
    const t = PR.readDesignTable(csv, br.part.params);
    for (const row of t.rows || []) calls.push({ op: 'pp.apply', args: [clone(br.part), Object.fromEntries(row.overrides)] });
    calls.push({ op: 'params.readDesignTable', args: ['name,colour\na,1\n', clone(br.part.params)] }, { op: 'params.readDesignTable', args: ['name,width\na,wide\n', clone(br.part.params)] });
  }
  const pt = structuredClone(fixtures[2]);
  for (const step of [-1, -2, 1, 'end', 1, -99, -1]) {
    if (step === 'end') { calls.push({ op: 'pp.rollTo', args: [clone(pt), null] }); PP.rollTo(pt, null); continue; }
    calls.push({ op: 'pp.stepRollback', args: [clone(pt), step] }); PP.stepRollback(pt, step);
  }
  calls.push({ op: 'pp.rollbackWords', args: [clone(pt)] });
  return calls;
}

function paramUses() {
  const calls = [];
  const part = P.newPart('Uses');
  const b = blockSketch(100, 50);
  const s1 = P.addFeature(part, 'sketch', { plane: { kind: 'base', name: 'top' }, sketch: b.sk });
  const e1 = P.addFeature(part, 'extrude', { sketch: s1.id, depth: 20 });
  const sk = S.newSketch(); S.addPoint(sk, 20, 20);
  const s2 = P.addFeature(part, 'sketch', { plane: { kind: 'face', feature: e1.id, face: 'end' }, sketch: sk });
  const h1 = P.addFeature(part, 'hole', { sketch: s2.id, d: 6 });
  const tg = PP.partAdapter.targets(part);
  const dimKey = [...tg.keys()].find((k) => k.startsWith(s1.id + '.'));
  part.params = { vars: [{ name: 'w', expr: '40' }, { name: 'h', expr: 'w / 2' }, { name: 'd', expr: 'h + 2' }, { name: 't', expr: '3' }],
    links: [{ target: e1.id + '.depth', expr: 'w' }, { target: dimKey, expr: 'h' }, { target: h1.id + '.d', expr: 'd' }] };
  part.configs = { rows: [{ name: 'Small', values: { w: 30 } }, { name: 'Big', values: { w: 60 } }], active: null };
  part.studies = { script: { v: 1, active: 0, keepVars: false, scripts: [
    { name: 'half.craft', lang: 'craft', text: "x = param('h') * 2;\ny = param(\"h\");  % twice, listed once" },
    { name: 'all.lua', lang: 'lua', text: 'local p = cs.params()\nprint(p.w)' },
    { name: 'gone.py', lang: 'python', text: "v = cs.param( 'nope' ) + cs.param('w')" },
  ] } };
  calls.push({ op: 'uses.paramUses', args: [clone(part)] }, { op: 'uses.paramUses', args: [{ ...clone(part), studies: {} }] });
  calls.push({ op: 'uses.paramUses', args: [{ ...clone(part), params: { vars: [{ name: 'a', expr: 'b + 1' }, { name: 'b', expr: 'a + 1' }], links: [{ target: e1.id + '.depth', expr: 'b' }] }, configs: null, studies: {} }] });
  calls.push({ op: 'uses.paramUses', args: [P.newPart('Empty')] });
  for (const s of ["a = param('w') + param(\"h\") + param( 'w' )", "param('w')", 'local x = cs.param("depth")', "x = cs.param('w')  # python", 's = params(); x = s.w', 's = params ( )', 's = paramsx()', "n = 'w'; x = param(n)", "x = myparam('w')", "x = param('2bad')"]) calls.push({ op: 'uses.scriptParams', args: [s] });
  return calls;
}

export const GENS = { tests2: () => [{ calls: sketchTools() }, { calls: scripts() }, { calls: paramUses() }] };
