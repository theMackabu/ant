'use strict';
const assert = require('node:assert');

const holes = (array) => Array.from({ length: array.length }, (_, i) => i in array);

{
  let array = [1, , 3, , 5].copyWithin(0, 1);
  assert.deepStrictEqual(holes(array), [false, true, false, true, true]);
  assert.deepStrictEqual([array[1], array[3], array[4]], [3, 5, 5]);
  array = [1, , 3, , 5].copyWithin(1, 0);
  assert.deepStrictEqual(holes(array), [true, true, false, true, false]);
  array = [1, 2, , ,].copyWithin(0, 2);
  assert.deepStrictEqual(holes(array), [false, false, false, false]);
  array = [1, 2, 3, 4, 5].copyWithin('1', '0', '2');
  assert.deepStrictEqual(array, [1, 1, 2, 4, 5]);
  assert.deepStrictEqual([1, 2, 3, 4, 5].copyWithin(0, 1, Infinity), [2, 3, 4, 5, 5]);
  assert.deepStrictEqual([1, 2, 3, 4, 5].copyWithin(NaN, 1.9), [2, 3, 4, 5, 5]);
  assert.deepStrictEqual([1, 2, 3, 4, 5].copyWithin(-2, 0, -1), [1, 2, 3, 1, 2]);
  assert.deepStrictEqual([1, 2, 3].copyWithin(0, undefined, undefined), [1, 2, 3]);

  const like = Array.prototype.copyWithin.call({ length: 3, 0: 'a', 2: 'c' }, 0, 1);
  assert.deepStrictEqual(like, { 1: 'c', 2: 'c', length: 3 });
  assert.strictEqual(0 in like, false);

  const inherited = [1, , 3];
  Object.setPrototypeOf(inherited, Object.assign(Object.create(Array.prototype), { 1: 'P' }));
  inherited.copyWithin(0, 1);
  assert.deepStrictEqual([inherited[0], inherited[1], Object.hasOwn(inherited, 0)], ['P', 3, true]);

  assert.throws(() => Object.freeze([1, 2, 3]).copyWithin(0, 1), TypeError);

  const sealed = Object.seal([1, , 3]);
  assert.throws(() => sealed.copyWithin(0, 1), TypeError);
  assert.strictEqual(0 in sealed, true);
  const closed = Object.preventExtensions([1, , 3]);
  assert.throws(() => closed.copyWithin(1, 0), TypeError);
  assert.strictEqual(1 in closed, false);
  assert.deepStrictEqual(Object.seal([1, 2, 3]).copyWithin(0, 1), [2, 3, 3]);

  const shrunk = [1, 2, 3, 4, 5, 6, 7, 8];
  shrunk.copyWithin(0, { valueOf() { shrunk.length = 2; return 4; } });
  assert.deepStrictEqual(holes(shrunk), [false, false]);
}

console.log('ok');
