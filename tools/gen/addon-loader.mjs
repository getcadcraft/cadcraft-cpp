#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Writes tests/golden/js-0.5-addon-loader.json: CadCraft 0.5's add-on loader (frozen js-0.5 assets/js/addons.js) on
// checkManifest / apiParts / cleanAddonData cases, and a lifecycle run of bundled add-ons whose modules follow small
// scripts (the same scripts libs/addons' test gives its built-in modules): list(), what the modules did, and the
// storage after every step. Installed packages are left out: the C++ refuses every package with code (docs/gaps.md).
//   node tools/gen/addon-loader.mjs
import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

const root = process.env.FORK_PARITY_JS || join(process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles', 'fork-parity/js-0.5');
const A = await import(pathToFileURL(join(root, 'site/public_html/assets/js/addons.js')).href);

const good = { id: 'electrical', name: 'Electrical', version: '1.2.3', apps: ['cadcraft'], api: '1.0', license: 'Apache-2.0', entry: './main.js' };
const w = (o) => ({ ...good, ...o });
const manifests = [
  ['good', good],
  ['every optional key', w({ permissions: ['commands', 'objects', 'commands'], description: 'Wires and panels', author: 'CadCraft', homepage: 'https://getcadcraft.com/x', default: 'off', lang: './lang/', help: './help/help.js' })],
  ['not an object', null], ['an array', [good]], ['a string', 'electrical'],
  ['id missing', w({ id: undefined })], ['id with capitals', w({ id: 'Electrical' })], ['id one letter', w({ id: 'e' })],
  ['id ..', w({ id: 'a..b' })], ['id --', w({ id: 'a--b' })], ['id ends with .', w({ id: 'ab.' })], ['id ends with -', w({ id: 'ab-' })],
  ['id 64', w({ id: 'a' + 'b'.repeat(63) })], ['id 65', w({ id: 'a' + 'b'.repeat(64) })], ['id dotted', w({ id: 'com.example.pipes' })],
  ['name blank', w({ name: '   ' })], ['name padded', w({ name: '  Pipes  ' })], ['name 60', w({ name: 'é'.repeat(60) })], ['name 61', w({ name: 'n'.repeat(61) })],
  ['name with <', w({ name: 'a<b' })], ['name with tab', w({ name: 'a\tb' })], ['name astral', w({ name: '😀'.repeat(30) })], ['name astral 31', w({ name: '😀'.repeat(31) })],
  ['version pre', w({ version: '1.0.0-pre.1' })], ['version two parts', w({ version: '1.0' })], ['version number', w({ version: 1 })], ['version long', w({ version: '12345.0.0' })],
  ['apps empty', w({ apps: [] })], ['apps not array', w({ apps: 'cadcraft' })], ['apps bad slug', w({ apps: ['CadCraft'] })], ['apps other', w({ apps: ['bimcraft', 'partcraft'] })],
  ['api 1', w({ api: '1' })], ['api number 1', w({ api: 1 })], ['api 1.5', w({ api: '1.5' })], ['api 2', w({ api: '2' })], ['api 0.9', w({ api: '0.9' })], ['api 1.0.7', w({ api: '1.0.7' })],
  ['api array', w({ api: [1] })], ['api bad', w({ api: 'one' })], ['api missing', w({ api: undefined })], ['api number 1.25', w({ api: 1.25 })],
  ['license or', w({ license: 'MIT OR Apache-2.0' })], ['license with', w({ license: 'GPL-2.0 WITH Classpath-exception-2.0' })], ['license trailing', w({ license: 'MIT AND' })],
  ['license double space', w({ license: 'MIT  AND BSD-3-Clause' })], ['license short', w({ license: 'M' })],
  ['entry plain', w({ entry: 'main.mjs' })], ['entry lib', w({ entry: './lib/a.js' })], ['entry ..', w({ entry: '../main.js' })], ['entry //', w({ entry: './a//b.js' })],
  ['entry url', w({ entry: 'https://x/main.js' })], ['entry ts', w({ entry: './main.ts' })], ['entry lua', w({ entry: './main.lua' })],
  ['permissions bad', w({ permissions: ['Network'] })], ['permissions 21', w({ permissions: Array.from({ length: 21 }, (_, i) => 'p' + i) })], ['permissions not array', w({ permissions: 'files' })],
  ['description 400', w({ description: 'd'.repeat(400) })], ['description 401', w({ description: 'd'.repeat(401) })], ['description newline', w({ description: 'a\nb' })],
  ['description control', w({ description: 'a\u0001b' })], ['author 201', w({ author: 'a'.repeat(201) })], ['author number', w({ author: 5 })],
  ['homepage http', w({ homepage: 'http://x.com' })], ['homepage space', w({ homepage: 'https://x.com/a b' })], ['homepage nbsp', w({ homepage: 'https://x.com/a b' })], ['homepage quote', w({ homepage: 'https://x.com/"' })],
  ['default on', w({ default: 'on' })], ['default bad', w({ default: true })],
  ['lang abs', w({ lang: '/lang/' })], ['lang url', w({ lang: 'HTTPS:x' })], ['lang ..', w({ lang: './../lang' })], ['help number', w({ help: 3 })],
  ['extra keys kept out', w({ extra: 1, scripts: ['a'] })],
];
const apps = [['cadcraft', '1.0'], ['cadcraft', '1.4'], ['cadcraft', '2.0'], ['markupcraft', '1.0']];
const checks = [];
for (const [name, raw] of manifests) for (const [app, apiVersion] of apps) checks.push({ name, raw: raw === undefined ? null : raw, app, apiVersion, out: A.checkManifest(raw, { app, apiVersion }) });

const apiCases = ['1', '1.0', '1.2.3', '1.2.3.4', '', '01.02', '1000', '1.', '.1', 'x', null, 3, 3.5, true];
const api = apiCases.map((v) => ({ v, out: A.apiParts(v) }));

const xd = [
  null, [], 'x', {}, { electrical: { kind: '', v: 1, data: { a: 1 } } },
  { electrical: { kind: 'wire', v: 2, data: [1, 'two', null] }, 'Bad Id': { kind: '', v: 1, data: 1 } },
  { electrical: { kind: 'Wire', v: 1, data: 1 } }, { electrical: { kind: '', v: 0, data: 1 } }, { electrical: { kind: '', v: 10000, data: 1 } },
  { electrical: { kind: '', v: 1.5, data: 1 } }, { electrical: { kind: '', v: 1 } }, { electrical: { v: 1, data: 1 } },
  { electrical: { kind: '', v: 1, data: 'x'.repeat(11998) } }, { electrical: { kind: '', v: 1, data: 'x'.repeat(11999) } },
  { electrical: { kind: '', v: 1, data: 'é'.repeat(11998) } }, { electrical: { kind: '', v: 1, data: '"'.repeat(5999) } },
  { 'a..b': { kind: '', v: 1, data: 1 }, 'ab-': { kind: '', v: 1, data: 1 }, ok: [] },
  Object.fromEntries(Array.from({ length: 25 }, (_, i) => ['a' + i, { kind: '', v: 1, data: i }])),
  Object.fromEntries([...Array.from({ length: 20 }, (_, i) => [String(i), { kind: '', v: 1, data: i }]), ['late', { kind: '', v: 1, data: 1 }]]),
  { electrical: { kind: '', v: 1, data: { b: 1, a: 2, 1: 3 } } },
];
const clean = xd.map((v) => ({ v, out: A.cleanAddonData(v) ?? '$undefined' }));

// ---- the lifecycle run
const base = 'https://x.test/addons/';
const bundled = {
  alpha: { manifest: w({ id: 'alpha', name: 'Alpha', lang: './lang/', permissions: ['commands'] }),
    lang: { en: { hello: 'Hello {who}', items: { one: '{n} item', other: '{n} items' }, '@note': 'x', only_en: 'English', bad: 3 },
      es: { hello: 'Hola {who}', items: { one: '{n} elemento', other: '{n} elementos', '=0': 'ninguno' } }, 'es-MX': { hello: '¡Hola {who}! {{x}} {missing}' } },
    script: { activate: [['t', 'hello', { who: 'Ana' }], ['t', 'items', { n: 0 }], ['t', 'items', { n: 1 }], ['t', 'items', { n: 2 }], ['t', 'items', { n: '1' }], ['t', 'only_en'], ['t', 'bad'], ['t', 'nope'], ['t', 'hello', { who: null }], ['t', 'hello', { who: 2.5 }],
      ['set', 'k', { a: [1, 2] }], ['get', 'k'], ['get', 'none'], ['set', 'u', '$undefined'], ['get', 'u'], ['cleanup', 'alpha-1'], ['cleanup', 'alpha-2']], deactivate: true, returns: 'alpha-ret' } },
  beta: { manifest: w({ id: 'beta', name: 'Beta' }), script: { activate: [['get', 'k']] } },
  gamma: { manifest: w({ id: 'gamma', name: 'Gamma' }), script: { activate: [['cleanup', 'gamma-1']] } },
  failing: { manifest: w({ id: 'failing', name: 'Failing' }), script: { activate: [['cleanup', 'failing-1'], ['throw', 'it broke']] } },
  noact: { manifest: w({ id: 'noact', name: 'No activate' }), script: null },
  defoff: { manifest: w({ id: 'defoff', name: 'Default off', default: 'off' }), script: { activate: [['cleanup', 'defoff-1']] } },
  otherapp: { manifest: w({ id: 'otherapp', name: 'Other', apps: ['bimcraft'] }), script: { activate: [] } },
  newer: { manifest: w({ id: 'newer', name: 'Newer', api: '1.1' }), script: { activate: [] } },
  broken: { manifest: w({ id: 'Broken' }), script: { activate: [] } },
  missing: { manifest: null, script: null },
  dupe: { manifest: w({ id: 'alpha', name: 'Alpha again' }), script: { activate: [['t', 'x']] } },
  badcleanup: { manifest: w({ id: 'badcleanup', name: 'Bad cleanup' }), script: { activate: [['cleanup', '$throw']], deactivate: true } },
};
const index = ['alpha', 'beta', 'gamma', 'failing', 'noact', 'defoff', 'otherapp', 'newer', 'broken', 'missing', 'dupe', 'badcleanup', '../evil', ''];

function storage(init = {}) {
  const m = new Map(Object.entries(init));
  return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => { m.set(k, String(v)); }, removeItem: (k) => { m.delete(k); }, dump: () => Object.fromEntries([...m.entries()].sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0))) };
}
let trace = [];
function moduleFor(dir) {
  const s = bundled[dir].script;
  if (!s) return {};
  const mod = {
    activate(api) {
      let ret;
      for (const op of s.activate) {
        if (op[0] === 't') trace.push(dir + ' t ' + op[1] + ' = ' + api.t(op[1], ...(op[2] ? [op[2]] : [])));
        else if (op[0] === 'set') trace.push(dir + ' set ' + op[1] + ' = ' + api.storage.set(op[1], op[2] === '$undefined' ? undefined : op[2]));
        else if (op[0] === 'get') trace.push(dir + ' get ' + op[1] + ' = ' + JSON.stringify(api.storage.get(op[1])));
        else if (op[0] === 'cleanup') api.onDeactivate(() => { if (op[1] === '$throw') throw new Error('x'); trace.push('cleanup ' + op[1]); });
        else if (op[0] === 'throw') throw new Error(op[1]);
      }
      trace.push(dir + ' active ' + api.version + ' ' + api.app.slug + ' ' + api.addon.id + ' ' + api.addon.version);
      if (s.returns) ret = () => trace.push('cleanup ' + s.returns);
      return ret;
    },
  };
  if (s.deactivate) mod.deactivate = (api) => { trace.push('deactivate ' + api.addon.id); };
  return mod;
}
function loader(st) {
  return new A.AddonLoader({
    app: 'cadcraft', apiVersion: '1.0', appName: 'CadCraft', base, storage: st, locale: 'es-MX', packages: A.packageStore(null),
    fetchJson: async (u) => {
      if (u === base + 'index.json') return { addons: index };
      const rel = u.slice(base.length);
      const [dir, ...rest] = rel.split('/');
      const b = bundled[dir];
      if (!b) return null;
      if (rest.join('/') === 'addon.json') return b.manifest ? structuredClone(b.manifest) : null;
      const m = /^lang\/(.+)\.json$/.exec(rest.join('/'));
      return m && b.lang && b.lang[m[1]] ? structuredClone(b.lang[m[1]]) : null;
    },
    importer: async (u) => { const dir = u.slice(base.length).split('/')[0]; return moduleFor(dir); },
  });
}
const steps = [];
const snap = (name, L, st) => { steps.push({ step: name, list: L.list().map(({ manifest, ...r }) => ({ ...r, manifest })), trace, storage: st.dump() }); trace = []; };
const st = storage({ 'cc-addons:cadcraft': JSON.stringify({ x: 1, off: { gamma: 'user', beta: 'failed' }, on: {} }), 'cc-addon:cadcraft:beta:k': '"from before"' });
const L = loader(st);
await L.start(); snap('start', L, st);
await L.setEnabled('gamma', true); snap('gamma on', L, st);
await L.setEnabled('beta', true); snap('beta on', L, st);
await L.setEnabled('alpha', false); snap('alpha off', L, st);
await L.setEnabled('beta', false); snap('beta off (nothing to undo: reload)', L, st);
await L.setEnabled('badcleanup', false); snap('badcleanup off (a cleanup throws)', L, st);
await L.setEnabled('failing', true); snap('failing on again', L, st);
await L.setEnabled('defoff', true); snap('defoff on', L, st);
await L.setEnabled('newer', true); snap('newer on (refused)', L, st);
await L.setEnabled('nobody', true); snap('unknown id', L, st);
await L.setEnabled('alpha', true); snap('alpha on again', L, st);
const L2 = loader(st);
await L2.start(); snap('a second start on the same storage', L2, st);
const st3 = storage({ 'cc-addons:cadcraft': '[1,2]' });
const L3 = loader(st3); await L3.start(); await L3.setEnabled('beta', false); snap('storage holding an array', L3, st3);
const st4 = storage({ 'cc-addons:cadcraft': '{"off": 5, "on": [1], "z": {"1": 2}, "2": 0}' });
const L4 = loader(st4); await L4.start(); await L4.setEnabled('alpha', false); snap('storage holding odd shapes', L4, st4);

const out = { source: 'CadCraft 0.5 assets/js/addons.js (frozen js-0.5), tools/gen/addon-loader.mjs', checks, api, clean, lifecycle: { index, bundled, steps } };
const dest = process.argv[2] || 'tests/golden/js-0.5-addon-loader.json';
writeFileSync(dest, JSON.stringify(out, null, 1) + '\n');
console.log(dest, checks.length, 'checks,', api.length, 'api,', clean.length, 'clean,', steps.length, 'steps');
