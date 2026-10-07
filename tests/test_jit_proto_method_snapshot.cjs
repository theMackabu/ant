// Compiled reads of a method on the receiver's prototype check the receiver
// shape, its prototype and that prototype's shape against what the inline cache
// saw at compile time, then load the slot directly. Anything that changes the
// answer must miss and take the generic path.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

function same(actual, expected, what) {
  if (!Object.is(actual, expected)) throw new Error(`${what}: ${String(actual)} !== ${String(expected)}`);
}

function Thing(v) { this.v = v; }
Thing.prototype.get = function () { return this.v; };
function call(o) { return o.get(); }
function read(o) { return o.get; }

const warm = new Thing(1);
for (let i = 0; i < 5000; i++) { call(warm); read(warm); }

same(call(new Thing(7)), 7, 'plain hit');

// replaced method value: same shape, new function
const original = Thing.prototype.get;
Thing.prototype.get = function () { return this.v * 10; };
same(call(new Thing(2)), 20, 'replaced method');
Thing.prototype.get = original;
same(call(new Thing(3)), 3, 'restored method');

// own property shadows the prototype
const shadow = new Thing(4);
shadow.get = () => 'own';
same(call(shadow), 'own', 'own shadow');

// another prototype, a null prototype
const other = Object.setPrototypeOf(new Thing(5), { get() { return 'other proto'; } });
same(call(other), 'other proto', 'other prototype');
same(read(Object.setPrototypeOf(new Thing(6), null)), undefined, 'null prototype');

// different receiver shapes
same(call({ v: 8, get() { return 'literal'; } }), 'literal', 'other shape');
const extra = new Thing(9);
extra.more = 1;
same(call(extra), 9, 'extra own property');

// accessor on the prototype
Object.defineProperty(Thing.prototype, 'get', { get() { return () => 'from getter'; }, configurable: true });
same(call(new Thing(10)), 'from getter', 'getter');
Object.defineProperty(Thing.prototype, 'get', { value: original, writable: true, configurable: true });
same(call(new Thing(11)), 11, 'back to data');

// deleted from the prototype
delete Thing.prototype.get;
same(read(new Thing(12)), undefined, 'deleted method');
Thing.prototype.get = original;
for (let i = 0; i < 5000; i++) call(warm);
same(call(new Thing(13)), 13, 'readded method');

// proxy receiver
const proxied = new Proxy(new Thing(14), { get(t, k) { return k === 'get' ? () => 'proxied' : t[k]; } });
same(call(proxied), 'proxied', 'proxy receiver');

// methods in the prototype's overflow storage, two levels up, function prototype
function Wide() {}
for (let k = 0; k < 20; k++) Wide.prototype['m' + k] = function () { return k; };
function callWide(o) { return o.m17(); }
for (let i = 0; i < 5000; i++) callWide(new Wide());
same(callWide(new Wide()), 17, 'overflow slot');
Wide.prototype.m17 = () => 'changed overflow';
same(callWide(new Wide()), 'changed overflow', 'changed overflow slot');
class Base { hello() { return 'base'; } }
class Derived extends Base {}
function hello(o) { return o.hello(); }
for (let i = 0; i < 5000; i++) hello(new Derived());
same(hello(new Derived()), 'base', 'two levels up');
Base.prototype.hello = () => 'base changed';
same(hello(new Derived()), 'base changed', 'two levels up changed');
const fnProto = Object.setPrototypeOf({}, Object.assign(function () {}, { get() { return 'function proto'; } }));
same(call(fnProto), 'function proto', 'function prototype');

// the snapshot is what compiled code uses
const child = spawnSync(process.execPath, ['-e', `
function Thing(v) { this.v = v; }
Thing.prototype.get = function () { return this.v; };
function call(o) { return o.get(); }
const t = new Thing(1); let s = 0;
for (let i = 0; i < 20000; i++) s += call(t);
console.log('total ' + s);
`], { encoding: 'utf8', env: { ...process.env, ANT_DEBUG: 'dump/vm:jit' }, maxBuffer: 64 * 1024 * 1024, timeout: 30000 });
assert.strictEqual(child.status, 0, String(child.error || child.stderr));
const fn = child.stderr.match(/jit_call_[^\n]*:\s*func[\s\S]*?endfunc/);
assert.ok(fn, 'missing compilation for call');
assert.match(fn[0], /gf_proto_snapshot_holder/, 'prototype hit uses the compile-time snapshot');

console.log('PASS compiled prototype method reads follow every change');
