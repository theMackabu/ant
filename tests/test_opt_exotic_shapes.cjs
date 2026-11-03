const assert = require('node:assert');
const vm = require('node:vm');

function protoRead(o) { return o.hasOwnProperty; }
function ownRead(o) { return o.x; }
function ownWrite(o, v) { o.x = v; return o; }
function addX(o, v) { o.x = v; return o; }
function readFoo(o) { return o.foo; }
function poly(o) { return o.y; }

for (let i = 0; i < 200000; i++) {
  protoRead({});
  ownRead({ x: i });
  ownWrite({ x: 0 }, i);
  addX(Object.create(null), i);
  readFoo(Object.create(null));
  poly(i & 1 ? { y: 1 } : { z: 2, y: 3 });
}

const log = [];
const handler = {
  get(target, key) { log.push('get:' + String(key)); return 42; },
  set(target, key, value) { log.push('set:' + String(key)); target[key] = value * 2; return true; },
};

assert.strictEqual(protoRead(new Proxy({}, handler)), 42);
assert.strictEqual(ownRead(new Proxy({ x: 1 }, handler)), 42);
assert.strictEqual(poly(new Proxy({ y: 1 }, handler)), 42);
assert.strictEqual(readFoo(new Proxy(Object.create(null), handler)), 42);

const written = { x: 0 };
ownWrite(new Proxy(written, handler), 5);
assert.strictEqual(written.x, 10);

const added = {};
addX(new Proxy(added, handler), 6);
assert.strictEqual(added.x, 12);

assert.deepStrictEqual(log, ['get:hasOwnProperty', 'get:x', 'get:y', 'get:foo', 'set:x', 'set:x']);

function sloppyArgs(a) { a = 7; return arguments; }
function strictArgs(a) { 'use strict'; a = 7; return arguments; }
function readArgs(args) { return args.length + args[0]; }
for (let i = 0; i < 200000; i++) {
  readArgs(sloppyArgs(1));
  readArgs(strictArgs(1));
}
assert.strictEqual(readArgs(sloppyArgs(1)), 8);
assert.strictEqual(readArgs(strictArgs(1)), 2);

const typed = new Uint8Array(4);
const lookalike = Object.create(Uint8Array.prototype);
function readZero(o) { return o[0]; }
for (let i = 0; i < 200000; i++) readZero(lookalike);
typed[0] = 9;
assert.strictEqual(readZero(typed), 9);

process.env.ANT_OPT_EXOTIC_PROBE = 'from-env';
function readEnv(o) { return o.ANT_OPT_EXOTIC_PROBE; }
for (let i = 0; i < 200000; i++) readEnv({ ANT_OPT_EXOTIC_PROBE: 1 });
assert.strictEqual(readEnv(process.env), 'from-env');

const ctx = vm.createContext({ base: 3 });
assert.strictEqual(vm.runInContext('let t = 0; for (let i = 0; i < 100000; i++) t += base; t', ctx), 300000);

console.log('ok');
