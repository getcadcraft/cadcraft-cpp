#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// m3d kernel parity (M6.2): model scripts (tests/m3d/*.json: edits of a ModelCraft model as kernel calls) run through
// CadCraft 0.5's ModelCraft kernel (assets/apps/modelcraft/dev/model.js, its own copy, with its English catalog) and
// through `cadcraft-cli m3d-script <file>` (cc::m3d). Compared after every step: what the step returned (or the error
// it threw, in the catalog's words) and the whole model in its file form (toJSON), numbers at abs 1e-9 + rel 1e-12;
// at the end the undo and redo labels.
//   node tools/parity/m3d.mjs <scripts.json>… [--detail] [--max N] [--js-only]
//   … --golden-out <dir>: also write the JS results of each file, canonical, as <dir>/<file name> (the libs/m3d test
//     replays the scripts against them; tests/golden/README.md)
//
// A script: {name, units?, steps: [{op, …}]}. Ops (the kernel call each makes is in OPS below): rect, polygon, ngon,
// edge, arc, pushPull, erase, eraseEdges, move, rotate, scale, copy, reverse, connected, cleanup, tagCurve, curveInfo,
// curveRadius, spline, editSpline, hide, isolate, unhideAll, flip, follow, chainThrough, paint, paintInst, offsetFace,
// offsetChain, solidInfo, solidProblems, fixSolid, soften, intersect, group, explode, makeUnique, purge, place,
// moveInto, copyClip, pasteClip, guideLine, guidePoint, dim, dimLength, edgeLength, defSize, radialDim, angleDim,
// addTag, renameTag, deleteTag, setTag, tagHidden, tagCounts, section, deleteSection, axes, resetAxes, light, setLight,
// deleteLight, captureVis, applyVis, change (nested steps as one undo step), undo, redo, begin, preview, commit, abort,
// load, meshDef, edgesDef, worldTriangles. `path` (instance ids) picks the open group (default: the model).
// Selectors: a face is an id or {facing: n, nth?} / {area: A, facing?} / {last: true}; an edge an id or {between: [p, q]};
// a selection a list of {t, id} or {all: 'faces' | 'edges' | 'insts' | 'everything'}.
import { readFileSync, writeFileSync, mkdirSync, mkdtempSync, rmSync } from 'node:fs';
import { join, basename } from 'node:path';
import { tmpdir } from 'node:os';
import { execFileSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';

const site = process.env.CADCRAFT_SITE || '/var/www/CADCRAFT/getcadcraft.com';
const { canon, diff } = await import(join(site, 'tools/fork-parity/lib/canon.mjs'));
const testfiles = process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles';
const js05 = process.env.FORK_PARITY_JS || join(testfiles, 'fork-parity/js-0.5');
const app = join(js05, 'site/public_html/assets/apps/modelcraft/dev');
const cli = process.env.CC_CLI || 'build/release/apps/cadcraft-cli/cadcraft-cli';
const imp = (p) => import(pathToFileURL(join(app, p)).href);
const { init } = await imp('i18n.js');
init({ locale: 'en', strings: JSON.parse(readFileSync(join(app, 'lang/en.json'), 'utf8')) });
const K = await imp('model.js');
const V = await imp('vec.js');

const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const detail = args.includes('--detail');
const max = Number(opt('--max', '5'));
const goldenOut = opt('--golden-out', null);
const files = args.filter((a, i) => !a.startsWith('--') && !['--max', '--golden-out'].includes(args[i - 1]));

// ---- selectors (cc::m3d's runner resolves them the same way)
const facing = (def, n) => [...def.faces].filter(([, f]) => V.dot(K.faceNormal(def, f), V.norm(n)) > 1 - 1e-9).map(([id]) => id);
function face(def, s) {
  if (typeof s === 'number') return s;
  if (!s || typeof s !== 'object') return null;
  if (s.last) return [...def.faces.keys()].pop() ?? null;
  let ids = s.facing ? facing(def, s.facing) : [...def.faces.keys()];
  if (s.area !== undefined) ids = ids.filter((id) => Math.abs(K.faceArea(def, def.faces.get(id)) - s.area) <= 1e-6);
  if (s.loops !== undefined) ids = ids.filter((id) => def.faces.get(id).loops.length === s.loops);
  return ids[s.nth ?? 0] ?? null;
}
function edge(def, s) {
  if (typeof s === 'number') return s;
  if (s && s.between) {
    for (const [id, e] of def.edges) {
      const a = def.verts.get(e.a), b = def.verts.get(e.b), [p, q] = s.between;
      if ((V.dist(a, p) <= 1e-6 && V.dist(b, q) <= 1e-6) || (V.dist(a, q) <= 1e-6 && V.dist(b, p) <= 1e-6)) return id;
    }
  }
  return null;
}
const edges = (def, l) => (Array.isArray(l) ? l.map((x) => edge(def, x)) : l && l.all ? [...def.edges.keys()] : []);
const faces = (def, l) => (Array.isArray(l) ? l.map((x) => face(def, x)) : l && l.all ? [...def.faces.keys()] : []);
function sel(def, l) {
  if (Array.isArray(l)) return l.map((x) => ({ t: x.t, id: x.t === 'f' ? face(def, x.id) : x.t === 'e' ? edge(def, x.id) : x.id }));
  if (!l || !l.all) return [];
  const out = [];
  if (l.all === 'faces' || l.all === 'everything') for (const id of def.faces.keys()) out.push({ t: 'f', id });
  if (l.all === 'edges' || l.all === 'everything') for (const id of def.edges.keys()) out.push({ t: 'e', id });
  if (l.all === 'insts' || l.all === 'everything') for (const id of def.insts.keys()) out.push({ t: 'i', id });
  return out;
}
const deg = (x) => (x * Math.PI) / 180;
const plain = (x) => (x === undefined ? null : JSON.parse(JSON.stringify(x, (k, v) => (v instanceof Map ? [...v] : v instanceof Set ? [...v] : v))));

let clip = null;
const OPS = {
  rect: (m, d, s) => K.addPolygon(d, K.rectPts(s.p, s.u || [1, 0, 0], s.v || [0, 1, 0], s.w, s.h), s.hint),
  polygon: (m, d, s) => K.addPolygon(d, s.pts, s.hint),
  ngon: (m, d, s) => {
    const pts = K.ngonPts(s.c, s.n || [0, 0, 1], s.u || [1, 0, 0], s.r, s.sides);
    const ids = K.addPolygon(d, pts, s.hint);
    return s.curve ? { edges: ids, curve: K.tagCurve(d, pts, s.curve, true) } : ids;
  },
  edge: (m, d, s) => K.addEdge(d, s.p, s.q, { ...(s.hint ? { hint: s.hint } : {}), ...(s.faces === false ? { faces: false } : {}) }),
  arc: (m, d, s) => {
    const pts = K.arcPts(s.c, s.n || [0, 0, 1], s.u || [1, 0, 0], s.r, deg(s.sweep), s.segs);
    for (let i = 0; i + 1 < pts.length; i++) K.addEdge(d, pts[i], pts[i + 1]);
    return K.tagCurve(d, pts, 'arc');
  },
  pushPull: (m, d, s) => K.pushPull(d, face(d, s.face), s.d),
  erase: (m, d, s) => { K.eraseSel(d, sel(d, s.sel)); },
  eraseEdges: (m, d, s) => { K.eraseEdges(d, edges(d, s.edges)); },
  move: (m, d, s) => { K.transformSel(d, sel(d, s.sel), V.translation(s.by)); },
  rotate: (m, d, s) => { K.transformSel(d, sel(d, s.sel), V.rotation(s.axis, deg(s.deg), s.about || [0, 0, 0])); },
  scale: (m, d, s) => { K.transformSel(d, sel(d, s.sel), V.scaling(s.s, s.about || [0, 0, 0])); },
  copy: (m, d, s) => { const out = K.copySel(d, sel(d, s.sel)); if (s.by) K.transformSel(d, out, V.translation(s.by)); return out; },
  reverse: (m, d, s) => { K.reverseFaces(d, faces(d, s.faces)); },
  connected: (m, d, s) => K.connected(d, sel(d, s.sel)),
  cleanup: (m, d) => K.cleanup(d),
  tagCurve: (m, d, s) => K.tagCurve(d, s.pts, s.kind, !!s.closed),
  curveInfo: (m, d, s) => K.curveInfo(d, s.curve),
  curveRadius: (m, d, s) => K.setCurveRadius(d, s.curve, s.r),
  spline: (m, d, s) => K.addSpline(d, s.pts, { closed: !!s.closed, ...(s.segs ? { segs: s.segs } : {}) }),
  editSpline: (m, d, s) => K.editSpline(d, s.curve, s.pts, { ...(s.closed !== undefined ? { closed: s.closed } : {}), ...(s.segs ? { segs: s.segs } : {}) }),
  hide: (m, d, s) => { K.hideSel(d, sel(d, s.sel), s.on !== false); },
  isolate: (m, d, s) => K.isolateSel(d, sel(d, s.sel)),
  unhideAll: (m) => K.unhideAll(m),
  flip: (m, d, s) => K.flipSel(d, sel(d, s.sel), s.c, s.n, !!s.copy),
  follow: (m, d, s) => K.followMe(d, face(d, s.face), edges(d, s.along)),
  chainThrough: (m, d, s) => K.chainThrough(d, edge(d, s.edge)),
  paint: (m, d, s) => { K.paintFaces(d, faces(d, s.faces), s.c ?? null); },
  paintInst: (m, d, s) => { K.paintInstance(d, s.inst, s.c ?? null); },
  offsetFace: (m, d, s) => K.offsetFace(d, face(d, s.face), s.d),
  offsetChain: (m, d, s) => K.offsetChainPts(d, edges(d, s.edges), s.d, s.n || [0, 0, 1]),
  solidInfo: (m, d) => K.solidInfo(d),
  solidProblems: (m, d) => K.solidProblems(d),
  fixSolid: (m, d) => K.fixSolid(d),
  soften: (m, d, s) => K.softenEdges(d, edges(d, s.edges), s.deg),
  intersect: (m, d, s) => K.intersectFaces(m, s.path || [], sel(d, s.sel), s.others || 'model'),
  group: (m, d, s) => m.makeGroup(d, sel(d, s.sel), s.kind || 'group', s.name),
  explode: (m, d, s) => m.explode(d, s.inst),
  makeUnique: (m, d, s) => m.makeUnique(d, s.inst),
  purge: (m) => { m.purge(); },
  place: (m, d, s) => m.place(d, s.def, s.m || V.ident()),
  moveInto: (m, d, s) => m.moveInto(s.from || [], s.inst, s.to || []),
  copyClip: (m, d, s) => { clip = m.copyClip(s.path || [], sel(d, s.sel)); return clip && { ...clip, source: clip.source === m.uid }; },
  pasteClip: (m, d, s) => m.pasteClip(s.path || [], s.clip || clip, s.m || V.ident()),
  guideLine: (m, d, s) => m.addGuideLine(s.p, s.d),
  guidePoint: (m, d, s) => m.addGuidePoint(s.p),
  dim: (m, d, s) => m.addDim(s.a, s.b, s.off || [0, 0, 0], s.prefer || []),
  dimLength: (m, d, s) => m.setDimLength(s.dim, s.L),
  edgeLength: (m, d, s) => m.setEdgeLength(s.path || [], edge(d, s.edge), s.L),
  defSize: (m, d, s) => { m.setDefSize(s.def, s.axis, s.size); },
  radialDim: (m, d, s) => m.addRadialDim(s.path || [], s.curve, s.off || [0, 0, 0], s.kind || 'rad'),
  angleDim: (m, d, s) => m.addAngleDim(s.o, s.a, s.b, s.off || [0, 0, 0], s.prefer || []),
  addTag: (m, d, s) => m.addTag(s.name || ''),
  renameTag: (m, d, s) => m.renameTag(s.old, s.name),
  deleteTag: (m, d, s) => m.deleteTag(s.name),
  setTag: (m, d, s) => { m.setTag(d, s.insts, s.name ?? null); },
  tagHidden: (m, d, s) => { m.setTagHidden(s.name, !!s.hidden); },
  tagCounts: (m) => [...m.tagCounts()],
  section: (m, d, s) => m.addSection(s.p, s.n, s.name || ''),
  deleteSection: (m, d, s) => { m.deleteSection(s.id); },
  axes: (m, d, s) => { m.setAxes(s.o, s.x, s.y); },
  resetAxes: (m) => { m.resetAxes(); },
  light: (m, d, s) => m.addLight(s.light),
  setLight: (m, d, s) => m.setLight(s.id, s.patch),
  deleteLight: (m, d, s) => m.deleteLight(s.id),
  captureVis: (m) => m.captureVis(),
  applyVis: (m, d, s) => { m.applyVis(s.vis); },
  undo: (m) => m.undo(),
  redo: (m) => m.redo(),
  begin: (m) => { m.begin(); },
  commit: (m, d, s) => { m.commit(s.label); },
  abort: (m) => { m.abort(); },
  load: (m, d, s) => { m.load(s.json); },
  meshDef: (m, d, s) => { const nd = K.meshDef(m, s.polys, s.name, s.kind || 'group', s.segs || []); return s.place ? m.place(d, nd.id, V.ident()) : nd.id; },
  edgesDef: (m, d, s) => { const nd = K.edgesDef(m, s.segs, s.name); return s.place ? m.place(d, nd.id, V.ident()) : nd.id; },
  worldTriangles: (m) => K.worldTriangles(m),
};

function step(m, s, log) {
  let r, err = null;
  try {
    if (s.op === 'change') r = m.change(s.label, (mm) => { for (const t of s.steps) { const d = mm.context(t.path || []).def; OPS[t.op](mm, d, t); } });
    else if (s.op === 'preview') r = m.preview((mm) => { for (const t of s.steps) OPS[t.op](mm, mm.context(t.path || []).def, t); });
    else if (!OPS[s.op]) throw new Error('unknown op ' + s.op);
    else {
      const c = m.context(s.path || []);
      if (!c) throw new Error('no such context');
      r = OPS[s.op](m, c.def, s);
      // an edit outside change() edits in place: it says so, as the app's code does (model.js: "edits in place and
      // sets version++"), or the kernel's kept definition JSON would be stale and the next change() records a step
      // that changed nothing
      if (!['undo', 'redo', 'begin', 'commit', 'abort', 'load'].includes(s.op)) m.version++;
    }
  } catch (e) { err = String(e.message); }
  log.push({ op: s.op, ...(err !== null ? { err } : { r: plain(r) }), model: plain(m.toJSON()) });
}

function runJs(script) {
  clip = null;
  const m = new K.Model();
  if (script.units) m.units = script.units;
  const log = [];
  for (const s of script.steps) step(m, s, log);
  return { name: script.name, steps: log, undo: m.undoStack.map((x) => x.label), redo: m.redoStack.map((x) => x.label) };
}

const TOL = { abs: 1e-9, rel: 1e-12 };
let total = 0, same = 0;
for (const file of files) {
  const scripts = JSON.parse(readFileSync(file, 'utf8')).scripts;
  const want = scripts.map(runJs);
  if (args.includes('--emit-models')) {  // tools/parity/m3d-io.mjs: the model each script ends with
    for (const w of want) console.log(JSON.stringify({ file: basename(file), name: w.name, model: w.steps[w.steps.length - 1].model }));
    continue;
  }
  if (args.includes('--js-only')) {  // what the 0.5 kernel does, step by step (writing scripts)
    for (const w of want) { console.log('# ' + w.name); for (const st of w.steps) console.log('  ' + st.op + ' → ' + JSON.stringify(st.err !== undefined ? { err: st.err } : st.r).slice(0, 300)); }
    continue;
  }
  if (goldenOut) { mkdirSync(goldenOut, { recursive: true }); writeFileSync(join(goldenOut, basename(file)), JSON.stringify(canon(want)) + '\n'); }
  const dir = mkdtempSync(join(tmpdir(), 'm3d-'));
  let got;
  try {
    const out = execFileSync(cli, ['m3d-script', file], { maxBuffer: 1 << 30 }).toString();
    got = out.trim().split('\n').map((l) => JSON.parse(l));
  } finally { rmSync(dir, { recursive: true, force: true }); }
  want.forEach((w, i) => {
    total++;
    const g = got[i];
    const d = g ? diff(canon(w), canon(g), TOL) : ['missing'];
    if (!d.length) { same++; return; }
    let first = -1;
    if (g) for (let k = 0; k < w.steps.length; k++) if (diff(canon(w.steps[k]), canon(g.steps?.[k]), TOL).length) { first = k; break; }
    console.log(`  differs: ${basename(file)} · ${w.name}${first >= 0 ? ` (first at step ${first + 1}: ${w.steps[first].op})` : ''}`);
    if (detail) for (const x of d.slice(0, max)) console.log('    ' + JSON.stringify(x).slice(0, 400));
  });
}
if (!args.includes('--emit-models')) console.log(`m3d: ${same}/${total} scripts the same as the 0.5 kernel`);
process.exitCode = same === total ? 0 : 1;
