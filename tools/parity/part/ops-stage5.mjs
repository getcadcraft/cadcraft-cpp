// SPDX-License-Identifier: Apache-2.0
// pcdiff/ops-stage5.mjs — JS ops for cc-part stage 5 (M10.5): export.js and the drawing modules. Mirrored by
// crates/part/src/script5.rs. Byte outputs are Uint8Arrays (the parity kit hashes them).
import { DEV } from './driver.mjs';

import { inflateSync } from 'node:zlib';
export const X = await import(DEV + 'export.js');
export const DR = await import(DEV + 'drawing.js');
const G = await import(DEV + 'gdt.js');
const AN = await import(DEV + 'analysis.js');
const SEC = await import(DEV + 'section.js');
const SU = await import(DEV + 'surface.js');
const MS = await import(DEV + 'measure.js');
const AD = await import(DEV + 'asm-drawing.js');
const IE = await import(DEV + 'ifc-export.js');
const PRM = await import(DEV + 'params.js');
const PT = await import(DEV + 'part.js');
const buildOf = (b) => ({ ...b, ...(b.mesh ? { mesh: { positions: Float32Array.from(b.mesh.positions), indices: Uint32Array.from(b.mesh.indices), ...(b.mesh.triFace ? { triFace: Int32Array.from(b.mesh.triFace) } : {}), ...(b.mesh.edgeSegs ? { edgeSegs: Float32Array.from(b.mesh.edgeSegs), segEdge: Int32Array.from(b.mesh.segEdge) } : {}) } } : {}) });
const projOf = (m) => (p) => { const r = (i) => m[i * 4] * p[0] + m[i * 4 + 1] * p[1] + m[i * 4 + 2] * p[2] + m[i * 4 + 3]; return r(2) < 0 ? null : [r(0), r(1)]; };
const rowsOut = (rows) => ({ rows, text: MS.asText(rows) });
const meshOf = (m) => ({ positions: Float32Array.from(m.positions), indices: Uint32Array.from(m.indices), ...(m.triFace ? { triFace: Int32Array.from(m.triFace) } : {}) });

export const OPS = {
  'export.stl': (m, name) => X.stlBinary(meshOf(m), name ?? undefined),
  'export.3mf': (m, name) => X.threeMf(meshOf(m), name ?? undefined),
  'export.crc32': (s) => X.crc32(new TextEncoder().encode(s)),
  'export.zip': (files) => X.zipStored(files),
  'export.dxfSketch': (sk) => X.dxfSketch(sk),
  'export.dxfLoops': (loops) => X.dxfLoops(loops),
  'export.dxfFlat': (flat) => X.dxfFlat(flat),
  'export.step': (m, name, iso, schema) => X.stepFaceted(meshOf(m), name ?? undefined, { date: new Date(iso), schema: schema ?? undefined }),
  'draw.sheetLabel': (k) => DR.sheetLabel(k),
  'draw.nextRevision': (revs) => DR.nextRevision(revs),
  'draw.scaleLabel': (s) => DR.scaleLabel(s),
  'draw.meshEdges': (m) => DR.meshEdges(meshOf(m)),
  'draw.meshViews': (m, iso) => DR.meshViews(meshOf(m), { iso: iso ?? undefined }),
  'draw.mergeSegments': (segs, size, minus) => DR.mergeSegments(segs, size, minus ?? undefined),
  'draw.hatch': (segs, gap, angle) => DR.hatch(segs, gap, angle ?? undefined),
  'draw.clipToCircle': (segs, c, r) => DR.clipToCircle(segs, c, r),
  'draw.partHoles': (part, g) => DR.partHoles(part, { ...g, threads: g.threads ? new Map(Object.entries(g.threads)) : undefined }),
  'draw.holeTableRows': (holes, view, box) => DR.holeTableRows(holes, view, box),
  'draw.compose': (data, o) => DR.composeSheet(data, { ...o, name: o.name ?? undefined, date: new Date(o.date), annotate: o.annotate ? G.annotationGeometry : null, holes: o.holes ?? null }),
  'draw.makeDrawing': (m, o) => DR.makeDrawing(meshOf(m), { ...o, name: o.name ?? undefined, date: new Date(o.date), annotate: o.annotate ? G.annotationGeometry : null, holes: o.holes ?? null }),
  'draw.winAnsi': (s) => DR.winAnsi(s),
  'draw.pdfContent': async (sheet) => {
    const { bytes, missing } = await DR.toPdf(sheet);
    const txt = Buffer.from(bytes).toString('latin1');
    const a = txt.indexOf('stream\n') + 7, b = txt.indexOf('\nendstream');
    return { content: inflateSync(Buffer.from(bytes.slice(a, b))).toString('latin1'), missing };
  },
  'an.draft': (m, cn, pull, minDeg, colors) => AN.draftAnalysis({ ...meshOf(m), ...(cn ? { cornerNormals: Float32Array.from(cn) } : {}) }, pull, minDeg, colors),
  'an.zebra': (m, cn, view, stripes, c) => AN.zebraColors({ ...meshOf(m), ...(cn ? { cornerNormals: Float32Array.from(cn) } : {}) }, view, stripes, c),
  'an.curvature': (m, c) => AN.curvatureColors(meshOf(m), c),
  'an.curvatureRaw': (m) => SU.curvature(meshOf(m)),
  'an.draftAngle': (n, pull) => AN.draftAngle(n, pull),
  'sec.range': (box, n) => SEC.sectionRange(box, n),
  'sec.face': (face, M) => SEC.faceSection(face, M ?? null),
  'ms.circleOf': (e, b, i) => MS.circleOf(e, buildOf(b), i),
  'ms.snap': (b) => MS.snapCandidates(buildOf(b)),
  'ms.snapAt': (b, m, x, y, tol) => MS.snapAt(MS.snapCandidates(buildOf(b)), projOf(m), x, y, tol),
  'ms.nearestEdge': (b, m, x, y, tol) => MS.nearestEdgeAt(buildOf(b), projOf(m), x, y, tol),
  'ms.info': (b, pick) => MS.info(buildOf(b), pick),
  'ms.exactSub': (pick) => MS.exactSub(pick),
  'ms.geometry': (b, pick) => MS.geometryOf(buildOf(b), pick),
  'ms.pair': (b, A, B, exact) => { const bb = buildOf(b); const ia = MS.info(bb, A), ib = MS.info(bb, B); const res = MS.pair({ ...A, geometry: MS.geometryOf(bb, A) }, { ...B, geometry: MS.geometryOf(bb, B) }, ia, ib, exact ?? null); return { res, ...rowsOut(MS.pairRows(res)) }; },
  'ms.infoRows': (inf) => rowsOut(MS.infoRows(inf)),
  'ms.meshMass': (m) => MS.meshMass(meshOf(m)),
  'ms.place': (p, M, d) => MS.placeMass(p, M ?? null, d),
  'ms.combine': (list) => MS.combineMass(list),
  'ms.principal': (T) => MS.principal(T),
  'ms.massRows': (m) => rowsOut(MS.massRows(m)),
  'ms.kindLabel': (k) => MS.kindLabel(k),
  'adw.fit': (s, size, w) => AD.fit(s, size, w),
  'adw.balloonsFor': (rows, insts, anchors) => AD.balloonsFor(rows, insts, anchors),
  'adw.sheet': (data, rows, o) => AD.assemblySheet(data, rows, { ...o, name: o.name ?? undefined, paper: o.paper ?? undefined, date: new Date(o.date), exploded: o.exploded ?? null }),
  'adw.mesh': (items) => AD.assemblyMesh(items.map((it) => ({ mesh: meshOf(it.mesh), matrix: it.matrix }))),
  'adw.3mf': (name, parts, items) => AD.assembly3mf(name, new Map(parts.map(([k, p]) => [k, { name: p.name, mesh: meshOf(p.mesh) }])), items),
  'ifc.guid': (seed) => IE.guidFor(seed),
  'ifc.R': (v, d) => IE.R(v, d ?? undefined),
  'ifc.Rq': (v) => IE.Rq(v),
  'ifc.S': (s) => IE.S(s),
  'ifc.part': (part, g, now, fileName) => IE.partIfc(part, { mesh: meshOf(g.mesh), stats: g.stats }, { now: new Date(now), evaluate: PRM.evaluateVars, ...(fileName ? { fileName } : {}) }),
  'ifc.asm': (asm, builds, world, now, bom) => IE.assemblyIfc(asm, new Map(Object.entries(builds).map(([k, b]) => [k, { mesh: b.mesh ? meshOf(b.mesh) : null, stats: b.stats }])), new Map(Object.entries(world)), { now: new Date(now), evaluate: PRM.evaluateVars, readPart: PT.readPart, bom }),
};
