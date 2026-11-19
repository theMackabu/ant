const assert = require('node:assert');

const N = 8000;
const descriptor = (key) => Object.getOwnPropertyDescriptor(Function.prototype, key);

for (const key of ['arguments', 'caller']) {
  const d = descriptor(key);
  assert.strictEqual(typeof d.get, 'function', key);
  assert.strictEqual(typeof d.set, 'function', key);
  assert.strictEqual(d.enumerable, false, key);
  assert.strictEqual(d.configurable, true, key);
  assert.notStrictEqual(d.get, d.set, key);
}
assert.notStrictEqual(descriptor('arguments').get, descriptor('caller').get);
assert.throws(() => Function.prototype.arguments, TypeError);
assert.throws(() => Function.prototype.caller, TypeError);

function sloppy() {}
function strict() { 'use strict'; }
const arrow = () => {};
const strictArrow = (() => { 'use strict'; return () => {}; })();
async function asyncFunction() {}
function* generator() {}
const method = { m() {} }.m;
const getter = Object.getOwnPropertyDescriptor({ get g() { return 1; } }, 'g').get;
const bound = sloppy.bind(null);
class Parent { m() { return 1; } static s() { return 2; } }

const restricted = { strict, arrow, strictArrow, asyncFunction, generator, method, getter, bound, Parent, instanceMethod: Parent.prototype.m, staticMethod: Parent.s, builtin: Math.max, proxy: new Proxy(sloppy, {}) };
for (const [name, fn] of Object.entries(restricted)) {
  for (const key of ['arguments', 'caller']) {
    assert.throws(() => fn[key], TypeError, `${name}.${key}`);
    assert.throws(() => { fn[key] = 1; }, TypeError, `${name}.${key} =`);
    assert.strictEqual(Object.prototype.hasOwnProperty.call(fn, key), false, `${name} own ${key}`);
    assert.strictEqual(key in fn, true, `${name} in ${key}`);
  }
}

for (const key of ['arguments', 'caller']) {
  assert.strictEqual(sloppy[key], null, key);
  sloppy[key] = 1;
  assert.strictEqual(sloppy[key], null, `${key} after assignment`);
  assert.strictEqual(Object.prototype.hasOwnProperty.call(sloppy, key), false, key);
  assert.strictEqual(descriptor(key).get.call(sloppy), null, key);
  assert.strictEqual(descriptor(key).set.call(sloppy, 1), undefined, key);
  assert.strictEqual(Reflect.get(Function.prototype, key, sloppy), null, key);
  assert.throws(() => descriptor(key).get.call(strict), TypeError, key);
  assert.throws(() => descriptor(key).get.call({}), TypeError, key);
  assert.throws(() => descriptor(key).get.call(undefined), TypeError, key);
  assert.throws(() => Reflect.get(Function.prototype, key, {}), TypeError, key);
  assert.throws(() => Object.create(Function.prototype)[key], TypeError, key);
  assert.throws(() => Object.create(sloppy)[key], TypeError, key);
}

(function () {
  'use strict';
  assert.strictEqual(sloppy.arguments, null);
  assert.strictEqual(sloppy.caller, null);
  assert.strictEqual({}.arguments, undefined);
  assert.strictEqual([].caller, undefined);
})();

class Child extends Parent {
  readArguments() { return super.arguments; }
  readCaller() { return super.caller; }
  writeArguments() { super.arguments = 1; return this.arguments; }
  static readArguments() { return super.arguments; }
  static readCaller() { return super.caller; }
  static writeArguments() { super.arguments = 1; }
  static writeCaller() { super.caller = 1; }
}
const child = new Child();
assert.strictEqual(child.readArguments(), undefined);
assert.strictEqual(child.readCaller(), undefined);
assert.strictEqual(child.writeArguments(), 1);

const outcome = (fn) => { try { return [typeof fn()]; } catch (error) { return error instanceof TypeError ? 'TypeError' : 'other'; } };
const hot = { readArguments: 0, readCaller: 0, writeArguments: 0, writeCaller: 0 };
for (let i = 0; i < N; i++) {
  for (const name in hot) if (outcome(() => Child[name]()) === 'TypeError') hot[name]++;
}
assert.deepStrictEqual(hot, { readArguments: N, readCaller: N, writeArguments: N, writeCaller: N });

function SloppyHome() {}
function sloppyStatics() {}
const viaSloppy = { __proto__: sloppyStatics, read() { return super.arguments; }, readCaller() { return super.caller; } };
let nulls = 0, throws = 0;
for (let i = 0; i < N; i++) {
  const receiver = i < N / 2 ? SloppyHome : strict;
  try {
    if (viaSloppy.read.call(receiver) === null && viaSloppy.readCaller.call(receiver) === null) nulls++;
  } catch (error) {
    if (error instanceof TypeError) throws++;
  }
}
assert.deepStrictEqual([nulls, throws], [N / 2, N / 2]);

function readDirect(fn) { return fn.arguments; }
let direct = 0, directThrows = 0;
for (let i = 0; i < N; i++) {
  try {
    if (readDirect(i < N / 2 ? sloppy : strict) === null) direct++;
  } catch (error) {
    if (error instanceof TypeError) directThrows++;
  }
}
assert.deepStrictEqual([direct, directThrows], [N / 2, N / 2]);

class Own { static arguments = 3; static caller() { return 4; } }
assert.strictEqual(Own.arguments, 3);
assert.strictEqual(Own.caller(), 4);

console.log('ok');
