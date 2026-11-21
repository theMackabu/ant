'use strict';
const assert = require('node:assert');

{
  const array = [];
  array.length = 4294967294;
  assert.throws(() => array.push('x', 1), RangeError);
  assert.deepStrictEqual([array.length, array[4294967294], array[4294967295]], [4294967295, 'x', 1]);

  const full = [];
  full.length = 4294967295;
  assert.throws(() => full.push('x'), RangeError);
  assert.deepStrictEqual([full.length, full[4294967295]], [4294967295, 'x']);

  const near = [1];
  near.length = 4294967290;
  assert.strictEqual(near.push('x', 'y'), 4294967292);
  assert.deepStrictEqual([near.length, near[4294967290], near[4294967291], near[0]], [4294967292, 'x', 'y', 1]);

  const sparse = [1, 2];
  sparse.length = 5000000;
  assert.strictEqual(sparse.push('end'), 5000001);
  assert.deepStrictEqual([sparse[5000000], sparse[2], 2 in sparse, sparse.length], ['end', undefined, false, 5000001]);

  const like = { length: 2 ** 53 - 1 };
  assert.throws(() => Array.prototype.push.call(like, 1), TypeError);
  assert.strictEqual(like.length, 2 ** 53 - 1);
  assert.strictEqual(Array.prototype.push.call(like), 2 ** 53 - 1);

  const dense = [];
  for (let i = 0; i < 5000; i++) assert.strictEqual(dense.push(i), i + 1);
  assert.strictEqual(dense[4999], 4999);
}

console.log('ok');
