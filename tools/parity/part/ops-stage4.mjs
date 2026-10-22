// SPDX-License-Identifier: Apache-2.0
// pcdiff/ops-stage4.mjs — JS ops for cc-part stage 4 (M10.4): assembly.js (without interference / motion checks,
// which need manifold booleans), explode.js, kernel.js planeFrame / faceRefAt on kernel results passed as data,
// stdparts.js + shapes/parts.js, clipboard.js. Mirrored by crates/part/src/script4.rs.
import { DEV } from './driver.mjs';

export const A = await import(DEV + 'assembly.js');
export const X = await import(DEV + 'explode.js');
const K = await import(DEV + 'kernel.js');
const SP = await import(DEV + 'shapes/parts.js');
const STD = await import(DEV + 'stdparts.js');
const C = await import(DEV + 'clipboard.js');

const clone = (v) => (v === undefined ? undefined : structuredClone(v));
const map = (o) => new Map(Object.entries(o || {}));
/** A kernel result from its JSON form (see buildJson). */
export const toBuild = (b) => (b ? { results: map(b.results), candidates: map(b.candidates), axes: map(b.axes), holeAxes: map(b.holeAxes), refGeom: map(b.refGeom), exact: !!b.exact, planes: b.planes } : undefined);
/** The parts of a kernel regenerate() result assembly placement reads, as JSON. */
export const buildJson = (r) => ({ results: Object.fromEntries(r.results), candidates: Object.fromEntries(r.candidates), axes: Object.fromEntries(r.axes), holeAxes: Object.fromEntries(r.holeAxes), refGeom: Object.fromEntries(r.refGeom) });
const builds = (o) => new Map(Object.entries(o || {}).map(([k, b]) => [k, toBuild(b)]));

export const OPS = {
  'asm.new': (name) => A.newAssembly(name ?? undefined),
  'asm.insertPart': (asm, part, place, pid) => { asm = clone(asm); const res = A.insertPart(asm, part, { place: place ?? null, pid: pid ?? null }); return { res, asm }; },
  'asm.addJoint': (asm, type, a, b, more) => { asm = clone(asm); const res = A.addJoint(asm, type, a, b, more || {}); return { res, asm }; },
  'asm.removeInstance': (asm, id) => { asm = clone(asm); A.removeInstance(asm, id); return asm; },
  'asm.rigidInverse': (m) => A.rigidInverse(m),
  'asm.axesOf': (b, part) => A.axesOf(toBuild(b), part),
  'asm.mateFrame': (end, b, part) => A.mateFrame(end, toBuild(b), part),
  'asm.planeFrame': (ref, b, part) => K.planeFrame(ref, toBuild(b), part),
  'asm.faceRefAt': (b, fid, n, p) => K.faceRefAt(toBuild(b), fid, n, p),
  'asm.limitOf': (j, key) => A.limitOf(j, key ?? undefined),
  'asm.jointValue': (j, key) => A.jointValue(j, key ?? undefined),
  'asm.jointMotion': (j) => A.jointMotion(j),
  'asm.placeAll': (asm, b) => { const r = A.placeAll(asm, builds(b)); return { world: Object.fromEntries(r.world), problems: r.problems }; },
  'asm.jointTree': (asm, b, world) => A.jointTree(asm, builds(b), map(world)),
  'asm.freeIds': (asm) => [...A.freeIds(asm)],
  'asm.movePlace': (place, o) => A.movePlace(place, o || {}),
  'asm.moveInstance': (asm, id, place) => { asm = clone(asm); A.moveInstance(asm, id, place); return asm; },
  'asm.filterClashes': (asm, list) => A.filterClashes(asm, list),
  'asm.setKindHidden': (asm, k, h) => { asm = clone(asm); A.setKindHidden(asm, k, h); return asm; },
  'asm.setPairHidden': (asm, a, b, h) => { asm = clone(asm); A.setPairHidden(asm, a, b, h); return asm; },
  'asm.readClashFilter': (raw, insts) => A.readClashFilter(raw ?? undefined, new Set(insts)),
  'asm.read': (raw) => A.readAssembly(raw),
  'asm.readLimits': (L, type) => A.readLimits(L, type),
  'asm.fileVersion': (asm) => A.fileVersion(asm),
  'asm.toJSON': (asm) => A.toJSON(asm),
  'explode.newView': (asm, name) => { asm = clone(asm); const res = X.newView(asm, name ?? undefined); return { res, asm }; },
  'explode.addStep': (asm, vid, insts, dir, dist) => { asm = clone(asm); const view = (asm.views || []).find((v) => v.id === vid); const res = X.addStep(asm, view, insts, dir, dist); return { res, asm }; },
  'explode.offsets': (view, t) => Object.fromEntries(X.offsets(view ?? undefined, t ?? undefined)),
  'explode.exploded': (world, offs) => Object.fromEntries(X.exploded(map(world), map(offs))),
  'explode.trails': (view, centres, t) => X.trails(view ?? undefined, map(centres), t ?? undefined),
  'explode.autoSteps': (items, mode, o) => X.autoSteps(items, mode, o || {}),
  'parts.families': () => SP.PART_FAMILIES,
  'parts.sizes': (id) => SP.partSizes(id),
  'parts.options': (id, size) => SP.partOptions(id, size),
  'parts.make': (id, size, o) => SP.makePart(id, size, o ?? undefined),
  'std.templates': () => STD.TEMPLATES,
  'std.sizes': (id) => STD.templateSizes(id),
  'std.make': (id, size) => STD.makeTemplate(id, size ?? null),
  'clip.copySketch': (sk, ids) => C.copySketch(sk, ids),
  'clip.pasteSketch': (sk, clip, d) => { sk = clone(sk); const res = C.pasteSketch(sk, clip, d ?? undefined); return { res, sk }; },
  'clip.copyFeature': (part, id) => C.copyFeature(part, id),
  'clip.pasteFeatures': (part, clip) => { part = clone(part); const res = C.pasteFeatures(part, clip); return { res, part }; },
  'clip.copyInstance': (asm, id) => C.copyInstance(asm, id),
  'clip.pasteInstance': (asm, clip, place) => { asm = clone(asm); const res = C.pasteInstance(asm, clip, place); return { res, asm }; },
  'clip.read': (raw) => C.readClip(raw),
  'clip.describe': (c) => C.describeClip(c),
};
