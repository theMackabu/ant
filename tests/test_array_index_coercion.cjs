'use strict';
const assert = require('node:assert');

const message = (run) => {
  try {
    run();
  } catch (error) {
    return `${error.constructor.name}: ${error.message}`;
  }
  return 'no throw';
};

{
  assert.deepStrictEqual([1, 2, 3, 4].fill(7, '1', '2'), [1, 7, 3, 4]);
  assert.deepStrictEqual([1, 2, 3, 4].fill(7, -Infinity, Infinity), [7, 7, 7, 7]);
  assert.deepStrictEqual([1, 2, 3, 4].fill(7, undefined, 2), [7, 7, 3, 4]);
  assert.deepStrictEqual([1, 2, 3, 4].fill(7, NaN, -1), [7, 7, 7, 4]);
  assert.deepStrictEqual(Array.prototype.fill.call({ length: 3, 0: 'a' }, 7, '1', '2'), { 0: 'a', 1: 7, length: 3 });

  assert.strictEqual([5, 1, 3].indexOf(3, '1'), 2);
  assert.strictEqual([5, 1, 3].indexOf(5, '1'), -1);
  assert.strictEqual([5, 1, 3].indexOf(3, -100), 2);
  assert.strictEqual([5, 1, 3].indexOf(3, 100), -1);
  assert.strictEqual([5, 1, 3].indexOf(3, Infinity), -1);
  assert.strictEqual([5, 1, 3].lastIndexOf(5, undefined), 0);
  assert.strictEqual([5, 1, 3].lastIndexOf(1, undefined), -1);
  assert.strictEqual([5, 1, 3].lastIndexOf(3, -100), -1);
  assert.strictEqual([5, 1, 3].lastIndexOf(3, '1'), -1);
  assert.strictEqual([5, 1, 3].lastIndexOf(3, 100), 2);
  assert.strictEqual([5, 1, 3].lastIndexOf(3), 2);

  assert.throws(() => [1, 2].with(5, 'x'), RangeError);
  assert.throws(() => [1, 2].with(-3, 'x'), RangeError);
  assert.throws(() => [].with(0, 'x'), RangeError);
  assert.deepStrictEqual([1, 2].with('1', 'x'), [1, 'x']);
  assert.deepStrictEqual([1, 2].with(-1, 'x'), [1, 'x']);
  assert.deepStrictEqual([1, 2].with(0), [undefined, 2]);

  const sealed = Object.seal([3, 1, 2]);
  assert.deepStrictEqual(sealed.splice(undefined, undefined), []);
  assert.deepStrictEqual(sealed, [3, 1, 2]);
  assert.deepStrictEqual([1, 2, 3].splice('1', '1'), [2]);
  assert.deepStrictEqual([1, 2, 3].splice(-Infinity, Infinity), [1, 2, 3]);
  assert.deepStrictEqual([1, 2, 3].splice(1, undefined), []);
  assert.deepStrictEqual([1, 2, 3].splice(undefined), [1, 2, 3]);
}

assert.match(message(() => [1, 2].with(9, 1)), /^RangeError: Invalid index/);

for (const index of [1n, { valueOf: () => 1n }]) {
  assert.throws(() => [1, 2].fill(0, index), TypeError);
  assert.throws(() => [1, 2].indexOf(1, index), TypeError);
  assert.throws(() => [1, 2].lastIndexOf(1, index), TypeError);
  assert.throws(() => [1, 2].splice(index), TypeError);
  assert.throws(() => [1, 2].copyWithin(index), TypeError);
  assert.throws(() => [1, 2].with(index, 0), TypeError);
}

{
  const base = () => [0, 1, 2, 3, 4, 5, 6, 7];
  const edges = [
    0, -0, 1, -1, 7, 8, 9, -7, -8, -9, 0.5, -0.5, 1.9, -1.9, 7.999, -8.5,
    2147483647, 2147483648, -2147483648, -2147483649, 4294967296, -4294967296,
    2 ** 53, -(2 ** 53), 1e300, -1e300, Infinity, -Infinity, NaN,
  ];
  const clamp = (value, length) => {
    const whole = Number.isNaN(value) ? 0 : Math.trunc(value);
    return whole < 0 ? Math.max(length + whole, 0) : Math.min(whole, length);
  };

  for (const a of edges) {
    for (const b of edges) {
      const start = clamp(a, 8), end = clamp(b, 8);
      const filled = base();
      for (let i = start; i < end; i++) filled[i] = 'x';
      assert.deepStrictEqual(base().fill('x', a, b), filled, `fill ${a} ${b}`);

      const removed = Math.min(Math.max(Number.isNaN(b) ? 0 : Math.trunc(b), 0), 8 - start);
      const spliced = base();
      const taken = spliced.splice(a, b);
      assert.deepStrictEqual(taken, base().slice(start, start + removed), `splice ${a} ${b}`);
      assert.deepStrictEqual(spliced, [...base().slice(0, start), ...base().slice(start + removed)], `splice rest ${a} ${b}`);
    }

    const from = clamp(a, 8);
    assert.strictEqual(base().indexOf(7, a), from <= 7 ? 7 : -1, `indexOf ${a}`);
    assert.strictEqual(base().indexOf(0, a), from === 0 ? 0 : -1, `indexOf first ${a}`);

    const whole = Number.isNaN(a) ? 0 : Math.trunc(a);
    const last = whole < 0 ? 8 + whole : Math.min(whole, 7);
    assert.strictEqual(base().lastIndexOf(0, a), last >= 0 ? 0 : -1, `lastIndexOf ${a}`);
    assert.strictEqual(base().lastIndexOf(7, a), last >= 7 ? 7 : -1, `lastIndexOf last ${a}`);

    const at = whole < 0 ? 8 + whole : whole;
    if (at < 0 || at >= 8) assert.throws(() => base().with(a, 'x'), RangeError, `with ${a}`);
    else {
      const replaced = base();
      replaced[at] = 'x';
      assert.deepStrictEqual(base().with(a, 'x'), replaced, `with ${a}`);
    }

    const moved = base();
    const target = clamp(a, 8);
    for (let i = 0; i < Math.min(4, 8 - target); i++) moved[target + i] = base()[4 + i];
    assert.deepStrictEqual(base().copyWithin(a, 4), moved, `copyWithin ${a}`);
  }

  const huge = { length: 2 ** 53 - 1 };
  assert.strictEqual(Array.prototype.indexOf.call(huge, 1, Infinity), -1);
  assert.strictEqual(Array.prototype.indexOf.call(huge, 1, 2 ** 53), -1);
  assert.strictEqual(Array.prototype.fill.call(huge, 1, Infinity), huge);
  assert.strictEqual(Array.prototype.copyWithin.call(huge, Infinity, 0), huge);

  const sparse = [];
  sparse[4000] = 1;
  sparse.push(2, 3);
  assert.deepStrictEqual([sparse.length, sparse[4001], sparse[4002], 3999 in sparse], [4003, 2, 3, false]);
  const wide = [1];
  wide.length = 5000;
  wide.push(2, 3);
  assert.deepStrictEqual([wide.length, wide[5000], wide[5001], 4999 in wide], [5002, 2, 3, false]);
  assert.throws(() => Object.freeze([1]).push(), TypeError);
  assert.throws(() => Object.freeze([1]).push(2), TypeError);
}

console.log('ok');
