// `'key' in obj` caches per site and integer keys skip string conversion on dense arrays;
// every case runs hot so the cached and compiled paths see the same mutations
const assert = require('node:assert');

const hot = (fn, n = 3000) => { let r; for (let i = 0; i < n; i++) r = fn(i); return r; };

class Base { m() {} }
class Derived extends Base {}
const d = new Derived();
assert.deepStrictEqual(hot(() => ['m' in d, 'nope' in d, 'constructor' in d, 'toString' in d]), [true, false, true, true]);

const siteMiss = o => 'late' in o;
const plain = { a: 1 };
assert.strictEqual(hot(() => siteMiss(plain)), false);
Object.prototype.late = 1;
assert.strictEqual(siteMiss(plain), true, 'a key added to Object.prototype invalidates a cached miss');
delete Object.prototype.late;
assert.strictEqual(siteMiss(plain), false, 'deleting it again is seen');
Base.prototype.late = 1;
assert.strictEqual(hot(() => siteMiss(d)), true, 'a key added to a class prototype is seen');
delete Base.prototype.late;
assert.strictEqual(siteMiss(d), false);

const siteOwn = o => 'a' in o;
const own = { a: 1 };
assert.strictEqual(hot(() => siteOwn(own)), true);
delete own.a;
assert.strictEqual(siteOwn(own), false, 'deleting an own key is seen');
Object.setPrototypeOf(own, { a: 2 });
assert.strictEqual(siteOwn(own), true, 'a new prototype is seen');

const shapes = [{ a: 1 }, { b: 1, a: 2 }, { c: 1 }, Object.create({ a: 3 }), Object.create(null), [], function a() {}];
assert.deepStrictEqual(hot(i => shapes.map(siteOwn)), [true, true, false, true, false, false, false]);

const accessor = Object.create({ get a() { throw new Error('getter must not run'); } });
assert.strictEqual(hot(() => siteOwn(accessor)), true, 'an inherited accessor counts without running');

let traps = 0;
const proxy = new Proxy({}, { has(t, k) { traps++; return k === 'a'; } });
assert.strictEqual(hot(() => siteOwn(proxy), 100), true);
assert.strictEqual(traps, 100, 'a proxy has trap runs on every check');
assert.strictEqual(hot(() => siteOwn(Object.create(proxy)), 10), true, 'a proxy in the chain is consulted');

assert.deepStrictEqual(hot(() => ['buffer' in new Uint8Array(1), 'foo' in new String('ab'), 'prototype' in function () {}, 'prototype' in (() => {}), 'name' in Math.max, '__proto__' in {}, '__proto__' in Object.create(null)]),
  [true, false, true, false, true, true, false]);
assert.throws(() => 'a' in 5, TypeError);
assert.throws(() => hot(() => 'a' in null, 10), TypeError);

globalThis.inGlobalLater = undefined;
assert.strictEqual(hot(() => 'inGlobalLater' in globalThis), true);

const arr = [1, , 3];
assert.deepStrictEqual(hot(() => [0 in arr, 1 in arr, 2 in arr, 3 in arr, -0 in arr, 1.5 in arr, NaN in arr, -1 in arr]), [true, false, true, false, true, false, false, false]);
Array.prototype[1] = 'p';
assert.deepStrictEqual([1 in arr, 4 in arr], [true, false], 'a hole sees Array.prototype elements');
Object.prototype[4] = 'o';
assert.deepStrictEqual(hot(() => [1 in arr, 4 in arr]), [true, true], 'an index on Object.prototype is seen');
delete Array.prototype[1];
delete Object.prototype[4];
assert.deepStrictEqual(hot(() => [1 in arr, 4 in arr]), [false, false]);

const long = [1, 2];
long.length = 10;
assert.deepStrictEqual(hot(() => [1 in long, 5 in long, 9 in long]), [true, false, false]);
const big = [];
big[4294967295] = 'not an index';
assert.deepStrictEqual(hot(() => [4294967295 in big, 4294967294 in big]), [true, false]);
assert.strictEqual(hot(() => (function () { return 0 in arguments && !(1 in arguments); })(7)), true);

console.log('in operator ic OK');
