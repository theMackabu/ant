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

console.log('ok');
