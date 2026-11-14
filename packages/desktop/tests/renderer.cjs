'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const read = name => fs.readFileSync(path.join(__dirname, '..', 'js', name), 'utf8');
const source = `(bindings => {\n${read('clone.js')}\n${read('renderer.js')}\nreturn createRendererBridge(bindings);\n})`;

function load(bindings) {
  const context = vm.createContext({ atob, btoa, DOMException });
  const sent = [];
  const bridge = vm.runInContext(source, context)({
    versions: { ant: '1', desktop: '2', chrome: '3' },
    capabilities: 'send:note;invoke:echo;receive:event',
    sandbox: true,
    integration: false,
    send: (...args) => sent.push(args),
    ...bindings
  });
  return { bridge, context, sent };
}

const { bridge, context, sent } = load({});
assert.ok(Object.isFrozen(context.Ant.versions));
assert.equal(context.Ant.versions.chrome, '3');
assert.equal(typeof context.require, 'undefined');
context.Ant.ipc.send('note', { value: 42 });
assert.deepEqual(sent[0].slice(0, 3), [0, 0, 'note']);
assert.throws(() => context.Ant.ipc.send('other', 1), /not granted/);
assert.throws(() => bridge.require('node:fs'), /requires webPreferences\.sandbox: false/);

const api = bridge.require('ant:desktop/renderer');
assert.equal(api.ipcRenderer, context.Ant.ipc);
api.contextBridge.exposeInMainWorld('exampleApi', { ready: true });
assert.equal(context.exampleApi.ready, true);

const received = [];
const stop = context.Ant.ipc.on('event', value => received.push(value));
bridge.receive(2, 0, 'event', sent[0][3]);
bridge.receive(2, 0, 'hidden', sent[0][3]);
stop();
bridge.receive(2, 0, 'event', sent[0][3]);
assert.equal(received.length, 1);
assert.equal(received[0].value, 42);

const pending = context.Ant.ipc.invoke('echo', 'question');
bridge.receive(0, sent[1][1], '', sent[0][3]);

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

pending.then(value => {
  assert.equal(value.value, 42);
  console.log('desktop-renderer-bridge-ok');
});
