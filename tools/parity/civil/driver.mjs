// SPDX-License-Identifier: Apache-2.0
// Runs a call script on the FROZEN CivilCraft 0.5 modules: the JS side of `civilcraft-cli script`. Every op here has its
// twin in libs/civil (script_*.cpp); results are the same JSON projections. (Adapted from the Rust fork's cvdiff.)
import { mod, dec } from './env.mjs';
// lone UTF-16 surrogates compare as U+FFFD on both sides (the Rust fork's strings cannot hold them)
const wf = (v) => typeof v === 'string' ? v.toWellFormed() : Array.isArray(v) ? v.map(wf) : v && typeof v === 'object' ? Object.fromEntries(Object.entries(v).map(([k, x]) => [k, wf(x)])) : v;
import { canon } from './kit.mjs';

const M = {};
const need = async (name, file) => (M[name] ??= await mod(file));

const surfView = (s) => ({ X: Array.from(s.X), Y: Array.from(s.Y), Z: Array.from(s.Z), tris: Array.from(s.tris), ox: s.ox, oy: s.oy, nT: s.nT, constrainedEdges: s.constrainedEdges });
const built = (r) => ({ surface: r.surface ? surfView(r.surface) : null, report: r.report });
const alView = (a) => ({ elements: a.elements, start: a.start, errors: a.errors, pis: a.pis.map((p) => ({ x: p.x, y: p.y, r: Number(p.r) || 0 })), length: a.length, end: a.end, ok: a.ok });
const profView = (p) => ({ pvis: p.pvis, grades: p.grades, curves: p.curves, errors: p.errors, start: p.start, end: p.end, ok: p.ok });

// more op tables (ops-*.mjs: io formats, GIS, coordinate systems …): { 'op.name': async (a, self, reg, call, keep) => result }
export const EXTOPS = {};

export async function runOp(op, a, call, reg) {
  if (call.self != null && !reg.has(call.self)) throw new Error('no object ' + call.self);
  const self = call.self != null ? reg.get(call.self) : undefined;
  const keep = (v) => { if (call.as != null) reg.set(call.as, v); };
  const U = await need('U', 'units.js'), PTS = await need('PTS', 'points.js'), TIN = await need('TIN', 'tin.js');
  const AL = await need('AL', 'alignment.js'), PR = await need('PR', 'profile.js');
  switch (op) {
    case 'units.formatStation': return U.formatStation(a[0], a[1], a[2] ?? undefined);
    case 'units.parseStation': return U.parseStation(a[0], a[1]);
    case 'units.isNum': return U.isNum(a[0]);
    case 'units.fixed': return U.fixed(a[0], a[1] ?? undefined);
    case 'units.grouped': return U.grouped(a[0], a[1] ?? undefined);
    case 'units.pct': return U.pct(a[0], a[1] ?? undefined);
    case 'units.volumeText': return U.volumeText(a[0], a[1], a[2] ?? undefined);
    case 'units.areaText': return U.areaText(a[0], a[1], a[2] ?? undefined);
    case 'units.bearing': return U.bearing(a[0]);
    case 'units.dms': return U.dms(a[0]);

    case 'points.parsePoints': return PTS.parsePoints(a[0], a[1] ?? undefined);
    case 'points.groupKey': return PTS.groupKey(a[0]);
    case 'points.pointGroups': return PTS.pointGroups(a[0]);
    case 'points.groupPoints': return PTS.groupPoints(a[0], a[1]);
    case 'points.writePoints': return PTS.writePoints(a[0], a[1] ?? undefined);
    case 'points.lassoPoints': return PTS.lassoPoints(a[0], a[1], a[2], a[3]).map((p) => p.n);

    case 'tin.buildSurface': { const r = TIN.buildSurface(a[0]); if (r.surface) keep(r.surface); return built(r); }
    case 'tin.surfaceFromFaces': { const r = TIN.surfaceFromFaces(a[0]); if (r.surface) keep(r.surface); return built(r); }
    case 'tin.convexHull': return TIN.convexHull(a[0]);
    case 'tin.polyArea': return TIN.polyArea(a[0]);
    case 'tin.ccw': return TIN.ccw(a[0]);
    case 'tin.pointInPoly': return TIN.pointInPoly(a[0], a[1], a[2]);
    case 'tin.clipConvex': return TIN.clipConvex(a[0], a[1]);

    case 'surface.view': return surfView(self);
    case 'surface.stats': return self.stats();
    case 'surface.elevationAt': return self.elevationAt(a[0], a[1], a[2] ?? undefined);
    case 'surface.contours': return self.contours(a[0], a[1] ?? undefined);
    case 'surface.bands': return self.bands(a[0]);
    case 'surface.slopes': return self.slopes();
    case 'surface.edges': return self.edges();
    case 'surface.outline': return self.outline();
    case 'surface.toFaces': return self.toFaces();
    case 'surface.hull': return self.hull();
    case 'surface.triangles': return [...self.triangles()];

    case 'alignment.normAng': return AL.normAng(a[0]);
    case 'alignment.build': { const al = AL.buildAlignment(a[0]); keep(al); return alView(al); }
    case 'al.pointAt': return self.pointAt(a[0]);
    case 'al.stationOffset': return self.stationOffset(a[0], a[1]);
    case 'al.polyline': return self.polyline(a[0] ?? undefined, a[1] ?? undefined);
    case 'al.ticks': return self.ticks(a[0]);
    case 'al.keyStations': return self.keyStations();

    case 'profile.sampleSurface': return PR.sampleSurface(a[0] == null ? null : reg.get(a[0]), reg.get(a[1]), a[2] ?? undefined);
    case 'profile.build': { const p = PR.buildProfile(a[0]); keep(p); return profView(p); }
    case 'prof.elevationAt': return self.elevationAt(a[0]);
    case 'prof.gradeAt': return self.gradeAt(a[0]);
    case 'prof.sample': return self.sample(a[0] ?? undefined);
  }
  if (Object.prototype.hasOwnProperty.call(EXTOPS, op)) return EXTOPS[op](a, self, reg, call, keep);
  throw new Error('unknown op ' + op);
}

export async function run(script) {
  const reg = new Map(), out = [];
  for (const call of script.calls || []) {
    // canonical at once: later calls may change the objects a result refers to (a project's live state)
    try { out.push({ ok: wf(canon(await runOp(call.op, dec(call.args || []), call, reg))) }); }
    catch (e) { out.push({ err: String(e && e.message || e) }); }
  }
  return out;
}
