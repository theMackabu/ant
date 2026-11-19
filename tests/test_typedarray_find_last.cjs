const assert = require('node:assert');

{
  const bytes = new Uint8Array([1, 2, 3, 4]);
  const seen = [];
  assert.deepStrictEqual([bytes.findLast((x) => x < 3), bytes.findLastIndex((x) => x < 3), bytes.findLast((x) => x > 9), bytes.findLastIndex((x) => x > 9)], [2, 1, undefined, -1]);
  bytes.findLast(function (value, index, array) { seen.push(index, array === bytes, this.t); return false; }, { t: 7 });
  assert.deepStrictEqual(seen, [3, true, 7, 2, true, 7, 1, true, 7, 0, true, 7]);
  assert.throws(() => bytes.findLast(1), TypeError);
  assert.strictEqual(new Uint8Array([1, 2]).toLocaleString(), '1,2');
  assert.strictEqual(new Uint8Array(0).toLocaleString(), '');
  assert.strictEqual(new Float64Array([1.5]).toLocaleString(), (1.5).toLocaleString());
  const join = Array.prototype.join;
  Array.prototype.join = () => 'patched';
  try { assert.strictEqual(new Uint8Array([1, 2]).toLocaleString(), '1,2'); } finally { Array.prototype.join = join; }
}

console.log('ok');
