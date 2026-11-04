const assert = require('node:assert');
const { Writable } = require('node:stream');

function invoke(cb, value) {
  return cb(value);
}

function invokeThis(target, cb, value) {
  return cb.call(target, value);
}

const resolvers = [];
for (let i = 0; i < 64; i++) new Promise((resolve) => resolvers.push(resolve));
for (let round = 0; round < 2000; round++)
  for (const resolve of resolvers) assert.strictEqual(invoke(resolve, round), undefined);

const revokes = [];
for (let i = 0; i < 32; i++) {
  const { proxy, revoke } = Proxy.revocable({}, {});
  revokes.push([proxy, revoke]);
}
for (let round = 0; round < 3000; round++) assert.strictEqual(invoke(revokes[round % revokes.length][1]), undefined);
for (const [proxy] of revokes) assert.throws(() => proxy.x, TypeError);

let thrown = 0;
let completed = 0;
function guarded(cb, value) {
  try {
    const result = cb(value);
    completed++;
    return result;
  } catch (err) {
    thrown++;
    return err;
  }
}
for (let i = 0; i < 5000; i++) {
  const result = guarded(require, i % 100 === 0 ? './__missing_native_closure_target__' : 'node:assert');
  if (i % 100 === 0) assert.ok(result instanceof Error);
  else assert.strictEqual(result, assert);
}
assert.strictEqual(thrown, 50);
assert.strictEqual(completed, 4950);

const mixed = [(x) => x + 1, resolvers[0], Math.abs, revokes[0][1], function () { 'use strict'; return this; }];
for (let i = 0; i < 20000; i++) {
  const fn = mixed[i % mixed.length];
  const result = invoke(fn, -i);
  if (fn === mixed[0]) assert.strictEqual(result, 1 - i);
  else if (fn === mixed[2]) assert.strictEqual(result, i);
  else if (fn === mixed[4]) assert.strictEqual(result, undefined);
  else assert.strictEqual(result, undefined);
}
assert.strictEqual(invokeThis(null, mixed[4]), null);

let bytes = 0;
const streams = Array.from({ length: 8 }, () => new Writable({ write(chunk, enc, cb) { bytes += chunk.length; cb(); } }));
const chunk = Buffer.alloc(16, 1);
function pump(n) {
  for (let i = 0; i < n; i++) for (const ws of streams) ws.write(chunk);
}
pump(20000);
assert.strictEqual(bytes, 20000 * 8 * 16);

const failures = [];
const failing = new Writable({ write(chunk, enc, cb) { cb(new Error('boom ' + chunk.length)); } });
failing.on('error', (err) => failures.push(err.message));
failing.write(Buffer.alloc(3));

setImmediate(() => {
  assert.deepStrictEqual(failures, ['boom 3']);
  console.log('ok');
});
