// SPDX-License-Identifier: Apache-2.0
// tools/parity/part/driver.mjs — the JS side of tools/parity/part.mjs: the FROZEN 0.5 PartCraft modules called with the
// op names `cadcraft-cli part` answers (libs/partcraft script.cpp; the Rust fork's cc-part answers the same ones). Each op takes plain JSON args and returns plain JSON
// (Maps / Sets are canonicalised by the parity kit); ops that change a document return it with the result.
import { readFileSync } from 'node:fs';

export const DEV = (process.env.FORK_PARITY_JS || (process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles') + '/fork-parity/js-0.5') + '/site/public_html/assets/apps/partcraft/dev/';
const I18N = await import(DEV + 'i18n.js');
I18N.init({ locale: 'en', strings: JSON.parse(readFileSync(DEV + 'lang/en.json', 'utf8')) });

export const E = await import(DEV + 'expr.js');
export const PR = await import(DEV + 'params.js');
export const PP = await import(DEV + 'params-part.js');
export const S = await import(DEV + 'sketch.js');
export const SP = await import(DEV + 'sketch-spline.js');
export const SV = await import(DEV + 'solver.js');
export const P = await import(DEV + 'part.js');
export const CF = await import(DEV + 'configs.js');
export const G = await import(DEV + 'gdt.js');
export const BOM = await import(DEV + 'bom.js');
export const TH = await import(DEV + 'threads.js');
export const FM = await import(DEV + 'form.js');
export const FONT = await import(DEV + 'font.js');
// lib/shapes (the canonical copy; partcraft/dev/shapes/parts.js and cadcraft/dev/shapes/* are byte-identical)
export const SHAPES = (process.env.FORK_PARITY_JS || (process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles') + '/fork-parity/js-0.5') + '/lib/shapes/';
export const SEC = await import(SHAPES + 'sections.js');
export const PARTS = await import(SHAPES + 'parts.js');
export const ITEMS = await import(SHAPES + 'items.js');

const clone = (v) => (v === undefined ? undefined : structuredClone(v));
const num = (v) => (v === '$NaN' ? NaN : v === '$Inf' ? Infinity : v === '$-Inf' ? -Infinity : v);
const omap = (o) => new Map(Object.entries(o).map(([k, v]) => [k, num(v)]));
const exprRes = (r) => (r.ok ? r : { ok: false, error: r.error, pos: r.pos ?? null });
const target = (t) => { const o = { ...t, value: t.get() }; delete o.get; delete o.set; delete o.when; return o; };

export const OPS = {
  // expr.js
  'expr.parse': (src, names) => { const r = E.parse(src, names); return r.ok ? { ok: true, ast: r.ast, uses: r.uses } : exprRes(r); },
  'expr.compute': (src, values) => exprRes(E.compute(src, values || {})),
  'expr.literal': (src) => E.literal(src),
  'expr.formatNumber': (v) => E.formatNumber(num(v)),
  'expr.solveFor': (src, values, name, tgt) => { const p = E.parse(src); if (!p.ok) return exprRes(p); return E.solveFor(p.ast, values, name, num(tgt)); },
  'expr.renameIn': (src, a, b) => E.renameIn(src, a, b),
  // params.js
  'params.read': (raw, keys) => PR.readParams(raw, keys ? { targetKeys: new Set(keys) } : {}),
  'params.evaluateVars': (p, ov) => PR.evaluateVars(p, ov ? omap(ov) : null),
  'params.setVar': (p, name, expr, opts) => { p = clone(p); const res = PR.setVar(p, name, expr, opts || {}); return { res, p }; },
  'params.removeVar': (p, name, inline) => { p = clone(p); const res = PR.removeVar(p, name, { inline: !!inline }); return { res, p }; },
  'params.driveVar': (p, name, v, prefer) => { p = clone(p); const res = PR.driveVar(p, name, num(v), { prefer: prefer ?? null }); return { res, p }; },
  'params.drivingBehind': (p, src) => PR.drivingBehind(p, src),
  'params.usesOf': (p, name) => PR.usesOf(p, name),
  'params.readDesignTable': (csv, p) => PR.readDesignTable(csv, p),
  'params.applyOverrides': (p, ov) => { p = clone(p); PR.applyOverrides(p, omap(ov)); return p; },
  // params-part.js
  'pp.targets': (part) => Object.fromEntries([...PP.partAdapter.targets(part)].map(([k, t]) => [k, target(t)])),
  'pp.apply': (part, ov) => { part = clone(part); const res = PP.applyPartParams(part, ov ? { overrides: omap(ov) } : {}); return { res, part }; },
  'pp.setLink': (part, key, expr) => { part = clone(part); const p = PP.paramsOf(part); const res = PR.setLink(p, PP.partAdapter, part, key, expr); return { res, part }; },
  'pp.setVar': (part, name, expr) => { part = clone(part); const p = PP.paramsOf(part); const res = PR.setVar(p, name, expr); return { res, part }; },
  'pp.driveBack': (part, key, v, prefer) => { part = clone(part); const p = part.params || { vars: [], links: [] }; const res = PR.driveBack(p, PP.partAdapter, part, key, num(v), { prefer: prefer ?? null }); return { res, part }; },
  'pp.resolveTarget': (part, text) => PP.resolveTarget(part, text),
  'pp.targetName': (part, key) => PP.targetName(part, key),
  'pp.report': (part) => PP.paramsReport(part),
  'pp.pruneDeleted': (before, after) => { after = clone(after); const res = PP.pruneDeleted(before, after); return { res, after }; },
  'pp.stepRollback': (part, d) => { part = clone(part); const res = PP.stepRollback(part, d); return { res, part }; },
  'pp.rollTo': (part, id) => { part = clone(part); const res = PP.rollTo(part, id); return { res, part }; },
  'pp.rollbackWords': (part) => PP.rollbackWords(part),
  'pp.fieldInUse': (f, field) => PP.fieldInUse(f, field),
  'pp.readPartParams': (part, raw) => PP.readPartParams(raw, part),
  // part.js
  'part.read': (raw) => P.readPart(raw),
  'part.toJSON': (part) => P.toJSON(part),
  'part.fileVersion': (part) => P.fileVersion(part),
  'part.new': (name) => P.newPart(name),
  'part.addFeature': (part, type, props) => { part = clone(part); const f = P.addFeature(part, type, props || {}); return { f, part }; },
  'part.dependsOn': (f) => P.dependsOn(f),
  'part.dependents': (part, id) => P.dependents(part, id),
  'part.canMove': (part, id, to) => P.canMove(part, id, to),
  'part.moveFeature': (part, id, to) => { part = clone(part); const res = P.moveFeature(part, id, to); return { res, part }; },
  'part.deleteFeatures': (part, ids) => { part = clone(part); const names = P.deleteFeatures(part, ids); return { names, part }; },
  'part.builtCount': (part) => P.builtCount(part),
  'part.readDrawing': (d) => P.readDrawing(d),
  // sketch.js / solver.js / sketch-spline.js
  'sketch.read': (raw) => S.readSketch(raw),
  'sketch.solve': (sk, pin, pinR) => { sk = clone(sk); const res = SV.solveSketch(sk, { pin: pin ? new Map(Object.entries(pin)) : null, pinRadius: pinR ? omap(pinR) : null }); return { res, sk }; },
  'sketch.analyze': (sk, tolScale) => SV.analyze(sk, tolScale ?? undefined),
  'sketch.residuals': (sk) => { const { vars, x } = buildVars(sk); return S.equations(sk, x, vars).map((q) => [q.cid, q.f()]); },
  'sketch.profile': (sk, seg) => S.profile(sk, seg),
  'sketch.measure': (sk, type, refs) => S.measure(sk, type, refs),
  'sketch.matchSig': (sk, type, refs) => S.matchSig(sk, type, refs),
  'sketch.applicable': (sk, refs) => S.applicable(sk, refs),
  'sketch.addConstraint': (sk, type, refs, value, extra) => { sk = clone(sk); const res = S.addConstraint(sk, type, refs, value ?? undefined, extra || {}); return { res, sk }; },
  'sketch.addPoint': (sk, x, y, o) => { sk = clone(sk); const res = S.addPoint(sk, x, y, o || {}); return { res, sk }; },
  'sketch.addLine': (sk, a, b, o) => { sk = clone(sk); const res = S.addLine(sk, a, b, o || {}); return { res, sk }; },
  'sketch.addCircle': (sk, c, r, o) => { sk = clone(sk); const res = S.addCircle(sk, c, r, o || {}); return { res, sk }; },
  'sketch.addArc': (sk, c, s, e, o) => { sk = clone(sk); const res = S.addArc(sk, c, s, e, o || {}); return { res, sk }; },
  'sketch.addRect': (sk, a, b, o) => { sk = clone(sk); const res = S.addRect(sk, a, b, o || {}); return { res, sk }; },
  'sketch.addSpline': (sk, pts, o) => { sk = clone(sk); const res = S.addSpline(sk, pts, o || {}); return { res, sk }; },
  'sketch.removeEntities': (sk, ids) => { sk = clone(sk); const res = S.removeEntities(sk, ids); return { res, sk }; },
  'sketch.removeConstraints': (sk, ids) => { sk = clone(sk); S.removeConstraints(sk, ids); return { res: null, sk }; },
  'sketch.filletCorner': (sk, id, r) => { sk = clone(sk); const res = S.filletCorner(sk, id, r); return { res, sk }; },
  'sketch.describe': (c) => S.describe(c),
  'sketch.extent': (sk) => S.extent(sk),
  'sketch.bounds': (sk) => S.bounds(sk),
  'sketch.insideLoops': (loops, p) => S.insideLoops(loops, p),
  'sketch.loopArea': (loop) => S.loopArea(loop),
  'sketch.loosePoints': (sk) => S.loosePoints(sk),
  'sketch.tessArc': (sk, id, seg) => S.tessArc(sk, S.byId(sk, id), seg),
  'sketch.tessSpline': (sk, id, per) => S.tessSpline(sk, S.byId(sk, id), per),
  'sketch.endMeet': (sk, a, b) => S.endMeet(sk, S.byId(sk, a), S.byId(sk, b)),
  'spline.interpFit': (Q, o) => SP.interpFit(Q, o || {}),
  'spline.controlCurve': (P, o) => SP.controlCurve(P, o || {}),
  'spline.tessellate': (c, n) => SP.tessellate(c, n),
  'spline.tessellateTol': (c, tol) => SP.tessellateTol(c, tol),
  'spline.areaTerm': (c) => SP.areaTerm(c),
  'spline.arcLength': (c) => SP.arcLength(c),
  'spline.closestParam': (c, p, n) => SP.closestParam(c, p, n),
  'spline.curvatureAt': (c, u) => SP.curvatureAt(c, u),
  'spline.derivsAt': (c, u, n) => SP.derivsAt(c, u, n),
  'spline.pointAt': (c, u) => SP.pointAt(c, u),
  'spline.insertKnot': (c, u) => SP.insertKnot(c, u),
  'spline.segment': (c, a, b) => SP.segment(c, a, b),
  'spline.reverse': (c) => SP.reverse(c),
  'spline.normalizeKnots': (c) => SP.normalizeKnots(c),
  'spline.knotProblem': (U, m) => SP.knotProblem(U, m),
  'spline.offsetFit': (c, d, tol) => SP.offsetFit(c, d, tol),
  // configs.js
  'configs.read': (raw, p) => CF.readConfigs(raw, p),
  'configs.rowFromCurrent': (p, name) => CF.rowFromCurrent(p, name),
  'configs.saveRow': (cfg, row) => CF.saveRow(cfg, row),
  'configs.applyRow': (p, row) => { p = clone(p); CF.applyRow(p, row); return p; },
  'configs.rowMatches': (p, row) => CF.rowMatches(p, row),
  'configs.renameRow': (cfg, from, to) => CF.renameRow(cfg, from, to),
  'configs.deleteRow': (cfg, name) => CF.deleteRow(cfg, name),
  'configs.followParams': (cfg, b, a) => CF.followParams(cfg, b, a),
  'configs.rowsFromTable': (csv, p) => CF.rowsFromTable(csv, p),
  'configs.csv': (cfg) => CF.configsCsv(cfg),
  // gdt.js, bom.js, threads.js, form.js
  'gdt.readNotes': (raw) => G.readNotes(raw),
  'gdt.validate': (a) => G.validate(a),
  'gdt.describe': (a) => G.describe(a),
  'gdt.newAnnotation': (kind, props) => G.newAnnotation(kind, props || {}),
  'gdt.geometry': (a, anchor) => G.annotationGeometry(a, anchor),
  'gdt.textW': (s, size) => { let w = 0; for (const ch of s) w += FONT.charAdvance(ch); return w / 1000 * size; },
  'bom.readMaterial': (m) => BOM.readMaterial(m),
  'bom.readPartProps': (p) => BOM.readPartProps(p),
  'bom.presetOf': (m) => BOM.presetOf(m),
  'bom.materials': () => BOM.MATERIALS.map((m) => ({ name: m.name, density: m.density })),
  'bom.rows': (asm, vols) => BOM.bomRows(asm, new Map(Object.entries(vols || {}).filter(([, v]) => v).map(([k, v]) => [k, { stats: v }]))),
  'bom.csv': (rows) => BOM.bomCsv(rows),
  'bom.csvCell': (v, o) => BOM.csvCell(v, o || {}),
  'threads.dims': (form, size) => TH.threadDims(form, size),
  'threads.sizeFor': (form, d, ext) => TH.sizeFor(form, d, ext),
  'threads.profileCorners': (form, size, shift) => TH.profileCorners(form, size, shift),
  'threads.profileAt': (c, u) => TH.profileAt(c, u),
  'threads.boltSection': (c, o) => TH.boltSection(c, o || {}),
  'threads.callout': (form, size, o) => TH.threadCallout(form, size, o || {}),
  'threads.sizeLabel': (form, size) => TH.sizeLabel(form, size),
  'form.read': (raw) => FM.readForm(raw),
  'form.box': (...a) => FM.box(...a),
  'i18n.t': (k) => I18N.t(k),
  // lib/shapes: sections.js, parts.js, items.js (a null argument stands for one left out)
  'shapes.constants': () => ({ STEEL_DENSITY: SEC.STEEL_DENSITY, IN: SEC.IN, LBFT: SEC.LBFT, PARTS_STEEL_DENSITY: PARTS.STEEL_DENSITY }),
  'shapes.sources': () => SEC.SECTION_SOURCES,
  'shapes.families': () => SEC.FAMILIES,
  'shapes.sectionRows': () => SEC.sectionRows(),
  'shapes.findSection': (n) => SEC.findSection(n),
  'shapes.familyRows': (id) => SEC.familyRows(id),
  'shapes.displayName': (n) => SEC.displayName(n),
  'shapes.exactArea': (p) => SEC.exactArea(p),
  'shapes.outline': (p, seg) => SEC.outline(p, seg ?? undefined),
  'shapes.bbox': (loop) => SEC.bbox(loop),
  'shapes.shoelace': (loop) => SEC.shoelace(loop),
  'shapes.loopLength': (loop) => SEC.loopLength(loop),
  'shapes.partStandards': () => PARTS.PART_STANDARDS,
  'shapes.partFamilies': () => PARTS.PART_FAMILIES,
  'shapes.findFamily': (id) => PARTS.findFamily(id),
  'shapes.partSizes': (id) => PARTS.partSizes(id),
  'shapes.partOptions': (id, size) => PARTS.partOptions(id, size),
  'shapes.makePart': (id, size, opts) => PARTS.makePart(id, size, opts === null ? undefined : opts === '$null' ? null : opts),
  'shapes.librarySet': () => PARTS.LIBRARY_SET,
  'shapes.partTags': () => ITEMS.PART_TAGS,
  'shapes.cc0': () => ITEMS.CC0,
  'shapes.memberLoops': (name) => ITEMS.memberLoops(SEC.findSection(name)),
  'shapes.sectionItem': (name, o) => ITEMS.sectionItem(SEC.findSection(name), o ?? undefined),
  'shapes.partItem': (id, size, opts) => { const r = PARTS.makePart(id, size, opts ?? undefined); return r.ok ? ITEMS.partItem(r.part) : r; },
  'shapes.localItem': (it) => ITEMS.localItem(it),
};

// solver.js does not export buildVars: the same rule (points, then circles and spline angles)
function buildVars(sk) {
  const vars = new Map(), x = [];
  for (const e of sk.entities) if (e.type === 'point') { vars.set(e.id, [x.length, x.length + 1]); x.push(e.x, e.y); }
  for (const e of sk.entities) {
    if (e.type === 'circle') { vars.set(e.id, x.length); x.push(e.r); }
    if (e.type === 'spline') for (const k of ['a0', 'a1']) if (Number.isFinite(e[k])) { vars.set(e.id + ':' + k, x.length); x.push(e[k]); }
  }
  return { vars, x };
}

/** Run a script: each call's result as { ok } / { err }. */
export const EXTOPS = {};
/** Hooks run before each script (ops-kernel.mjs: a fresh wasm and kernel.js per script). */
export const HOOKS = { beforeScript: [], resolve: null };
async function callOp(op, args) {
  const fn = OPS[op] || EXTOPS[op];
  if (!fn) throw new Error('unknown op ' + op);
  const a = HOOKS.resolve ? await HOOKS.resolve(args || []) : (args || []);
  return await fn(...a.map(clone));
}
export async function run(script) {
  for (const h of HOOKS.beforeScript) await h(script);
  const out = [];
  for (const c of script.calls) {
    if (!(OPS[c.op] || EXTOPS[c.op])) { out.push({ err: 'unknown op ' + c.op }); continue; }
    try { out.push({ ok: await callOp(c.op, c.args) }); }
    catch (e) { out.push({ err: e && e.message !== undefined ? e.message : String(e) }); }
  }
  return out;
}
export { callOp };
