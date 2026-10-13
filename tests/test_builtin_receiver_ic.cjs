const assert = require('node:assert');

const callOf = f => f.call;
const applyOf = f => f.apply;
const nameOf = f => f.name;
const protoOf = f => f.prototype;
const fooOf = f => f.foo;

const fns = [Math.max, Object.prototype.toString, JSON.stringify, parseInt, Map, Promise.resolve];
for (let i = 0; i < 5000; i++) for (const f of fns) { callOf(f); applyOf(f); nameOf(f); protoOf(f); fooOf(f); }
assert.deepStrictEqual(
  fns.map(f => [callOf(f) === Function.prototype.call, applyOf(f) === Function.prototype.apply, nameOf(f), typeof protoOf(f), fooOf(f)]),
  fns.map(f => [true, true, f.name, f === Map ? 'object' : 'undefined', undefined]),
  'reads through builtin functions agree with Function.prototype'
);

Math.min.foo = 'own';
assert.deepStrictEqual([fooOf(Math.min), fooOf(Math.max)], ['own', undefined], 'a builtin that gained own properties is not served from the cache');
for (let i = 0; i < 5000; i++) fooOf(Math.abs);
Math.abs.foo = 'later';
assert.strictEqual(fooOf(Math.abs), 'later', 'a warm site sees a builtin that gains a property later');

const savedCall = Function.prototype.call;
for (let i = 0; i < 5000; i++) callOf(Math.floor);
Function.prototype.call = function patched() {};
assert.strictEqual(callOf(Math.floor).name, 'patched', 'a replaced Function.prototype method is seen by a warm site');
Function.prototype.call = savedCall;

const ts = Object.prototype.toString;
let acc = 0;
for (let i = 0; i < 20000; i++) acc += ts.call(i & 1 ? [] : new Uint8Array(1)).length;
assert.strictEqual(acc, 10000 * '[object Array]'.length + 10000 * '[object Uint8Array]'.length);

console.log('builtin receiver ic OK');
