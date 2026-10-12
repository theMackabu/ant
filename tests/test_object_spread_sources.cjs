// object spread and rest across source kinds (arrays, strings, typed arrays, proxies), compiled or not
const util = require('util');

function assert(cond, msg) {
  if (!cond) throw new Error(msg || 'assertion failed');
}

function throws(fn) {
  try { fn(); } catch (e) { return e; }
  return null;
}

const same = (a, b) => JSON.stringify(Reflect.ownKeys(a).map(String)) + JSON.stringify(a) === JSON.stringify(Reflect.ownKeys(b).map(String)) + JSON.stringify(b);

const arr = [10, , 30];
arr.named = 'n';
assert(same({ ...arr }, { 0: 10, 2: 30, named: 'n' }), 'array spread copies elements, skips holes, keeps named keys');
assert(same({ ...'ab' }, { 0: 'a', 1: 'b' }), 'string spread copies code units');
assert(same({ ...'😀' }, { 0: '\ud83d', 1: '\ude00' }), 'string spread splits surrogate pairs');

const wrapped = new String('xy');
wrapped.extra = 1;
assert(same({ ...wrapped }, { 0: 'x', 1: 'y', extra: 1 }), 'String objects spread their characters and own keys');
assert(same({ ...null, ...undefined, ...5, ...true }, {}), 'nullish and primitive non-strings spread nothing');

const { 0: first, ...restArr } = [1, 2, 3];
assert(first === 1 && same(restArr, { 1: 2, 2: 3 }), 'rest from an array leaves out the named index');
const { 1: second, ...restStr } = 'abc';
assert(second === 'b' && same(restStr, { 0: 'a', 2: 'c' }), 'rest from a string leaves out the named index');

function spread(o) { return { ...o, extra: 1 }; }
function rest(o) { const { b, ...r } = o; return r; }
function guarded(o) { try { return { ...o }; } catch (e) { return e.message; } }

let s, r, g;
for (let i = 0; i < 20000; i++) {
  s = spread({ x: i, y: 2 });
  r = rest({ b: 1, c: i, d: 3 });
  g = guarded(i % 2 ? { get t() { throw new Error('boom'); } } : { k: i });
}
assert(same(s, { x: 19999, y: 2, extra: 1 }), 'compiled spread copies every key');
assert(same(r, { c: 19999, d: 3 }), 'compiled rest leaves out named keys');
assert(g === 'boom', 'compiled spread rethrows getter errors');

const zz = Symbol.for('ant.spread.zz');
let setterHits = 0;
Object.defineProperty(Object.prototype, zz, { set() { setterHits++; }, configurable: true });
const symCopy = { ...{ [zz]: 1 } };
const { a: _a, ...symRest } = { a: 1, [zz]: 2 };
delete Object.prototype[zz];
assert(setterHits === 0 && symCopy[zz] === 1 && symRest[zz] === 2, 'symbol keys are defined, not set through inherited setters');

const ro = Symbol('ro');
Object.defineProperty(Object.prototype, ro, { value: 0, writable: false, configurable: true });
const roCopy = { ...{ [ro]: 7 } };
delete Object.prototype[ro];
assert(Object.hasOwn(roCopy, ro) && roCopy[ro] === 7, 'an inherited read-only symbol does not block the copy');

const grows = { a: 1, get b() { this.d = 4; return 2; }, c: 3 };
assert(same({ ...grows }, { a: 1, b: 2, c: 3 }), 'keys a getter adds are not copied');
const shrinks = { get a() { delete this.c; this.z = 9; return 1; }, b: 2, c: 3 };
assert(same({ ...shrinks }, { a: 1, b: 2 }), 'keys a getter deletes are skipped');
assert(same({ get a() { return 'acc'; }, ...{ a: 1 } }, { a: 1 }), 'spread replaces an accessor already on the literal');

const arrAcc = [1, , 3];
Object.defineProperty(arrAcc, 'acc', { get() { return 'g'; }, enumerable: true });
assert(same({ ...arrAcc }, { 0: 1, 2: 3, acc: 'g' }), 'array spread copies elements, then own accessors');

const wideKeys = Array.from({ length: 65540 }, (_, i) => 'k' + i).join(',');
let tooMany = null;
try { new Function('o', 'const {' + wideKeys + ', ...r} = o; return r;'); } catch (e) { tooMany = e; }
assert(tooMany instanceof SyntaxError, 'too many keys before a rest element is a SyntaxError');

const lowGetter = [1, 2, 3];
Object.defineProperty(lowGetter, 0, { get() { lowGetter[2] = 99; return 1; }, enumerable: true });
assert(same({ ...lowGetter }, { 0: 1, 1: 2, 2: 99 }), 'array elements are read in order, after earlier getters run');

assert(same({ ...new Uint8Array([7, 8]) }, { 0: 7, 1: 8 }), 'typed arrays spread their elements');
assert(Object.keys(new Uint8Array([7, 8])).join() === '0,1', 'typed array keys are its indices');
assert(Object.hasOwn(new Uint8Array([7]), 0) && !Object.hasOwn(new Uint8Array([7]), 1), 'typed array indices are own properties');

const original = { a: 1, b: 2, c: 3 };
const copied = { ...original };
copied.d = 4;
delete copied.a;
Object.defineProperty(copied, 'b', { enumerable: false });
assert(same(original, { a: 1, b: 2, c: 3 }), 'changing a copy leaves the source alone');
original.e = 5;
Object.freeze(original);
assert(same({ ...copied }, { c: 3, d: 4 }) && !Object.isFrozen(copied), 'changing the source leaves a copy alone');

const big = {};
for (let i = 0; i < 40; i++) big['p' + i] = i;
const bigCopy = { ...big };
bigCopy.p0 = 'x';
assert(big.p0 === 0 && bigCopy.p39 === 39 && Object.keys(bigCopy).length === 40, 'large sources copy every slot');

class Point { constructor() { this.x = 1; this.y = 2; this.z = 3; this.w = 4; this.v = 5; } }
const point = new Point();
const pointCopy = { ...point };
assert(Object.getPrototypeOf(pointCopy) === Object.prototype && same(pointCopy, { x: 1, y: 2, z: 3, w: 4, v: 5 }), 'class instances copy as plain data');

const watchedKeys = { then: 1, prototype: 2, constructor: 3 };
assert(same({ ...watchedKeys }, { then: 1, prototype: 2, constructor: 3 }), 'watched names copy as data');
const hiddenSrc = Object.defineProperty({ shown: 1 }, 'hidden', { value: 2, enumerable: false });
assert(same({ ...hiddenSrc }, { shown: 1 }), 'non-enumerable keys are left out');

let lastCopy;
for (let i = 0; i < 5000; i++) { lastCopy = { ...original }; lastCopy.n = i; }
assert(same(lastCopy, { a: 1, b: 2, c: 3, e: 5, n: 4999 }) && !('n' in original), 'hot copies stay independent');

const internalOnly = [
  new ArrayBuffer(2), new DataView(new ArrayBuffer(2)), /a/g, new Event('x'), (class K { m() {} }).prototype,
];
for (const value of internalOnly)
  assert(Object.keys(value).length === 0 && Object.keys({ ...value }).length === 0, 'internal state stays out of keys and spread: ' + Object.prototype.toString.call(value));

const p = new Proxy({}, {
  ownKeys: () => ['a', 'b'],
  getOwnPropertyDescriptor: () => ({ value: 0, enumerable: true, configurable: true }),
  get: (t, k) => 'v' + String(k),
});
const proxyCopy = { ...p };
assert(proxyCopy.a === 'va' && proxyCopy.b === 'vb', 'object spread uses proxy traps');
assert(util.types.isProxy(p), 'user proxies are still proxies');
assert(Reflect.ownKeys(p).join() === 'a,b' && Object.getOwnPropertyNames(p).join() === 'a,b', 'key APIs use the ownKeys trap');

const sym = Symbol('s');
const withSym = new Proxy({ x: 1, [sym]: 2 }, {});
const proxySymCopy = { ...withSym };
assert(proxySymCopy.x === 1 && proxySymCopy[sym] === 2, 'object spread copies symbol keys from proxies');

const throwing = new Proxy({}, {
  ownKeys: () => ['a'],
  getOwnPropertyDescriptor: () => ({ value: 0, enumerable: true, configurable: true }),
  get() { throw new Error('boom-get'); },
});
assert(throws(() => ({ ...throwing }))?.message === 'boom-get', 'spread rethrows a trap error');
assert(throws(() => { const { ...rest } = throwing; return rest; })?.message === 'boom-get', 'object rest rethrows a trap error');

const { proxy: revocable, revoke } = Proxy.revocable({ a: 1 }, {});
revoke();
assert(throws(() => ({ ...revocable })) instanceof TypeError, 'spreading a revoked proxy throws');

const getterThrows = { get a() { throw new Error('boom-accessor'); } };
assert(throws(() => ({ ...getterThrows }))?.message === 'boom-accessor', 'spread rethrows getter errors');

const log = [];
const traced = new Proxy({ a: 1, b: 2, c: 3 }, {
  ownKeys: (t) => { log.push('ownKeys'); return Reflect.ownKeys(t); },
  getOwnPropertyDescriptor: (t, k) => { log.push('desc:' + k); return Reflect.getOwnPropertyDescriptor(t, k); },
  get: (t, k) => { log.push('get:' + String(k)); return t[k]; },
});
const { a: tracedA, ...tracedRest } = traced;
assert(tracedA === 1 && tracedRest.b === 2 && tracedRest.c === 3 && !('a' in tracedRest), 'rest leaves out named keys');
assert(log.join() === 'get:a,ownKeys,desc:b,get:b,desc:c,get:c', 'rest skips excluded keys and reads each key before the next: ' + log.join());

let keyCalls = 0;
const key = () => { keyCalls++; return 'b'; };
const { [key()]: computedB, ...afterComputed } = { a: 1, b: 2 };
assert(keyCalls === 1 && computedB === 2 && !('b' in afterComputed) && afterComputed.a === 1, 'a computed rest key is evaluated once');

let excludedGetterCalls = 0;
const { x: namedX, ...noX } = { get x() { excludedGetterCalls++; return 1; }, y: 2 };
assert(excludedGetterCalls === 1 && noX.y === 2 && !('x' in noX), 'an excluded getter runs only for its own binding');

const withProtoKey = JSON.parse('{"__proto__": {"polluted": true}, "z": 1}');
const protoCopy = { ...withProtoKey };
assert(Object.getPrototypeOf(protoCopy) === Object.prototype && Object.hasOwn(protoCopy, '__proto__'), 'spread defines __proto__ as an own key');

// excluded keys stay out of rest even when the source creates its keys lazily
const fresh = n => ['k', n, Date.now() % 1].join('');
const { '0': s0, ...strRest } = 'xyz';
assert(s0 === 'x' && JSON.stringify(strRest) === '{"1":"y","2":"z"}', 'a string source drops the excluded index');
const { 0: n0, ...numKeyRest } = 'xyz';
assert(n0 === 'x' && !('0' in numKeyRest), 'a numeric excluded key matches the index');
const { [1]: w1, ...wrapRest } = new String('ab');
assert(w1 === 'b' && JSON.stringify(wrapRest) === '{"0":"a"}', 'a String wrapper source drops the excluded index');
const bigArr = Array.from({ length: 5000 }, (_, i) => i);
const idx = String(4000 + (Date.now() % 1));
const { [idx]: bigAt, ...bigRest } = bigArr;
assert(bigAt === 4000 && !(idx in bigRest) && bigRest[3999] === 3999, 'a dense array drops a computed index key');
const proxyKey = fresh(1);
const { [proxyKey]: pv, ...proxyRest } = new Proxy({ [proxyKey]: 1, keep: 2 }, {});
assert(pv === 1 && !(proxyKey in proxyRest) && proxyRest.keep === 2, 'a proxy source drops a never-interned key');
const envKey = ['ANT', 'REST', 'EXCL', process.pid].join('_');
process.env[envKey] = 'v';
const { [envKey]: ev, ...envRest } = process.env;
assert(ev === 'v' && !(envKey in envRest), 'process.env drops a runtime-built key');
delete process.env[envKey];

const getterOrder = [];
const indexGetters = { get b() { getterOrder.push('b'); return 1; }, get 1() { getterOrder.push('1'); return 2; }, get 0() { getterOrder.push('0'); return 3; } };
const orderedSpread = { ...indexGetters };
assert(JSON.stringify([getterOrder, Object.keys(orderedSpread)]) === '[["0","1","b"],["0","1","b"]]', 'index getters run first, in ascending order');

console.log('object spread sources OK');
