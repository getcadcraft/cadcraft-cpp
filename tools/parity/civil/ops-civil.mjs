// SPDX-License-Identifier: Apache-2.0
// ops-civil.mjs (from the Rust fork's cvdiff) — ops for the design modules (grading … project) on the FROZEN JS;
// twins of libs/civil's script_*.cpp.
import { mod } from './env.mjs';

const GR = await mod('grading.js'), CO = await mod('corridor.js'), SE = await mod('superelev.js'), PI = await mod('pipes.js');
const HY = await mod('hydro.js'), SEC = await mod('sections.js'), FL = await mod('featureline.js'), DK = await mod('desckeys.js');
const PC = await mod('parcels.js'), TV = await mod('traverse.js'), SK = await mod('staking.js'), MH = await mod('masshaul.js');
const WHY = await mod('solve-why.js'), CL = await mod('clip.js'), PE = await mod('point-edit.js'), DR = await mod('drive.js');
const PJ = await mod('project.js'), TIN = await mod('tin.js');

export const surfView = (s) => ({ X: Array.from(s.X), Y: Array.from(s.Y), Z: Array.from(s.Z), tris: Array.from(s.tris), ox: s.ox, oy: s.oy, nT: s.nT, constrainedEdges: s.constrainedEdges });
const alView = (a) => ({ elements: a.elements, start: a.start, errors: a.errors, pis: a.pis.map((p) => ({ x: p.x, y: p.y, r: Number(p.r) || 0 })), length: a.length, end: a.end, ok: a.ok });
const profView = (p) => ({ pvis: p.pvis, grades: p.grades, curves: p.curves, errors: p.errors, start: p.start, end: p.end, ok: p.ok });
const mapObj = (m, f = (v) => v) => { const o = {}; for (const [k, v] of m) o[k] = f(v); return o; };
const analysisView = (a) => ({
  structs: mapObj(a.structs, ({ rim, rimFrom, ins, outs, sump }) => ({ rim, rimFrom, ins, outs, sump })),
  pipes: mapObj(a.pipes, ({ len, slope, cover, coverAt, ground }) => ({ len, slope, cover, coverAt, ground })),
  problems: a.problems,
});
const flowsView = (f) => ({ structs: mapObj(f.structs), pipes: mapObj(f.pipes), problems: f.problems, hydro: f.hydro });
const zSource = (reg, v) => {
  if (v?.profile && reg.has(v.profile)) { const p = reg.get(v.profile); return (sta) => p.elevationAt(sta); }
  if (v?.surface && reg.has(v.surface)) { const s = reg.get(v.surface); return (sta, off, x, y) => s.elevationAt(x, y); }
  if (v && typeof v.flat === 'number') return () => v.flat;
  return () => null;
};
const gradedView = (g) => ({ surface: g.surface ? surfView(g.surface) : null, report: g.report, regions: g.regions, items: g.items });

export function applyOps(state, ops) {
  for (const op of ops || []) {
    const path = op.set || op.push || op.del;
    if (!path) continue;
    let cur = state;
    let ok = true;
    for (const k of path.slice(0, -1)) { if (cur && typeof cur === 'object' && Object.prototype.hasOwnProperty.call(cur, k)) cur = cur[k]; else { ok = false; break; } }
    if (!ok || !cur || typeof cur !== 'object') continue;
    const last = path[path.length - 1];
    if (op.set) { if (typeof last === 'string' && !Array.isArray(cur)) cur[last] = op.value ?? null; else if (typeof last === 'number' && Array.isArray(cur) && last < cur.length) cur[last] = op.value ?? null; }
    else if (op.push) { if (typeof last === 'string' && !Array.isArray(cur) && Array.isArray(cur[last])) cur[last].push(op.value ?? null); }
    else if (op.del) { if (typeof last === 'string' && !Array.isArray(cur)) delete cur[last]; else if (typeof last === 'number' && Array.isArray(cur) && last < cur.length) cur.splice(last, 1); }
  }
}
const pview = (p) => ({ state: p.state, points: p.points, pointsFile: p.pointsFile, version: p.version, pointsVersion: p.pointsVersion, gisVersion: p.gisVersion, undo: p.undoStack.map((s) => s.label), redo: p.redoStack.map((s) => s.label), dirty: p.dirty, gisFeatures: Object.fromEntries([...p.gisFeatures].map(([id, fs]) => [id, fs.map((f) => [f.g, f.p])])) });
const alOf = (p, id) => p.state.alignments.find((a) => a.id === id);

export const OPS = {
  'grading.castSlope': (a, s, reg) => GR.castSlope(reg.get(a[0]), a[1], a[2], a[3], a[4], a[5], a[6], a[7] ?? undefined),
  'grading.daylightPad': (a, s, reg) => GR.daylightPad(reg.get(a[0]), a[1], a[2] ?? undefined),
  'grading.gridVolume': (a, s, reg) => GR.gridVolume(reg.get(a[0]), reg.get(a[1]), a[2] ?? undefined),
  'grading.selfCrosses': (a) => GR.selfCrosses(a[0]),
  'grading.gradedSurface': (a, s, reg, call, keep) => {
    const items = (a[3] || []).map((it) => (it && it.corridor ? { name: it.name, patch: (sf) => CO.corridorPatch(sf, reg.get(it.corridor.alignment) ?? null, reg.get(it.corridor.profile) ?? null, it.corridor.asm) } : it));
    const g = GR.gradedSurface(reg.get(a[0]), a[1] || [], a[2] || [], items, (input) => TIN.buildSurface(input));
    if (g.surface) keep(g.surface);
    return { ...g, surface: g.surface ? surfView(g.surface) : null };
  },

  'corridor.defaultAssembly': (a) => CO.defaultAssembly(a[0]),
  'corridor.patch': (a, s, reg) => { const r = CO.corridorPatch(reg.get(a[0]), a[1] == null ? null : reg.get(a[1]), a[2] == null ? null : reg.get(a[2]), a[3]); return r; },

  'super.defaults': (a) => SE.SUPER_DEFAULTS(a[0]),
  'super.clean': (a) => SE.cleanSuper(a[0], a[1]),
  'super.rateFor': (a) => SE.rateFor(a[0], a[1]),
  'super.plan': (a, s, reg, call, keep) => { const p = SE.superPlan(reg.get(a[0]), a[1] ?? undefined); keep(p); return p; },
  'super.slopesAt': (a, s) => SE.slopesAt(s, a[0], a[1] ?? undefined),
  'super.keyStations': (a, s) => SE.keyStations(s),

  'pipes.analyze': (a, s, reg) => analysisView(PI.analyzeNetwork(a[0], a[1] == null ? null : reg.get(a[1]) ?? null, a[2])),
  'pipes.autoInverts': (a, s, reg) => PI.autoInverts(a[0], a[1] == null ? null : reg.get(a[1]) ?? null, a[2], a[3]?.only ? { only: new Set(a[3].only) } : undefined),
  'pipes.flowOrder': (a) => { const o = PI.flowOrder(a[0]); return o ? o.map((p) => p.id) : null; },
  'pipes.defaultNetwork': (a) => PI.defaultNetwork(a[0]),

  'hydro.defaults': (a) => HY.HYDRO_DEFAULTS(a[0]),
  'hydro.clean': (a) => HY.cleanHydro(a[0], a[1]),
  'hydro.cleanCatchment': (a) => HY.cleanCatchment(a[0]),
  'hydro.area': (a) => HY.catchmentArea(a[0], a[1]),
  'hydro.idf': (a) => HY.idf(a[0], a[1]),
  'hydro.manningFull': (a) => HY.manningFull(a[0], a[1], a[2], a[3]),
  'hydro.partFull': (a) => HY.partFull(a[0], a[1]),
  'hydro.normalDepth': (a) => HY.normalDepth(a[0], a[1], a[2], a[3], a[4]),
  'hydro.flows': (a, s, reg, call, keep) => { const f = HY.rationalFlows(a[0], a[1]); keep(f); return flowsView(f); },
  'hydro.size': (a, s, reg) => HY.sizePipes(a[0], reg.get(a[1]), a[2], a[3] ?? undefined),
  'hydro.grade': (a, s, reg) => { const g = HY.hydraulicGrade(a[0], reg.get(a[1]), a[2], a[3] == null ? null : reg.get(a[3]) ?? null); return { pipes: mapObj(g.pipes), structs: mapObj(g.structs), problems: g.problems }; },

  'sec.defaults': (a) => ({ ...SEC.SAMPLE_DEFAULTS(a[0]), extra: [] }),
  'sec.clean': (a) => SEC.cleanSampling(a[0], a[1]),
  'sec.stations': (a, s, reg) => SEC.sampleStations(reg.get(a[0]), a[1] ?? undefined),
  'sec.line': (a, s, reg) => SEC.sampleLine(reg.get(a[0]), a[1], a[2], a[3]),
  'sec.surfaceSection': (a, s, reg) => SEC.surfaceSection(a[0] == null ? null : reg.get(a[0]) ?? null, a[1], a[2], a[3], a[4]),
  'sec.zAt': (a) => SEC.zAt(a[0], a[1]),
  'sec.cutFill': (a) => SEC.cutFill(a[0], a[1]),
  'sec.featureMarks': (a) => SEC.featureMarks(a[0], a[1]),
  'sec.regionSpans': (a) => SEC.regionSpans(a[0], a[1]),
  'sec.clipRuns': (a) => SEC.clipRuns(a[0], a[1]),
  'sec.build': (a, s, reg, call, keep) => {
    const o = a[0], g = o.ground == null ? null : reg.get(o.ground) ?? null, d = o.design == null ? null : reg.get(o.design) ?? null;
    const set = SEC.buildSections({ alignment: reg.get(o.alignment), ground: g, design: d, regions: o.regions ?? null, corridor: o.corridor ?? null, sampling: o.sampling ?? null, from: o.from ?? undefined, to: o.to ?? undefined });
    keep(set); return set;
  },
  'sec.project': (a, s, reg, call, keep) => { const p = reg.get(a[0]), al = p.state.alignments.find((x) => x.id === a[1]); if (!al) return null; const set = p.sections(al); if (!set) return null; keep(set); return set; },
  'sec.csv': (a, s) => SEC.sectionsCsv(s, a[0] ?? undefined),

  'fl.clean': (a) => FL.cleanFeature(a[0], a[1]),
  'fl.stations': (a) => FL.stations(a[0]),
  'fl.segments': (a) => FL.segments(a[0]),
  'fl.setElevation': (a) => FL.setElevation(a[0], a[1], a[2]),
  'fl.setGrade': (a) => FL.setGrade(a[0], a[1], a[2]),
  'fl.raise': (a) => FL.raise(a[0], a[1]),
  'fl.evenGrade': (a) => FL.evenGrade(a[0], a[1], a[2]),
  'fl.fromSurface': (a, s, reg) => FL.fromSurface(a[0], a[1] == null ? null : reg.get(a[1]) ?? null, a[2] ?? null),
  'fl.offset': (a) => FL.offsetFeature(a[0], a[1], a[2] ?? undefined),
  'fl.breaklines': (a) => FL.featureBreaklines(a[0], a[1]),
  'fl.item': (a) => { const it = FL.featureItem(a[0]); if (!it) return null; if (it.patch) { const { patch, ...rest } = it; return { ...rest, patch: patch() }; } return it; },
  'fl.gradeLabel': (a) => FL.gradeLabel(a[0]),
  'fl.crossings': (a, s, reg) => FL.alignmentCrossings(reg.get(a[0]), a[1]),
  'fl.runs': (a, s, reg) => FL.projectedRuns(reg.get(a[0]), a[1], a[2]),
  'fl.profile': (a, s, reg) => FL.featureProfile(reg.get(a[0]), a[1], a[2] ?? undefined),
  'fl.overlay': (a) => { const t = a[1] || {}, k = t.k ?? 1, ox = t.ox ?? 0, oy = t.oy ?? 0; return FL.gradeOverlay(a[0], (x, y) => ({ x: (x - ox) * k, y: -(y - oy) * k }), a[2] ?? undefined); },

  'dk.cleanKeys': (a) => DK.cleanDescKeys(a[0]),
  'dk.cleanLineCodes': (a) => DK.cleanLineCodes(a[0]),
  'dk.parseDesc': (a) => DK.parseDesc(a[0], a[1] ?? undefined),
  'dk.groups': (a) => a[1].map(DK.grouper(a[0])),
  'dk.figures': (a) => DK.figures(a[0], { ...(a[1] || {}), only: a[1]?.only ? new Set(a[1].only) : null }),
  'dk.keyStats': (a) => DK.keyStats(a[0], a[1]),

  'pc.clean': (a) => PC.cleanParcel(a[0]),
  'pc.parseAngle': (a) => PC.parseAngle(a[0]),
  'pc.parseBearing': (a) => PC.parseBearing(a[0]),
  'pc.parseCalls': (a) => PC.parseCalls(a[0]),
  'pc.runText': (a) => PC.runCalls(a[0], PC.parseCalls(a[1]).calls),
  'pc.runCalls': (a) => PC.runCalls(a[0], a[1]),
  'pc.courses': (a) => PC.courses(a[0]),
  'pc.area': (a) => PC.parcelArea(a[0]),
  'pc.perimeter': (a) => PC.perimeter(a[0]),
  'pc.centroid': (a) => PC.centroid(a[0]),
  'pc.areaWords': (a) => PC.areaWords(a[0], a[1]),
  'pc.legal': (a) => PC.legalDescription(a[0], a[1] ?? undefined),
  'pc.labels': (a) => PC.parcelLabels(a[0], a[1] ?? undefined),

  'tv.parse': (a) => TV.parseTraverse(a[0], a[1] ?? undefined),
  'tv.run': (a, s, reg, call, keep) => { const o = a[0] && typeof a[0].legsText === 'string' ? { ...a[0], legs: TV.parseTraverse(a[0].legsText, a[0].mode).legs } : a[0]; const r = TV.runTraverse(o); keep(r); return r; },
  'tv.report': (a, s) => TV.traverseReport(s, a[0] ?? undefined),
  'tv.dir2az': (a) => TV.dir2az(a[0]),

  'sk.parseOffsets': (a) => SK.parseOffsets(a[0]),
  'sk.cutFillText': (a) => SK.cutFillText(a[0]),
  'sk.offsetText': (a) => SK.offsetText(a[0]),
  'sk.csv': (a) => SK.stakingCsv(a[0], a[1] ?? undefined),
  'sk.stations': (a, s, reg) => SK.stakeStations(reg.get(a[0]), a[1] ?? undefined),
  'sk.rows': (a, s, reg) => { const o = a[1]; const d = zSource(reg, o.design), g = zSource(reg, o.ground); return SK.stakeRows(reg.get(a[0]), { ...o, design: (sta, off, x, y) => d(sta, off, x, y), ground: (x, y) => g(NaN, 0, x, y) }); },
  'sk.pointRows': (a, s, reg) => { const o = a[2] || {}; const d = zSource(reg, o.design), g = o.ground ? zSource(reg, o.ground) : null; return SK.pointRows(reg.get(a[0]), a[1], { design: (sta, off, x, y) => d(sta, off, x, y), ground: g ? (x, y) => g(NaN, 0, x, y) : null }); },

  'mh.massHaul': (a, s, reg, call, keep) => { const m = MH.massHaul(a[0], a[1] ?? undefined); keep(m); return m; },
  'mh.project': (a, s, reg, call, keep) => { const p = reg.get(a[0]), al = p.state.alignments.find((x) => x.id === a[1]); if (!al) return null; const set = p.sections(al); if (!set) return null; const m = MH.massHaul(set.rows, a[2] ?? undefined); keep(m); return m; },
  'mh.csv': (a, s) => MH.massHaulCsv(s, a[0] ?? undefined),

  'why.curve': (a) => WHY.curveWhy(a[0], a[1] ?? undefined),
  'why.maxRadius': (a) => WHY.maxRadius(a[0], a[1]),
  'why.vertical': (a) => WHY.verticalWhy(a[0]),

  'clip.pointsText': (a) => CL.pointsText(a[0], a[1] ?? undefined),
  'clip.make': (a) => ({ ...CL.makeClip(a[0], a[1] ?? undefined), at: 0 }),
  'clip.read': (a) => CL.readClip(a[0]),
  'clip.textPoints': (a) => CL.textPoints(a[0], a[1] ?? undefined),
  'clip.choose': (a) => CL.choosePaste(a[0] == null ? null : CL.readClip(a[0]), a[1], a[2] ?? undefined),
  'clip.renumber': (a) => CL.renumber(a[0], a[1], a[2] ?? undefined),
  'clip.renumberText': (a) => CL.renumberText(a[0], a[1]),
  'clip.moveBy': (a) => CL.moveBy(a[0], a[1], a[2]),
  'clip.centre': (a) => CL.centre(a[0]),

  'pe.check': (a) => PE.checkPointEdit(a[0], a[1], a[2] ?? undefined),

  'drive.speeds': (a) => DR.DRIVE_SPEEDS[a[0] === 'm' ? 'm' : 'ft'],
  'drive.eye': (a) => DR.EYE_HEIGHT[a[0] === 'm' ? 'm' : 'ft'],
  'drive.sun': (a) => DR.sunPosition(a[0], a[1], a[2]),
  'drive.sunVector': (a) => DR.sunVector({ azimuth: a[0], altitude: a[1] }),
  'drive.frames': (a, s, reg) => {
    const al = reg.get(a[0]), cfg = a[1] || {}, prof = cfg.profile ? reg.get(cfg.profile) ?? null : null, surf = cfg.ground ? reg.get(cfg.ground) ?? null : null;
    const elev = DR.elevationAlong(al, prof, surf ? (x, y) => surf.elevationAt(x, y) : null);
    return a[2].map((sta) => DR.driveFrame(al, elev, sta, a[3] ?? undefined));
  },

  'project.new': (a, s, reg, call, keep) => { const p = new PJ.Project(); keep(p); return pview(p); },
  'project.reload': (a, p, reg, call, keep) => { const j = JSON.parse(JSON.stringify(p.toJSON())); applyOps(j, a[0]); const { project, notes } = PJ.Project.fromJSON(j); keep(project); return { notes, view: pview(project) }; },
  'project.fromJSON': (a, s, reg, call, keep) => { const { project, notes } = PJ.Project.fromJSON(a[0]); keep(project); return { notes, view: pview(project) }; },
  'project.view': (a, p) => pview(p),
  'project.setPoints': (a, p) => { p.setPoints(a[0], a[1] ?? ''); return null; },
  'project.editPoints': (a, p) => { const e = a[1] || {}; return p.editPoints(a[0], (pts) => { if (e.none) return null; let out = pts; if (e.set) out = e.set.map((q) => ({ ...q })); if (e.remove) out = out.filter((q) => !e.remove.includes(q.n)); if (e.add) out.push(...e.add.map((q) => ({ ...q }))); return out; }); },
  'project.change': (a, p) => p.change(a[0], (st) => applyOps(st, a[1])),
  'project.setDisplay': (a, p) => { p.setDisplay(a[0], a[1]); return null; },
  'project.undo': (a, p) => p.undo(),
  'project.redo': (a, p) => p.redo(),
  'project.nextId': (a, p) => p.nextId(a[0]),
  'project.setTin': (a, p) => p.setTin(a[0], a[1], a[2] && a[2].length ? (st) => applyOps(st, a[2]) : null),
  'project.setCoding': (a, p) => (a[2] === undefined || a[2] === null ? p.setCoding(a[0], a[1]) : p.setCoding(a[0], a[1], a[2])),
  'project.groups': (a, p) => p.groups(),
  'project.figures': (a, p) => p.figures(a[0] ? new Set(a[0]) : null),
  'project.egBreaklines': (a, p) => p.egBreaklines(),
  'project.egPoints': (a, p) => p.egPoints(),
  'project.eg': (a, p) => { const r = p.eg(); return { surface: r.surface ? surfView(r.surface) : null, report: r.report }; },
  'project.contours': (a, p) => p.contours(),
  'project.alignment': (a, p) => { const al = alOf(p, a[0]); return al ? alView(p.alignment(al)) : null; },
  'project.egProfile': (a, p) => { const al = alOf(p, a[0]); return al ? p.egProfile(al) : null; },
  'project.designProfile': (a, p) => { const al = alOf(p, a[0]); return al ? profView(p.designProfile(al)) : null; },
  'project.graded': (a, p) => gradedView(p.graded()),
  'project.sections': (a, p) => { const al = alOf(p, a[0]); return al ? p.sections(al) : null; },
  'project.volumes': (a, p) => p.volumes(a[0] ?? undefined),
  'project.networkCheck': (a, p) => { const r = p.networkCheck(); return r ? analysisView(r) : null; },
  'project.toJSON': (a, p) => { const { saved, ...rest } = p.toJSON(); return rest; },
  'project.editPoint': (a, p) => PE.editPoint(p, a[0], a[1], a[2] ? { label: a[2] } : undefined),
};
