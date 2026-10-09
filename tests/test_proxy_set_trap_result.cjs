const assert = require('node:assert');

// A proxy's set trap answering false makes a strict Set throw ("trap returned
// falsish"), stays silent in sloppy code, and is what Reflect.set returns.
// Symbol keys reach the trap too, and Reflect.set converts keys like any Set.
const rejecting = () => new Proxy({}, { set() { return false; } });
const rejectingArray = () => new Proxy([1, 2], { set() { return false; } });
const falsish = { name: 'TypeError', message: /trap returned falsish/ };
const sym = Symbol('s');

assert.throws(() => { 'use strict'; rejecting().x = 1; }, falsish);
assert.throws(() => { 'use strict'; rejecting()[0] = 1; }, falsish);
assert.throws(() => { 'use strict'; const p = rejecting(); const k = 'k' + 1; p[k] = 1; }, falsish);
assert.throws(() => { 'use strict'; rejecting()[sym] = 1; },
  { name: 'TypeError', message: "'set' on proxy: trap returned falsish for property 'Symbol(s)'" });
assert.throws(() => { 'use strict'; Object.create(rejecting()).x = 1; }, falsish);
assert.throws(() => { 'use strict'; Object.create(rejecting())[sym] = 1; }, falsish);

(function sloppy() {
  const p = rejecting();
  p.x = 1;
  p[0] = 1;
  p[sym] = 1;
  const inheriting = Object.create(rejecting());
  inheriting.x = 1;
  assert.deepStrictEqual(Object.keys(inheriting), []);
})();

// builtins do Set(O, k, v, true) whatever the caller's mode
assert.throws(() => Array.prototype.fill.call(rejectingArray(), 0), falsish);
assert.throws(() => Array.prototype.push.call(rejectingArray(), 3), falsish);
assert.throws(() => Array.prototype.reverse.call(rejectingArray()), falsish);
assert.throws(() => Array.prototype.copyWithin.call(rejectingArray(), 0, 1), falsish);

// compiled code throws on every iteration too
(function hot() {
  'use strict';
  const p = rejecting();
  let thrown = 0;
  for (let i = 0; i < 2000; i++) {
    try { p.x = i; } catch { thrown++; }
  }
  assert.strictEqual(thrown, 2000);
})();

// truthy answers, missing traps and invariants are unchanged
(function accepted() {
  'use strict';
  new Proxy({}, { set() { return 1; } }).x = 1;
  const plain = new Proxy({}, {});
  plain.x = 2;
  plain[sym] = 3;
  assert.strictEqual(plain.x, 2);
  assert.strictEqual(plain[sym], 3);
  const target = {};
  Object.defineProperty(target, 'x', { value: 1 });
  assert.throws(() => { new Proxy(target, { set() { return true; } }).x = 2; }, TypeError);
})();

// symbol keys go to the trap, not onto the proxy
const log = [];
const tracing = new Proxy({}, { set(t, k) { log.push(typeof k); return true; } });
tracing[sym] = 1;
Object.create(tracing)[sym] = 2;
Reflect.set(tracing, sym, 3);
assert.deepStrictEqual(log, ['symbol', 'symbol', 'symbol']);
assert.strictEqual(Object.getOwnPropertySymbols(tracing).length, 0);

// Reflect.set reports the trap's answer and converts its key
(function reflect() {
  'use strict';
  assert.strictEqual(Reflect.set(rejecting(), 'x', 1), false);
  assert.strictEqual(Reflect.set(rejecting(), sym, 1), false);
  assert.strictEqual(Reflect.set(new Proxy({}, {}), 'x', 1), true);
  const seen = [];
  const p = new Proxy({}, { set(t, k) { seen.push(typeof k); return true; } });
  assert.strictEqual(Reflect.set(p, 5, 1), true);
  assert.deepStrictEqual(seen, ['string']);
  const arr = [];
  assert.strictEqual(Reflect.set(arr, 0, 'x'), true);
  assert.deepStrictEqual(arr, ['x']);
  const withSym = {};
  assert.strictEqual(Reflect.set(withSym, sym, 1), true);
  assert.strictEqual(withSym[sym], 1);
  assert.strictEqual(Reflect.set(Object.freeze({ [sym]: 1 }), sym, 2), false);
  const objKey = {};
  assert.strictEqual(Reflect.set(objKey, { toString() { return 'kk'; } }, 3), true);
  assert.strictEqual(objKey.kk, 3);
  assert.throws(() => Reflect.set(1, 'x', 1), { name: 'TypeError', message: 'Reflect.set called on non-object' });
})();

console.log('proxy set trap result: ok');
