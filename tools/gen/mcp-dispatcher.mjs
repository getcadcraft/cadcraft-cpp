#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Writes tests/golden/js-0.5-mcp-dispatcher.json: CadCraft 0.5's tool dispatcher (host.js "mcp-core", the same block
// as tools/mcp/core.mjs) answering a script of JSON-RPC messages with a few test tools, on a fixed clock: every answer
// and every activity-log entry. libs/mcp's test registers the same tools and replays the script.
//   node tools/gen/mcp-dispatcher.mjs
import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

const root = process.env.FORK_PARITY_JS || join(process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles', 'fork-parity/js-0.5');
const { createDispatcher, toolResult, checkSchema } = await import(pathToFileURL(join(root, 'site/public_html/assets/js/host.js')).href);

// The test tools (libs/mcp/tests/dispatcher_test.cpp registers the same ones).
const strict = {
  type: 'object',
  properties: {
    doc: { type: 'string', pattern: '^d[0-9]{1,6}$' },
    colour: { type: 'string', pattern: '^#[0-9a-fA-F]{6}$' },
    count: { type: 'integer', minimum: 1, maximum: 10 },
    ratio: { type: 'number', minimum: 0, maximum: 1 },
    name: { type: 'string', minLength: 2, maxLength: 5 },
    kind: { enum: ['a', 'b', 3] },
    fixed: { const: true },
    pts: { type: 'array', items: { type: 'number' }, minItems: 2, maxItems: 3 },
    mode: { type: 'string', default: 'fast' },
    shape: { anyOf: [{ type: 'string', maxLength: 3 }, { type: 'object', properties: { w: { type: 'number', default: 1 } }, required: ['h'], additionalProperties: false }] },
    any: { type: ['string', 'null'] },
  },
  required: ['doc'],
  additionalProperties: false,
};
const loose = { type: 'object', properties: { a: { type: 'integer' } }, additionalProperties: { type: 'string' } };
const tools = [
  { name: 'echo', group: 'read', title: 'Echo', description: 'Returns its arguments.', inputSchema: strict, run: (a) => a },
  { name: 'plain', group: 'read', description: 'No schema; returns a number.', run: () => 42 },
  { name: 'nothing', group: 'read', run: () => undefined },
  { name: 'loose', group: 'read', inputSchema: loose, run: (a) => ({ got: a, undo: 'Assistant: loose' }) },
  { name: 'fails', group: 'edit', run: () => { throw new Error('the drawing is locked'); } },
  { name: 'refuses', group: 'edit', run: () => { const e = new Error('not now'); e.refused = true; throw e; } },
  { name: 'soft_error', group: 'read', run: () => ({ error: 'no such layer', isError: true }) },
  { name: 'picture', group: 'export', run: () => toolResult([{ type: 'image', data: 'iVBORw0K', mimeType: 'image/png' }], { width: 2, undo: 'x' }) },
  { name: 'long_args', group: 'read', run: () => ({ ok: true }) },
];

const T0 = 1792108800000;
let clock = T0;
const log = [];
const d = createDispatcher({
  server: { name: 'cadcraft', title: 'CadCraft', version: '0.5.0' },
  now: () => clock,
  allowed: () => ['read', 'edit'],
  howToAllow: (g) => 'Restart with --allow read,edit,' + g + '.',
  instructions: () => 'Test instructions.',
  info: () => ({ app: 'cadcraft', version: '0.5.0', extra: 1 }),
  log: (e) => log.push(JSON.parse(JSON.stringify(e))),
});
for (const t of tools) d.tool(t);
d.method('custom.echo', (p) => ({ p }));

const long = 'x'.repeat(250);
const many = Object.fromEntries(Array.from({ length: 25 }, (_, i) => ['k' + i, i]));
const script = [
  { jsonrpc: '2.0', id: 1, method: 'initialize', params: { protocolVersion: '2025-06-18', clientInfo: { name: 'Test client', version: '1.2' } } },
  { jsonrpc: '2.0', method: 'notifications/initialized' },
  { jsonrpc: '2.0', id: 2, method: 'tools/list' },
  { jsonrpc: '2.0', id: 3, method: 'ping' },
  { jsonrpc: '2.0', id: 4, method: 'tools/call', params: { name: 'echo', arguments: { doc: 'd12', count: 3, pts: [1, 2], shape: 'abc', any: null } } },
  { jsonrpc: '2.0', id: 5, method: 'tools/call', params: { name: 'echo', arguments: { doc: 'x1', colour: '#12345g', count: 0, ratio: 2, name: 'a', kind: 'c', fixed: false, pts: [1, 'b', 3, 4], extra: 1, any: 3 } } },
  { jsonrpc: '2.0', id: 6, method: 'tools/call', params: { name: 'echo', arguments: { doc: 'd1', shape: { w: 2 } } } },
  { jsonrpc: '2.0', id: 7, method: 'tools/call', params: { name: 'echo', arguments: { doc: 'd1', shape: { h: 1 } } } },
  { jsonrpc: '2.0', id: 8, method: 'tools/call', params: { name: 'echo', arguments: { doc: 'd1', count: 2.5, toString: 1, constructor: 'x' } } },
  { jsonrpc: '2.0', id: 9, method: 'tools/call', params: { name: 'echo', arguments: [] } },
  { jsonrpc: '2.0', id: 10, method: 'tools/call', params: { name: 'echo' } },
  { jsonrpc: '2.0', id: 11, method: 'tools/call', params: { name: 'plain', arguments: { a: 1 } } },
  { jsonrpc: '2.0', id: 12, method: 'tools/call', params: { name: 'nothing' } },
  { jsonrpc: '2.0', id: 13, method: 'tools/call', params: { name: 'loose', arguments: { a: 1, b: 'two', c: 3 } } },
  { jsonrpc: '2.0', id: 14, method: 'tools/call', params: { name: 'loose', arguments: { a: 'one', b: 'two' } } },
  { jsonrpc: '2.0', id: 15, method: 'tools/call', params: { name: 'fails' } },
  { jsonrpc: '2.0', id: 16, method: 'tools/call', params: { name: 'refuses' } },
  { jsonrpc: '2.0', id: 17, method: 'tools/call', params: { name: 'soft_error' } },
  { jsonrpc: '2.0', id: 18, method: 'tools/call', params: { name: 'picture' } },
  { jsonrpc: '2.0', id: 19, method: 'tools/call', params: { name: 'nope' } },
  { jsonrpc: '2.0', id: 20, method: 'tools/call', params: {} },
  { jsonrpc: '2.0', id: 21, method: 'tools/call', params: { name: 'long_args', arguments: { s: long, many, deep: { a: { b: { c: 1 } }, list: [[1, 2], [3]] } } } },
  { jsonrpc: '2.0', id: 22, method: 'nope/nothing' },
  { jsonrpc: '2.0', method: 'nope/note' },
  { jsonrpc: '2.0', id: 23, method: 'custom.echo', params: { q: 1 } },
  { jsonrpc: '2.0', id: 24, method: 'app.info' },
  { jsonrpc: '2.0', id: 25, method: 'logging/setLevel', params: { level: 'debug' } },
  { jsonrpc: '2.0', id: 'str', method: 'control.hello', params: { client: { name: 'script runner', version: 7 } } },
  { jsonrpc: '2.0', id: 26, method: 'tools.list' },
  { jsonrpc: '2.0', id: 27, method: 'tools.call', params: { name: 'plain' } },
  { jsonrpc: '2.0', id: 28, method: 'tools.call', params: { nm: 'plain' } },
  { jsonrpc: '1.0', id: 29, method: 'ping' },
  { id: 30 },
  [1, 2],
  'text',
  null,
  { jsonrpc: '2.0', id: 31, result: {} },
  { jsonrpc: '2.0', id: { x: 1 }, method: 3 },
  { jsonrpc: '2.0', id: 32, method: 'initialize', params: { protocolVersion: '1999-01-01', clientInfo: 'x' } },
  { jsonrpc: '2.0', id: 33, method: 'initialize', params: [] },
];
const answers = [];
for (const m of script) {
  clock += 7;
  const session = (answers.session ??= { id: 'test' });
  answers.push(await d.handle(m, session) ?? null);
}
// checkSchema on its own, for the cases a tool table does not reach
const schemaCases = [
  [{ type: 'number' }, 1.5], [{ type: 'integer' }, 1.5], [{ type: 'integer' }, -0], [{ type: 'boolean' }, 'true'],
  [{ type: 'object' }, [1]], [{ type: 'array' }, {}], [{ type: 'null' }, null], [{ type: ['integer', 'string'] }, true],
  [{ minimum: 3 }, 2], [{ maxLength: 2 }, 'åäö'], [{ maxLength: 2 }, '😀'], [{ minLength: 2 }, '😀'],
  [{ pattern: '^[A-Za-z0-9]{3,80}$' }, 'abc'], [{ pattern: '^[A-Za-z0-9]{3,80}$' }, 'ab'], [{ pattern: '^[A-Za-z0-9]{3,80}$' }, 'abcé'],
  [{ enum: [1, '1'] }, 1], [{ enum: [[1]] }, [1]], [{ const: 'x' }, 'x'], [{ const: { a: 1 } }, { a: 1 }],
  [{ oneOf: [{ type: 'string' }, { type: 'number' }] }, true], [{ properties: { a: { default: [1] } } }, {}],
  [{ required: ['a', 'b'] }, { b: 1 }], [{ items: { maximum: 1 } }, [0, 2, 3]],
];
const schema = schemaCases.map(([s, v]) => { const val = JSON.parse(JSON.stringify(v)); const problems = checkSchema(s, val); return { schema: s, value: v, problems, after: val }; });
const out = { about: 'CadCraft 0.5 host.js mcp-core on tools/gen/mcp-dispatcher.mjs (frozen js-0.5)', t0: T0, step: 7, script, answers, log, schema };
writeFileSync(new URL('../../tests/golden/js-0.5-mcp-dispatcher.json', import.meta.url), JSON.stringify(out, null, 1) + '\n');
console.log(answers.length + ' answers, ' + log.length + ' log entries, ' + schema.length + ' schema cases');
