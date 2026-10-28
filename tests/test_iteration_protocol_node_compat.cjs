const assert = require('node:assert');

const log = [];
function tracked(n, opts = {}) {
  return {
    [Symbol.iterator]() {
      let i = 0;
      return {
        next() {
          if (i === opts.throwAt) { i++; throw new Error('next'); }
          return { done: i >= n, value: i++ };
        },
        return() { log.push('ret'); return {}; },
      };
    },
  };
}
function take(f) { log.length = 0; f(); return log.join(' '); }

assert.strictEqual(take(() => { const [a, b, c] = tracked(2); }), '');
assert.strictEqual(take(() => { const [a, b] = tracked(2); }), 'ret');
assert.strictEqual(take(() => { const [a, ...r] = tracked(3); }), '');
assert.strictEqual(take(() => { const [] = tracked(2); }), 'ret');
assert.strictEqual(take(() => { const [a, , ] = tracked(1); }), '');
assert.strictEqual(take(() => { try { for (const x of tracked(3, { throwAt: 1 })) {} } catch {} }), '');
assert.strictEqual(take(() => { try { const [a, b] = tracked(3, { throwAt: 1 }); } catch {} }), '');
assert.strictEqual(take(() => { for (const x of tracked(2)) {} }), '');
assert.strictEqual(take(() => { for (const x of tracked(2)) break; }), 'ret');
assert.strictEqual(take(() => { try { for (const x of tracked(2)) throw new Error('body'); } catch {} }), 'ret');
assert.strictEqual(take(() => { (function () { for (const x of tracked(2)) return; })(); }), 'ret');

let nextAfterDone = 0;
const exhausted = {
  [Symbol.iterator]() {
    return { next() { nextAfterDone++; return { done: true }; } };
  },
};
const [e1, e2, e3] = exhausted;
assert.strictEqual(nextAfterDone, 1);

function forOfConst() { const arr = [1, 2, 3]; const out = []; for (const x of arr) out.push(x); return out.join(); }
function forOfLiteral() { const out = []; for (const x of [7, 8]) out.push(x); return out.join(); }
function forOfParam(a) { const out = []; for (const x of a) out.push(x); return out.join(); }
function spreadParam(a) { return [...a].join(); }
function destructure(a) { const [x, y] = a; return `${x},${y}`; }
function forOfString(s) { const out = []; for (const c of s) out.push(c); return out.join(); }
function spreadString(s) { return [...s].join(); }
function forOfMap(m) { const out = []; for (const [k, v] of m) out.push(k + v); return out.join(); }
function forOfSet(s) { const out = []; for (const v of s) out.push(v); return out.join(); }
function breakArray(a) { for (const x of a) { if (x === 2) break; } }
function throwArray(a) { for (const x of a) { if (x === 1) throw new Error('body'); } }
function returnArray(a) { for (const x of a) return x; }
function breakString(s) { for (const c of s) break; }
function breakMap(m) { for (const e of m) break; }
function breakSet(s) { for (const v of s) break; }
function destructureOne(a) { const [x] = a; return x; }

for (let i = 0; i < 3000; i++) {
  assert.strictEqual(forOfConst(), '1,2,3');
  assert.strictEqual(forOfLiteral(), '7,8');
  assert.strictEqual(forOfParam([4, 5]), '4,5');
  assert.strictEqual(spreadParam([4, 5]), '4,5');
  assert.strictEqual(destructure([4, 5]), '4,5');
  assert.strictEqual(forOfString('ab'), 'a,b');
  assert.strictEqual(spreadString('ab'), 'a,b');
  assert.strictEqual(forOfMap(new Map([['k', 1]])), 'k1');
  assert.strictEqual(forOfSet(new Set([3])), '3');
  breakArray([1, 2, 3]);
  try { throwArray([1]); } catch {}
  returnArray([1]);
  breakString('xy');
  breakMap(new Map([[1, 1]]));
  breakSet(new Set([1]));
  assert.strictEqual(destructureOne([9, 8]), 9);
}

let returns = [];
Object.prototype.return = function () { returns.push(Object.prototype.toString.call(this)); return {}; };
breakArray([1, 2, 3]);
try { throwArray([1]); } catch (e) { assert.strictEqual(e.message, 'body'); }
assert.strictEqual(returnArray([5]), 5);
breakString('xy');
breakMap(new Map([[1, 1]]));
breakSet(new Set([1]));
assert.strictEqual(destructureOne([9, 8]), 9);
assert.strictEqual(destructureOne([9]), 9);
assert.strictEqual(forOfConst(), '1,2,3');
const [only] = [1, 2, 3];
assert.deepStrictEqual(returns, [
  '[object Array Iterator]', '[object Array Iterator]', '[object Array Iterator]',
  '[object String Iterator]', '[object Map Iterator]', '[object Set Iterator]',
  '[object Array Iterator]', '[object Array Iterator]', '[object Array Iterator]',
]);
delete Object.prototype.return;

let resumed = null;
for (const x of [1, 2, 3]) {
  if (x === 1) {
    Object.getPrototypeOf([][Symbol.iterator]()).return = function () { resumed = this.next().value; return {}; };
    break;
  }
}
assert.strictEqual(resumed, 2);
for (const c of 'pq') {
  Object.getPrototypeOf(''[Symbol.iterator]()).return = function () { resumed = this.next().value; return {}; };
  break;
}
assert.strictEqual(resumed, 'q');
delete Object.getPrototypeOf([][Symbol.iterator]()).return;
delete Object.getPrototypeOf(''[Symbol.iterator]()).return;

const IteratorProto = Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));
IteratorProto.return = function () { throw new Error('return-error'); };
assert.throws(() => breakArray([1, 2]), /return-error/);
assert.throws(() => throwArray([1]), /body/);
assert.throws(() => { for (const x of [1, 2]) break; }, /return-error/);
IteratorProto.return = function () { return 1; };
assert.throws(() => breakArray([1, 2]), TypeError);
assert.throws(() => throwArray([1]), /body/);
Object.defineProperty(IteratorProto, 'return', { configurable: true, get() { throw new Error('getter'); } });
assert.throws(() => breakArray([1, 2]), /getter/);
assert.throws(() => throwArray([1]), /body/);
delete IteratorProto.return;
breakArray([1, 2]);

function* inner() { yield 'a'; yield 'b'; }
function* delegateArray() { return yield* [1, 2]; }
const da = delegateArray();
da.next();
assert.throws(() => da.throw(new Error('thrown')), TypeError);
const dr = delegateArray();
dr.next();
assert.deepStrictEqual(dr.return(5), { value: 5, done: true });
let arrayReturnCalls = 0;
Object.getPrototypeOf([][Symbol.iterator]()).return = function (v) { arrayReturnCalls++; return { value: 'closed', done: true }; };
const dr2 = delegateArray();
dr2.next();
assert.deepStrictEqual(dr2.return(6), { value: 'closed', done: true });
const da2 = delegateArray();
da2.next();
assert.throws(() => da2.throw(new Error('thrown')), TypeError);
assert.strictEqual(arrayReturnCalls, 2);
delete Object.getPrototypeOf([][Symbol.iterator]()).return;

const innerResult = { value: 1, done: false };
let valueReads = 0;
const counted = { get value() { valueReads++; return 'v'; }, done: false };
const passThrough = {
  [Symbol.iterator]() { return this; },
  n: 0,
  next() { this.n++; return this.n === 1 ? innerResult : this.n === 2 ? counted : { done: true, value: 'end' }; },
};
function* delegating() { return yield* passThrough; }
const pt = delegating();
assert.strictEqual(pt.next(), innerResult);
assert.strictEqual(pt.next(), counted);
assert.strictEqual(valueReads, 0);
assert.deepStrictEqual(pt.next(), { value: 'end', done: true });
passThrough.n = 0;
valueReads = 0;
assert.deepStrictEqual([...delegating()], [1, 'v']);
assert.strictEqual(valueReads, 1);
const noReturn = { [Symbol.iterator]() { return this; }, next() { return { value: 1, done: false }; } };
function* delegateNoReturn() { yield* noReturn; }
const dnr = delegateNoReturn();
dnr.next();
assert.deepStrictEqual(dnr.return(7), { value: 7, done: true });

function* counter(n) { for (let i = 0; i < n; i++) yield i; return 'done'; }
function sumGen(n) { let s = 0; for (const v of counter(n)) s += v; return s; }
for (let i = 0; i < 2000; i++) assert.strictEqual(sumGen(10), 45);
assert.deepStrictEqual([...counter(3)], [0, 1, 2]);
assert.deepStrictEqual(Array.from(counter(3)), [0, 1, 2]);
assert.deepStrictEqual([...new Set(counter(3))], [0, 1, 2]);
const [g0, g1] = counter(5);
assert.strictEqual(g0 + g1, 1);
assert.deepStrictEqual(Object.fromEntries((function* () { yield ['a', 1]; })()), { a: 1 });

let genFinally = 0;
function* guarded() { try { yield 1; yield 2; } finally { genFinally++; } }
for (const v of guarded()) break;
const [gx] = guarded();
assert.strictEqual(genFinally, 2);
function* failing() { yield 1; throw new Error('gen-fail'); }
assert.throws(() => { for (const v of failing()) {} }, /gen-fail/);
const done = counter(1);
for (const v of done) {}
let after = 0;
for (const v of done) after++;
assert.strictEqual(after, 0);
let reentered;
const selfIter = (function* () { try { for (const v of selfIter) {} } catch (e) { reentered = e; } yield 1; })();
selfIter.next();
assert.ok(reentered instanceof TypeError);

const GenProto = Object.getPrototypeOf(counter(0)).__proto__;
const originalNext = GenProto.next;
let patchedCalls = 0;
GenProto.next = function (...args) { patchedCalls++; return originalNext.apply(this, args); };
assert.strictEqual(sumGen(4), 6);
assert.deepStrictEqual([...counter(2)], [0, 1]);
assert.deepStrictEqual(Array.from(counter(2)), [0, 1]);
assert.ok(patchedCalls >= 3 * 3);
GenProto.next = originalNext;
const own = counter(3);
own.next = function () { return { done: true }; };
let ownSteps = 0;
for (const v of own) ownSteps++;
assert.strictEqual(ownSteps, 0);

let nextGets = 0;
const getterNext = {
  [Symbol.iterator]() {
    let i = 0;
    return { get next() { nextGets++; return () => (i < 2 ? { value: ['k' + i, i++], done: false } : { done: true }); } };
  },
};
assert.deepStrictEqual(Object.fromEntries(getterNext), { k0: 0, k1: 1 });
assert.strictEqual(nextGets, 1);
nextGets = 0;
assert.strictEqual(Array.from(getterNext).length, 2);
assert.strictEqual(nextGets, 1);
nextGets = 0;
assert.strictEqual([...getterNext].length, 2);
assert.strictEqual(nextGets, 1);
nextGets = 0;
assert.strictEqual(new Map(getterNext).size, 2);
assert.strictEqual(nextGets, 1);

const badEntry = {
  [Symbol.iterator]() {
    return { next() { return { value: 5, done: false }; }, return() { throw new Error('ret'); } };
  },
};
assert.throws(() => Object.fromEntries(badEntry), TypeError);
assert.throws(() => Array.from(badEntry, () => { throw new RangeError('map'); }), RangeError);
assert.throws(() => Object.fromEntries({ [Symbol.iterator]() { return { next() { return 1; } }; } }), TypeError);
assert.throws(() => Object.fromEntries({ [Symbol.iterator]() { return 1; } }), TypeError);
assert.throws(() => Object.fromEntries({
  [Symbol.iterator]() { return { next() { throw new Error('n'); } }; },
}), /n/);
let inheritedDone = 0;
const protoResult = { get done() { inheritedDone++; return true; } };
assert.deepStrictEqual(Array.from({ [Symbol.iterator]() { return { next() { return Object.create(protoResult); } }; } }), []);
assert.strictEqual(inheritedDone, 1);

async function promiseCases() {
  let resolveCloses = 0;
  const thenable = {
    [Symbol.iterator]() {
      return { next() { return { value: 1, done: false }; }, return() { resolveCloses++; throw new Error('ret'); } };
    },
  };
  const OrigResolve = Promise.resolve;
  Promise.resolve = function () { throw new SyntaxError('resolve'); };
  try {
    await assert.rejects(Promise.all(thenable), SyntaxError);
  } finally {
    Promise.resolve = OrigResolve;
  }
  assert.strictEqual(resolveCloses, 1);
  assert.deepStrictEqual(await Promise.all(counter(3)), [0, 1, 2]);
  assert.deepStrictEqual(await Promise.all(getterNext), [['k0', 0], ['k1', 1]]);
}

const ArrayIterProto = Object.getPrototypeOf([][Symbol.iterator]());
const savedArrayIter = Array.prototype[Symbol.iterator];
Array.prototype[Symbol.iterator] = function* () { yield 'patched'; };
assert.strictEqual(forOfConst(), 'patched');
assert.strictEqual(forOfLiteral(), 'patched');
assert.strictEqual(forOfParam([1, 2]), 'patched');
assert.strictEqual(spreadParam([1, 2]), 'patched');
assert.strictEqual(destructure([1, 2]), 'patched,undefined');
assert.deepStrictEqual(Array.from([1, 2]), ['patched']);
assert.deepStrictEqual([...new Set([1, 2])], ['patched']);
assert.deepStrictEqual(Array.from(new Float64Array([1, 2])), [NaN]);
assert.deepStrictEqual(Array.from(Uint8Array.from([1, 2])), [0]);
assert.strictEqual(new Uint8Array([1, 2]).length, 1);
Array.prototype[Symbol.iterator] = savedArrayIter;
assert.strictEqual(forOfConst(), '1,2,3');
assert.strictEqual(new Uint8Array([1, 2]).length, 2);

const savedArrayNext = ArrayIterProto.next;
let arrayNextCalls = 0;
ArrayIterProto.next = function () { arrayNextCalls++; return savedArrayNext.call(this); };
assert.strictEqual(forOfConst(), '1,2,3');
assert.strictEqual(forOfLiteral(), '7,8');
assert.strictEqual(spreadParam([1, 2]), '1,2');
assert.strictEqual(new Float32Array([1, 2]).length, 2);
assert.ok(arrayNextCalls >= 4 + 3 + 3 + 3);
ArrayIterProto.next = savedArrayNext;

const savedStringIter = String.prototype[Symbol.iterator];
String.prototype[Symbol.iterator] = function* () { yield 'P'; };
assert.strictEqual(forOfString('ab'), 'P');
assert.strictEqual(spreadString('ab'), 'P');
assert.deepStrictEqual(Array.from('ab'), ['P']);
const [s0, s1] = 'ab';
assert.strictEqual(s0 + s1, 'Pundefined');
String.prototype[Symbol.iterator] = savedStringIter;
assert.strictEqual(forOfString('ab'), 'a,b');
const StringIterProto = Object.getPrototypeOf(''[Symbol.iterator]());
const savedStringNext = StringIterProto.next;
StringIterProto.next = function () { const r = savedStringNext.call(this); if (!r.done) r.value = r.value.toUpperCase(); return r; };
assert.strictEqual(forOfString('ab'), 'A,B');
assert.strictEqual(spreadString('ab'), 'A,B');
StringIterProto.next = savedStringNext;

const MapIterProto = Object.getPrototypeOf(new Map().entries());
const savedMapNext = MapIterProto.next;
MapIterProto.next = function () { return { done: true }; };
assert.strictEqual(forOfMap(new Map([['k', 1]])), '');
assert.deepStrictEqual([...new Map([[1, 2]])], []);
MapIterProto.next = savedMapNext;
const SetIterProto = Object.getPrototypeOf(new Set().values());
const savedSetNext = SetIterProto.next;
SetIterProto.next = function () { return { done: true }; };
assert.strictEqual(forOfSet(new Set([3])), '');
SetIterProto.next = savedSetNext;
assert.strictEqual(forOfSet(new Set([3])), '3');

let globalReads = 0;
const re = /a/g;
Object.defineProperty(re, 'global', { get() { globalReads++; return true; } });
assert.strictEqual('aaa'.replace(re, 'b'), 'bbb');
assert.deepStrictEqual('aaa'.match(re), ['a', 'a', 'a']);
assert.strictEqual(globalReads, 2);

promiseCases().then(() => console.log('iteration protocol ok'), e => { console.error(e); process.exit(1); });
