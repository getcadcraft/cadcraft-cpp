// SPDX-License-Identifier: Apache-2.0
// pcdiff/gen-stage5.mjs — stage-5 generators: files PartCraft writes (export.js) from meshes each side's OWN kernel
// builds (M12: "$k" / "$call" placeholders, ops-kernel.mjs / script6.rs; the JS rebuild here only decides which calls
// to make), random sketches and hand-made flat patterns.
import * as D from './driver.mjs';
import { ri, pick, chance } from './gen-core.mjs';
import { buildSketch } from './gen-sketch.mjs';
import { randomPart, flange } from './gen-part.mjs';
import { K, M } from './ops-stage3.mjs';
import { freshKernel } from './ops-kernel.mjs';

/** A placeholder: what this side's own kernel gives for `part` (a snapshot of the part as it is now). */
const KP = (part, get) => ({ $k: structuredClone(part), get });
const CALL = (op, ...args) => ({ $call: op, args });

const round = (x, d = 3) => Math.round(x * 10 ** d) / 10 ** d;
const regen = (part) => { try { return K.regenerate(part, M); } catch { return null; } };
void M;

async function genExport(r) {
  await freshKernel();
  const scripts = [];
  for (let s = 0; s < 3; s++) {
    const calls = [];
    for (let k = 0; k < 2; k++) {
      const part = s === 0 && k === 0 ? flange({ holes: ri(r, 1, 8), resolution: 32 }).part : randomPart(r);
      const g = regen(part);
      if (!g?.mesh) continue;
      const mesh = KP(part, 'mesh');
      const name = pick(r, ['part', 'Flange', 'Blöck "a" <b> & \u0001', 'x'.repeat(100), null]);
      calls.push({ op: 'export.stl', args: [mesh, name] }, { op: 'export.3mf', args: [mesh, name] });
      if (g.mesh.indices.length / 3 < 20000) calls.push({ op: 'export.step', args: [mesh, name, pick(r, ['2026-10-17T10:11:12.000Z', '2020-01-01T00:00:00.000Z']), pick(r, ['AP242', 'AP214', null])] });
    }
    for (let k = 0; k < 3; k++) {
      const sk = buildSketch(r, [], { clean: chance(r, 0.5) }) || D.S.newSketch();
      if (chance(r, 0.3)) for (const e of sk.entities) if (chance(r, 0.3)) e.construction = true;
      calls.push({ op: 'export.dxfSketch', args: [sk] });
    }
    const loops = Array.from({ length: ri(r, 0, 3) }, () => Array.from({ length: ri(r, 3, 8) }, () => [round(r() * 200 - 100, 4), round(r() * 200 - 100, 4)]));
    calls.push({ op: 'export.dxfLoops', args: [loops] });
    const x1 = round(50 + r() * 200, 2), y1 = round(30 + r() * 100, 2);
    const flat = {
      box: { x0: 0, y0: 0, x1, y1 },
      segs: [{ type: 'line', a: [0, 0], b: [x1, 0] }, { type: 'arc', c: [x1, y1 / 2], r: y1 / 2, a0: 270, a1: 90 }, { type: 'circle', c: [10, 10], r: 3 }, { type: 'poly', pts: [[0, y1], [x1 / 2, y1 + 5], [x1, y1]] }, { type: 'other' }],
      bends: Array.from({ length: ri(r, 0, 3) }, () => ({ a: [round(r() * x1, 2), 0], b: [round(r() * x1, 2), y1], up: chance(r, 0.5), angle: round(r() * 180, 3), r: round(r() * 5, 4) })),
    };
    calls.push({ op: 'export.dxfFlat', args: [flat] });
    for (const t of ['', 'abc', 'PartCraft €', 'x'.repeat(1000)]) calls.push({ op: 'export.crc32', args: [t] });
    calls.push({ op: 'export.zip', args: [[{ name: 'a.txt', data: 'hello' }, { name: 'dir/ü.xml', data: '<x/>' }, { name: '', data: '' }]] });
    scripts.push({ calls });
  }
  return scripts;
}

const DRW = await import(D.DEV + 'drawing.js');
const GD = await import(D.DEV + 'gdt.js');
function note(r, i) {
  const kind = pick(r, ['datum', 'fcf', 'finish']);
  const n = { id: 'g' + (i + 1), kind, view: pick(r, ['front', 'top', 'right', 'iso', 'section', 'detail', 'holes', 'side']), at: [round((r() - 0.5) * 100, 2), round((r() - 0.5) * 100, 2)], off: [round((r() - 0.5) * 40, 2), round((r() - 0.5) * 40, 2)] };
  if (kind === 'datum') n.letter = pick(r, ['A', 'B', 'C']);
  if (kind === 'finish') Object.assign(n, { ra: pick(r, [1.6, 0.8, 3.2]), process: pick(r, ['any', 'removal', 'noRemoval']), note: pick(r, ['', 'ground']) });
  if (kind === 'fcf') Object.assign(n, { char: pick(r, ['flatness', 'position', 'perpendicularity']), dia: chance(r, 0.4), tol: pick(r, [0.1, 0.05]), mod: pick(r, ['', 'M']), datums: chance(r, 0.5) ? [{ letter: 'A', mod: '' }] : [] });
  try { return GD.readNotes([n])[0]; } catch { return null; }
}
function drawingSettings(r) {
  const d = {};
  if (chance(r, 0.5)) d.sheet = pick(r, ['a4', 'a3', 'a2', 'ansi-b', 'ansi-d', 'nope']);
  if (chance(r, 0.4)) d.revisions = Array.from({ length: ri(r, 1, 4) }, (_, i) => ({ rev: pick(r, ['A', 'B', String(i + 1), '']), desc: pick(r, ['First issue', 'Hole moved 5 mm left of the datum edge and more words', '']), date: '2026-10-1' + i, by: pick(r, ['AB', '']) }));
  if (chance(r, 0.6)) d.title = { title: pick(r, ['', 'Bracket', 'A very long title that will not fit in the title block cell at all']), company: pick(r, ['', 'Acme']), number: pick(r, ['', 'BR-1001']), rev: pick(r, ['', 'C']), drawnBy: 'X', checkedBy: '', approvedBy: pick(r, ['', 'Y']), date: pick(r, ['', '2026-01-02']), tolerance: pick(r, ['', '±0.1']), finish: pick(r, ['', 'Ra 3.2']) };
  if (chance(r, 0.4)) d.section = { of: pick(r, ['front', 'top', 'right']) };
  if (chance(r, 0.3)) d.detail = { of: pick(r, ['front', 'top', 'section']), at: [round(r() * 40, 1), round(r() * 20, 1)], r: round(5 + r() * 15, 1), scale: pick(r, [2, 3, 5]) };
  if (chance(r, 0.4)) d.holeTable = { view: pick(r, ['front', 'top', 'right', 'iso']) };
  if (chance(r, 0.5)) d.notes = Array.from({ length: ri(r, 1, 4) }, (_, i) => note(r, i)).filter(Boolean);
  return d;
}

async function genDrawing(r) {
  await freshKernel();
  const scripts = [];
  for (let s = 0; s < 3; s++) {
    const calls = [];
    for (let k = 0; k < 2; k++) {
      const part = s === 0 && k === 0 ? flange({ holes: ri(r, 1, 8), resolution: 32 }).part : randomPart(r);
      const g = regen(part);
      if (!g?.mesh || g.mesh.indices.length / 3 > 12000) continue;
      const mesh = KP(part, 'mesh');
      calls.push({ op: 'draw.meshEdges', args: [mesh] }, { op: 'draw.meshViews', args: [mesh, chance(r, 0.5) ? false : null] });
      const gjP = KP(part, 'drawG');
      calls.push({ op: 'draw.partHoles', args: [part, gjP] });
      const gj = { named: !!g.named, faces: g.faces, threads: Object.fromEntries([...(g.threads || new Map())].map(([k2, th]) => [k2, { callout: th.callout }])) };
      const holes = DRW.partHoles(part, { ...gj, threads: new Map(Object.entries(gj.threads)) });
      const holesP = CALL('draw.partHoles', structuredClone(part), gjP);
      const drawing = drawingSettings(r);
      const opts = { name: pick(r, ['Part1', 'Bracket', null]), date: '2026-10-17T08:00:00.000Z', paper: pick(r, [null, 'a2', 'a4']), drawing, annotate: chance(r, 0.8), material: pick(r, ['', 'Steel']), holes: chance(r, 0.8) ? holesP : null };
      calls.push({ op: 'draw.makeDrawing', args: [mesh, opts] });
      // with a section view from each side's own meshSection (the solid library's trimByPlane)
      const iso = chance(r, 0.7), section = { of: pick(r, ['front', 'top', 'right']), ...(chance(r, 0.3) ? { at: round(r() * 10, 2) } : {}) };
      const data = DRW.meshViews(g.mesh, { iso, M, section });
      const threads = chance(r, 0.5) ? [{ count: 2, callout: 'M6×1 – 6H' }, { count: 1, callout: 'M8' }] : [];
      data.threads = threads;
      const dataP = CALL('util.json', CALL('util.set', CALL('draw.meshViewsSec', mesh, iso, section), 'threads', threads));
      const sheetOpts = { ...opts, drawing: { ...drawing, section: true } };
      calls.push({ op: 'draw.compose', args: [dataP, sheetOpts] });
      let sheet = null;
      try { sheet = DRW.composeSheet(JSON.parse(JSON.stringify(data)), { ...sheetOpts, holes: sheetOpts.holes ? holes : null, date: new Date(sheetOpts.date), annotate: sheetOpts.annotate ? GD.annotationGeometry : null }); } catch { sheet = null; }
      if (sheet) calls.push({ op: 'draw.pdfContent', args: [CALL('util.json', CALL('draw.compose', dataP, sheetOpts))] });
      if (holes?.length) for (const v of ['front', 'top', 'right', 'iso', 'x']) calls.push({ op: 'draw.holeTableRows', args: [holesP, v, { u0: -50, v0: -20, u1: 50, v1: 20 }] });
    }
    const segs = Array.from({ length: ri(r, 3, 20) }, () => [round(r() * 40, 3), round(r() * 40, 3), round(r() * 40, 3), round(r() * 40, 3)]);
    calls.push({ op: 'draw.hatch', args: [segs, pick(r, [2.5, 1, 7]), pick(r, [null, 30, 90])] }, { op: 'draw.clipToCircle', args: [segs, [20, 20], round(r() * 20, 2)] });
    const line = Array.from({ length: 8 }, () => { const a = round(r() * 10, 1), b = round(a + r() * 5, 1); return [a, 2 * a, b, 2 * b]; });
    calls.push({ op: 'draw.mergeSegments', args: [line.concat(segs), 50, chance(r, 0.5) ? line.slice(0, 2) : []] });
    for (const k of ['a4', 'a3', 'ansi-b', 'ansi-e', 'nope']) calls.push({ op: 'draw.sheetLabel', args: [k] });
    for (const revs of [[], [{ rev: 'A' }], [{ rev: 'Z' }], [{ rev: 'AZZ' }], [{ rev: '9' }], [{ rev: 'b2' }], [{}]]) calls.push({ op: 'draw.nextRevision', args: [revs] });
    for (const sc of [10, 1, 0.5, 0.02, 0.001]) calls.push({ op: 'draw.scaleLabel', args: [sc] });
    for (const t of ['Ø10 (THRU) \\ x', '€ “quote” – ½ 中', '\u0001tab\t']) calls.push({ op: 'draw.winAnsi', args: [t] });
    scripts.push({ calls });
  }
  return scripts;
}

async function genAnalysis(r) {
  await freshKernel();
  const calls = [];
  for (let k = 0; k < 3; k++) {
    const part = k === 0 ? flange({ holes: ri(r, 1, 8), resolution: 32 }).part : randomPart(r);
    const g = regen(part);
    if (!g?.mesh || g.mesh.indices.length / 3 > 15000) continue;
    const mesh = KP(part, 'mesh');
    const nt = g.mesh.indices.length / 3;
    const cn = chance(r, 0.4) ? Array.from({ length: nt * 9 }, () => round(r() * 2 - 1, 3)) : null;
    const pull = pick(r, [[0, 1, 0], [1, 0, 0], [0, 0, -1], [0.3, 0.9, 0.1]]);
    calls.push({ op: 'an.draft', args: [mesh, cn, pull, pick(r, [1, 3, -2, 0]), { positive: [0.1, 0.8, 0.2], needs: [0.9, 0.8, 0.1], negative: [0.9, 0.1, 0.1] }] });
    calls.push({ op: 'an.zebra', args: [mesh, cn, pick(r, [[0, 0, -1], [1, -1, -1]]), pick(r, [12, 7, 30]), { dark: [0.1, 0.1, 0.1], light: [0.95, 0.95, 0.95] }] });
    calls.push({ op: 'an.curvature', args: [mesh, { concave: [0.2, 0.3, 0.9], flat: [0.96, 0.95, 0.9], convex: [0.9, 0.2, 0.2] }] }, { op: 'an.curvatureRaw', args: [mesh] });
    const bb = g.stats?.bbox ? CALL('util.path', KP(part, 'stats'), ['bbox']) : { min: [0, 0, 0], max: [1, 1, 1] };
    for (const n of [[0, 0, 1], [0, 1, 0], [1, 0, 0], [0.6, 0.8, 0]]) calls.push({ op: 'sec.range', args: [bb, n] });
    const facesP = KP(part, 'faces');
    for (let fi = 0; fi < Math.min(6, (g.faces || []).length); fi++) calls.push({ op: 'sec.face', args: [CALL('util.get', facesP, fi), chance(r, 0.5) ? null : [0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1]] });
  }
  calls.push({ op: 'sec.face', args: [{ type: 'cylinder' }, null] }, { op: 'sec.face', args: [{ type: 'plane', n: [0, 0, 0], o: [1, 2, 3] }, null] }, { op: 'sec.face', args: [null, null] });
  for (let i = 0; i < 20; i++) calls.push({ op: 'an.draftAngle', args: [[round(r() * 2 - 1, 3), round(r() * 2 - 1, 3), round(r() * 2 - 1, 3)], [0, 1, 0]] });
  return [{ calls }];
}

async function genMeasure(r) {
  await freshKernel();
  const scripts = [];
  for (let s = 0; s < 2; s++) {
    const calls = [];
    for (let k = 0; k < 2; k++) {
      const part = s === 0 && k === 0 ? flange({ holes: ri(r, 1, 6), resolution: 32 }).part : randomPart(r);
      const g = regen(part);
      if (!g?.mesh || g.mesh.indices.length / 3 > 6000) continue;
      const m = g.mesh;
      const bJs = { exact: false, named: !!g.named, edges: g.edges || [], faces: g.faces || [], stats: g.stats, mesh: { positions: Array.from(m.positions), indices: Array.from(m.indices), ...(m.triFace ? { triFace: Array.from(m.triFace) } : {}), ...(m.edgeSegs ? { edgeSegs: Array.from(m.edgeSegs), segEdge: Array.from(m.segEdge) } : {}) } };
      const b = KP(part, 'measureB');
      const bNoEdges = CALL('util.set', b, 'edges', []);
      calls.push({ op: 'ms.snap', args: [b] }, { op: 'ms.snap', args: [bNoEdges] });
      const proj = [2, 0.1, 0, 300, 0, -2, 0.2, 200, 0.1, 0.1, 1, 500];
      for (let i = 0; i < 4; i++) {
        const x = round(200 + r() * 200, 1), y = round(100 + r() * 200, 1);
        calls.push({ op: 'ms.snapAt', args: [b, proj, x, y, pick(r, [10, 30, 3])] }, { op: 'ms.nearestEdge', args: [b, proj, x, y, pick(r, [6, 20])] });
      }
      const M = pick(r, [null, [0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0, 10, 20, 30, 1]]);
      const picks = [];
      for (let i = 0; i < Math.min(5, (g.edges || []).length); i++) { const ei = ri(r, 0, g.edges.length - 1); picks.push({ kind: 'edge', index: ei, matrix: M }); calls.push({ op: 'ms.circleOf', args: [CALL('util.get', KP(part, 'edges'), ei), b, ei] }); }
      for (let i = 0; i < Math.min(5, (g.faces || []).length); i++) picks.push({ kind: 'face', index: ri(r, 0, g.faces.length - 1), matrix: M, local: [round(r() * 10, 2), 0, 0] });
      picks.push({ kind: 'vertex', local: [1, 2, 3], matrix: M }, { kind: 'point', local: [round(r() * 50, 2), 4, 5], matrix: M }, { kind: 'body', matrix: M }, { kind: 'face', tris: [0, 1, 2], local: [0, 0, 0] }, { kind: 'edge', index: 9999 });
      for (const p of picks) {
        calls.push({ op: 'ms.info', args: [b, p] }, { op: 'ms.exactSub', args: [p] });
        if (p.kind !== 'body') calls.push({ op: 'ms.geometry', args: [b, p] });
        let inf = null; try { inf = MS.info(bJs, p); } catch { inf = null; }
        if (inf) calls.push({ op: 'ms.infoRows', args: [CALL('ms.info', b, p)] });
      }
      for (let i = 0; i < 6; i++) {
        const A = pick(r, picks.slice(0, -2)), B = pick(r, picks.slice(0, -2));
        calls.push({ op: 'ms.pair', args: [b, A, B, chance(r, 0.2) ? { distance: 3.5, p1: [0, 0, 0], p2: [3.5, 0, 0], inside: chance(r, 0.5) } : null] });
      }
      const meshP = KP(part, 'mesh');
      calls.push({ op: 'ms.meshMass', args: [meshP] });
      const mm = CALL('ms.meshMass', meshP);
      const placed = [CALL('ms.place', mm, null, 7.85), CALL('ms.place', mm, [0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 50, 0, 0, 1], 2.7)];
      calls.push({ op: 'ms.place', args: [mm, M, 7.85] }, { op: 'ms.combine', args: [placed] }, { op: 'ms.principal', args: [CALL('util.get', mm, 'tensor')] });
      calls.push({ op: 'ms.massRows', args: [mm] }, { op: 'ms.massRows', args: [placed[0]] }, { op: 'ms.massRows', args: [CALL('util.set', placed[1], 'mass', 1500)] });
    }
    for (const k of ['vertex', 'edge', 'body', 'odd']) calls.push({ op: 'ms.kindLabel', args: [k] });
    calls.push({ op: 'ms.principal', args: [[[2, 0, 0], [0, 2, 0], [0, 0, 1]]] }, { op: 'ms.principal', args: [[[4, 1, 0.5], [1, 3, 0.2], [0.5, 0.2, 1]]] });
    scripts.push({ calls });
  }
  return scripts;
}
const MS = await import(D.DEV + 'measure.js');

async function genAsmDrawing(r) {
  await freshKernel();
  const calls = [];
  const meshes = [];
  for (let k = 0; k < 3; k++) {
    const part = k === 0 ? flange({ holes: ri(r, 1, 6), resolution: 32 }).part : randomPart(r);
    const g = regen(part);
    if (g?.mesh && g.mesh.indices.length / 3 < 8000) meshes.push({ name: part.name, mesh: KP(part, 'mesh'), jsMesh: g.mesh });
  }
  if (!meshes.length) return [{ calls }];
  const V = await import(D.DEV + 'vec.js');
  const items = meshes.map((m, i) => ({ pid: 'p' + (i + 1), name: m.name, matrix: V.rotationAbout([i * 10, 0, 0], [0, 1, 0], i * 30), mesh: m.mesh }));
  calls.push({ op: 'adw.mesh', args: [items.map((it) => ({ mesh: it.mesh, matrix: it.matrix }))] });
  calls.push({ op: 'adw.3mf', args: [pick(r, ['Asm', 'A & <b>']), meshes.map((m, i) => ['p' + (i + 1), m]), items.map((it) => ({ pid: it.pid, matrix: it.matrix })).concat([{ pid: 'p99', matrix: V.translation([1, 2, 3]) }])] });
  const view = CALL('util.path', CALL('draw.meshViews', meshes[0].mesh, true), ['views', 'iso']);
  for (const m of meshes) delete m.jsMesh;
  for (let s = 0; s < 4; s++) {
    const nRows = pick(r, [0, 2, 5, 60]);
    const rows = Array.from({ length: nRows }, (_, i) => ({ item: i + 1, pid: 'p' + ri(r, 1, 3), qty: ri(r, 1, 9), number: pick(r, ['', 'BR-' + i, 'x'.repeat(40)]), name: pick(r, ['Bracket', 'Hex bolt ISO 4017 M10 × 40 with a long description']), material: pick(r, ['', 'Steel']), massEach: pick(r, [null, 12.5, 2500.123]) }));
    const insts = Array.from({ length: ri(r, 1, 6) }, () => ({ part: 'p' + ri(r, 1, 3) }));
    const anchors = insts.map(() => [round((r() - 0.5) * 150, 2), round((r() - 0.5) * 150, 2), round(r() * 100, 2)]);
    calls.push({ op: 'adw.balloonsFor', args: [rows, insts, anchors] });
    const balloons = AD.balloonsFor(rows, insts, anchors);
    const trails = chance(r, 0.5) ? [[[0, 0, 0], [10, 20, 0], [30, 20, 5]], [[5, 5, 5], [5, 50, 5]]] : [];
    calls.push({ op: 'adw.sheet', args: [{ view, anchors, exact: chance(r, 0.3) }, rows, { name: pick(r, ['Gearbox', null, 'A very long assembly name that goes on and on']), date: '2026-10-17T00:00:00.000Z', balloons, trails, exploded: pick(r, [null, 'Exploded 1']), paper: pick(r, [null, 'a2', 'a4']) }] });
  }
  for (const t of ['short', 'A rather long name that needs fitting', '€€€€€€€€€€€€€']) calls.push({ op: 'adw.fit', args: [t, 2.5, 20] });
  return [{ calls }];
}
const AD = await import(D.DEV + 'asm-drawing.js');

async function genIfc(r) {
  await freshKernel();
  const calls = [];
  const A = await import(D.DEV + 'assembly.js');
  const V = await import(D.DEV + 'vec.js');
  const built = [];
  for (let k = 0; k < 3; k++) {
    const part = k === 0 ? flange({ holes: ri(r, 1, 6), resolution: 32 }).part : randomPart(r);
    if (chance(r, 0.5)) part.material = { name: pick(r, ['Steel', 'Aluminium 6061-T6', 'Brass "C360"']), density: pick(r, [7.85, 2.7, 8.5]) };
    if (chance(r, 0.5)) part.params = { vars: [{ name: 'w', expr: '40' }, { name: 'h', expr: 'w / 2', note: 'half' }, { name: 'bad', expr: 'nope + 1' }], links: [] };
    if (chance(r, 0.5)) part.drawing = { sheet: pick(r, ['a3', 'ansi-b']), title: { title: 'Bracket', number: pick(r, ['', 'BR-1001']), rev: 'B', company: 'Acme', drawnBy: 'X', date: '2026-01-02', finish: 'Ra 3.2', tolerance: '±0.1 – ISO 2768-m' } };
    const g = regen(part);
    if (!g?.mesh || g.mesh.indices.length / 3 > 8000) continue;
    const gj = KP(part, 'ifcG');
    calls.push({ op: 'ifc.part', args: [part, gj, '2026-10-17T09:30:00.000Z', chance(r, 0.3) ? 'x.ifc' : null] });
    built.push({ part, gj });
  }
  if (built.length) {
    const asm = A.newAssembly(pick(r, ['Gearbox', 'Asm “one”']));
    for (const b of built) A.insertPart(asm, b.part, { place: V.translation([round(r() * 100, 1), 0, round(r() * 50, 1)]) });
    A.insertPart(asm, null, { pid: Object.keys(asm.parts)[0], place: V.rotationAbout([0, 0, 0], [0, 1, 0], 45) });
    const pids = Object.keys(asm.parts);
    const builds = Object.fromEntries(pids.map((pid, i) => [pid, built[i] ? built[i].gj : { mesh: null }]));
    const world = Object.fromEntries(asm.instances.map((i) => [i.id, i.place]));
    if (chance(r, 0.3)) asm.parts[pids[0]].part = JSON.stringify(asm.parts[pids[0]].part);
    if (chance(r, 0.5)) Object.assign(asm.parts[pids[0]], { number: 'P-1', description: 'Main body', material: { name: 'Steel', density: 7.85 } });
    const bom = pids.map((pid, i) => ({ pid, item: i + 1 }));
    calls.push({ op: 'ifc.asm', args: [asm, builds, world, '2026-10-17T09:30:00.000Z', chance(r, 0.7) ? bom : []] });
  }
  for (const seed of ['', 'partcraft|Part|project', 'ü€😀']) calls.push({ op: 'ifc.guid', args: [seed] });
  for (const v of [0, -0.0000001, 1, 1.5, 123456.789, 1e-7, -2.5e-9, 1e21, '$NaN']) calls.push({ op: 'ifc.R', args: [v, null] }, { op: 'ifc.R', args: [v, 12] }, { op: 'ifc.Rq', args: [v] });
  for (const t of ["it's", 'a\\b', 'Ø10 – é', '😀', null]) calls.push({ op: 'ifc.S', args: [t] });
  return [{ calls }];
}

export const GENS = { export: genExport, drawing: genDrawing, analysis: genAnalysis, measure: genMeasure, asmdrawing: genAsmDrawing, ifc: genIfc };
