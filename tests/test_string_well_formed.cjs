'use strict';

const assert = require('node:assert');

const units = s => Array.from({ length: s.length }, (_, i) => s.charCodeAt(i));

for (const name of ['isWellFormed', 'toWellFormed']) {
  const desc = Object.getOwnPropertyDescriptor(String.prototype, name);
  assert.strictEqual(typeof desc.value, 'function');
  assert.strictEqual(desc.enumerable, false);
  assert.strictEqual(desc.writable, true);
  assert.strictEqual(desc.configurable, true);
  assert.strictEqual(desc.value.length, 0);
  assert.strictEqual(desc.value.name, name);
  assert.throws(() => new desc.value(), TypeError);
  assert.throws(() => desc.value.call(null), TypeError);
  assert.throws(() => desc.value.call(undefined), TypeError);
  assert.throws(() => desc.value.call(Symbol('s')), TypeError);
  assert.throws(() => desc.value.call({ toString() { throw new RangeError('x'); } }), RangeError);
}
assert.ok(!Object.keys(String.prototype).includes('isWellFormed'));

assert.strictEqual(String.prototype.isWellFormed.call(42), true);
assert.strictEqual(String.prototype.toWellFormed.call(42), '42');
assert.strictEqual(String.prototype.isWellFormed.call({ toString() { return 'a\uD800'; } }), false);
assert.strictEqual(new String('a\uD800').isWellFormed(), false);

const hi = '\uD83D';
const lo = '\uDE00';
assert.strictEqual('ascii'.isWellFormed(), true);
assert.strictEqual('é€'.isWellFormed(), true);
assert.strictEqual((hi + lo).isWellFormed(), true);
assert.strictEqual(hi.isWellFormed(), false);
assert.strictEqual(lo.isWellFormed(), false);
assert.strictEqual((lo + hi).isWellFormed(), false);
assert.strictEqual(('x'.repeat(100) + hi).isWellFormed(), false);
assert.strictEqual(('x'.repeat(100) + hi + lo).isWellFormed(), true);
assert.strictEqual(['\uD800', '\uDC00'].join('').isWellFormed(), true);
assert.strictEqual(JSON.parse('"\\ud800\\udc00"').isWellFormed(), true);
assert.strictEqual(JSON.parse('"\\ud800"').isWellFormed(), false);
assert.strictEqual((hi + lo).slice(1).isWellFormed(), false);

assert.deepStrictEqual(units('a\uD800b'.toWellFormed()), [0x61, 0xfffd, 0x62]);
assert.deepStrictEqual(units((lo + hi).toWellFormed()), [0xfffd, 0xfffd]);
assert.deepStrictEqual(units(String.fromCharCode(0xd800, 0x41, 0xdfff).toWellFormed()), [0xfffd, 0x41, 0xfffd]);
assert.strictEqual((hi + lo).toWellFormed(), '\u{1F600}');
const wellFormed = 'abc€';
assert.strictEqual(wellFormed.toWellFormed(), wellFormed);

const ill = 'a\uD800';
for (let i = 0; i < 3; i++) {
  assert.strictEqual(ill.isWellFormed(), false);
  assert.strictEqual(ill.toWellFormed(), 'a�');
  assert.strictEqual(ill.toWellFormed().isWellFormed(), true);
}

const check = s => s.isWellFormed();
for (let i = 0; i < 2000; i++) check(i % 2 ? 'a\uD800' : 'ab');
for (let i = 0; i < 20; i++) {
  assert.strictEqual(check('a\uD800'), false);
  assert.strictEqual(check('ab'), true);
}

function measure(tail) {
  const s = '퟿'.repeat(1 << 20) + tail;
  let t = performance.now();
  const first = s.isWellFormed();
  const firstMs = performance.now() - t;
  t = performance.now();
  for (let i = 0; i < 2000; i++) assert.strictEqual(s.isWellFormed(), first);
  return { first, firstMs, restMs: performance.now() - t };
}

const isAnt = process.release.name === 'ant';
for (let i = 0; i < 3; i++) measure('x');
if (isAnt) for (const [tail, expected] of [['\uD800', false], ['\u{1F600}', true]]) {
  const { first, firstMs, restMs } = measure(tail);
  assert.strictEqual(first, expected);
  assert.ok(restMs < Math.max(firstMs, 0.05) * 100, `repeated isWellFormed is not cached: first ${firstMs}ms, 2000 more ${restMs}ms`);
}

console.log('string well-formed tests passed');
