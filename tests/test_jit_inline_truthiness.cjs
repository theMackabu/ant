const assert = require('node:assert');

function choose(value) { if (value) return 11; return 22; }
function caller(value) { const result = choose(value); return result + 1; }
function andValue(value) { return value && 13; }
function andCaller(value) { const result = andValue(value); return result; }
function orValue(value) { return value || 17; }
function orCaller(value) { const result = orValue(value); return result; }

const values = [0, -0, NaN, '', null, undefined, 0n, false, 1, -1, 'x', {}, [], 1n, true];
for (let i = 0; i < 400; i++) {
  for (const value of values) {
    assert.strictEqual(caller(value), value ? 12 : 23, `if condition at ${i}`);
    assert.strictEqual(andCaller(value), value && 13, `and condition at ${i}`);
    assert.strictEqual(orCaller(value), value || 17, `or condition at ${i}`);
  }
}
console.log('jit inline truthiness: ok');


function chooseCount(opts) { if (opts.count) return 3; return 7; }
function chooseNotCount(opts) { if (!opts.count) return 7; return 3; }
function negateCount(opts) { return !opts.count; }
function orCount(opts) { return opts.count || 'fallback'; }
function andCount(opts) { return opts.count && 'taken'; }

function callChooseCount(opts) { return chooseCount(opts); }
function callChooseNotCount(opts) { return chooseNotCount(opts); }
function callNegateCount(opts) { return negateCount(opts); }
function callOrCount(opts) { return orCount(opts); }
function callAndCount(opts) { return andCount(opts); }

// A backedge keeps this callee out of the inliner and exercises the shared
// JIT truthiness helper for tagged values as well as the inline callers.
function chooseWithBackedge(value) {
  let i = 0;
  do { i++; } while (i < 2);
  return value ? 3 : 7;
}

function negateWithBackedge(value) {
  let i = 0;
  do { i++; } while (i < 2);
  return !value;
}

const calls = [callChooseCount, callChooseNotCount, callNegateCount, callOrCount, callAndCount];
const warm = { count: 1 };
for (let i = 0; i < 600; i++) {
  chooseCount(warm); chooseNotCount(warm); negateCount(warm); orCount(warm); andCount(warm);
  for (const call of calls) call(warm);
}

const noCoercion = {
  [Symbol.toPrimitive]() { throw new Error('truthiness must not coerce objects'); }
};
const generator = (function* () { yield 1; })();
const longPart = 'ab'.repeat(1000);
const rope = longPart + longPart;
let accumulated = '';
for (let i = 0; i < 100; i++) accumulated += longPart;
const nanView = new DataView(new ArrayBuffer(8));
const payloadNaNs = [];
for (let tag = 0n; tag <= 6n; tag++) {
  nanView.setBigUint64(0, 0x7ff0000000000001n | (tag << 47n), true);
  const value = nanView.getFloat64(0, true);
  assert.ok(Number.isNaN(value));
  payloadNaNs.push(value);
}
const counts = [
  0, -0, NaN, 1, -1, 0.5, -0.5, Number.MIN_VALUE, -Number.MIN_VALUE,
  Number.MAX_VALUE, -Number.MAX_VALUE, Infinity, -Infinity, ...payloadNaNs,
  false, true, undefined, null, '', 'x', 'é𝄞', 'x'.repeat(1000), rope, accumulated,
  0n, 1n, -1n, 1n << 100n, Symbol('truthy'),
  {}, Object.create(null), [], function () {}, Math.abs, Promise.resolve(1),
  generator, noCoercion, new Proxy(noCoercion, {
    get() { throw new Error('truthiness must not invoke proxy traps'); }
  })
];
for (let round = 0; round < 50; round++) {
  for (const count of counts) {
    const opts = { count };
    assert.strictEqual(chooseWithBackedge(count), count ? 3 : 7);
    assert.strictEqual(negateWithBackedge(count), !count);
    assert.strictEqual(callChooseCount(opts), count ? 3 : 7);
    assert.strictEqual(callChooseNotCount(opts), count ? 3 : 7);
    assert.strictEqual(callNegateCount(opts), !count);
    assert.ok(Object.is(callOrCount(opts), count || 'fallback'));
    assert.ok(Object.is(callAndCount(opts), count && 'taken'));
  }
}

// A helper or a fallback must not replay the property read after an effect.
for (const count of counts) {
  for (const call of calls) {
    let reads = 0;
    const opts = { get count() { reads++; return count; } };
    call(opts);
    assert.strictEqual(reads, 1);
  }
}
console.log('PASS inline truthiness for numbers, tagged values and effectful reads');
