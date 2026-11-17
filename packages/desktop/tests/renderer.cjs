'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const v8 = require('node:v8');
const vm = require('node:vm');

const read = name => fs.readFileSync(path.join(__dirname, '..', 'src', 'builtins', 'page', name), 'utf8');
const source = `(bindings => {\n${read('wire.js')}\n${read('bridge.js')}\nreturn createRendererBridge(bindings);\n})`;

function load(bindings) {
  const context = vm.createContext({ DOMException, TextDecoder });
  const queries = [];
  const bridge = vm.runInContext(source, context)({
    versions: { ant: '1', desktop: '2', chrome: '3' },
    capabilities: 'send:note;invoke:echo;receive:event',
    sandbox: true,
    integration: false,
    query: options => queries.push(options),
    ...bindings
  });
  return { bridge, context, queries };
}

const message = query => v8.deserialize(Buffer.from(query.request));
const answer = (query, value) => query.onSuccess(new Uint8Array(v8.serialize(value)).buffer);

const { bridge, context, queries } = load({});
const [events] = queries;
assert.equal(events.persistent, true);
assert.deepEqual(message(events), ['receive']);
assert.ok(Object.isFrozen(context.Ant.versions));
assert.equal(context.Ant.versions.chrome, '3');
assert.equal(typeof context.require, 'undefined');

context.Ant.ipc.send('note', { value: 42, bytes: new Uint8Array([7]) });
assert.deepEqual(message(queries[1]), ['send', 'note', { value: 42, bytes: new Uint8Array([7]) }]);
assert.throws(() => context.Ant.ipc.send('other', 1), /not granted/);
assert.throws(() => context.Ant.ipc.send('note', () => {}), /could not be cloned/);
assert.throws(() => bridge.require('node:fs'), /requires webPreferences\.sandbox: false/);

const api = bridge.require('ant:desktop/renderer');
assert.equal(api.ipcRenderer, context.Ant.ipc);
api.contextBridge.exposeInMainWorld('exampleApi', { ready: true });
assert.equal(context.exampleApi.ready, true);

const received = [];
const stop = context.Ant.ipc.on('event', value => received.push(value));
answer(events, ['event', { value: 42 }]);
answer(events, ['hidden', { value: 1 }]);
stop();
answer(events, ['event', { value: 43 }]);
assert.equal(received.length, 1);
assert.equal(received[0].value, 42);

const pending = context.Ant.ipc.invoke('echo', 'question');
assert.deepEqual(message(queries[2]), ['invoke', 'echo', 'question']);
answer(queries[2], [true, { value: 42 }]);
const rejected = context.Ant.ipc.invoke('echo', 'bad');
answer(queries[3], [false, new RangeError('no')]);

const modules = {
  'node:process': [
    { name: 'platform', callable: false, value: 'darwin' },
    { name: 'arch', callable: false, value: 'arm64' },
    { name: 'cwd', callable: true }
  ]
};
const integrated = load({
  sandbox: false,
  integration: true,
  describe: specifier =>
    JSON.stringify(modules[specifier] ? { ok: true, value: modules[specifier] } : { ok: false, error: 'missing' }),
  call: (specifier, name, args) => JSON.stringify({ ok: true, value: `${name}:${args}` })
});
assert.equal(integrated.context.process.platform, 'darwin');
assert.equal(integrated.context.process.cwd(1), 'cwd:[1]');
assert.throws(() => integrated.context.require('node:nope'), /missing/);

Promise.all([pending, rejected.catch(error => error)]).then(([value, error]) => {
  assert.equal(value.value, 42);
  assert.equal(error.name, 'RangeError');
  assert.equal(error.message, 'no');
  console.log('desktop-renderer-bridge-ok');
});
