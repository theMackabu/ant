const assert = require('node:assert');

{
  const map = new Map();
  assert.deepStrictEqual([map.getOrInsert('a', 1), map.getOrInsert('a', 2), map.size], [1, 1, 1]);
  const log = [];
  assert.strictEqual(map.getOrInsertComputed('b', (key) => { log.push(key); return key + '!'; }), 'b!');
  assert.strictEqual(map.getOrInsertComputed('b', () => { log.push('again'); }), 'b!');
  assert.deepStrictEqual(log, ['b']);
  assert.strictEqual(map.getOrInsertComputed(-0, (key) => Object.is(key, 0)), true);
  assert.strictEqual(map.getOrInsertComputed('k', () => { map.set('k', 'inner'); return 'outer'; }), 'outer');
  assert.strictEqual(map.get('k'), 'outer');
  assert.throws(() => map.getOrInsertComputed('z', 1), TypeError);
  assert.throws(() => Map.prototype.getOrInsert.call({}, 1, 2), TypeError);
  const weak = new WeakMap();
  const key = {};
  assert.deepStrictEqual([weak.getOrInsert(key, 1), weak.getOrInsert(key, 2), weak.getOrInsertComputed(key, () => 3), weak.getOrInsertComputed({}, (k) => typeof k)], [1, 1, 1, 'object']);
  assert.throws(() => weak.getOrInsert(1, 2), TypeError);
  assert.throws(() => weak.getOrInsertComputed({}, 2), TypeError);
  assert.deepStrictEqual([Map.prototype.getOrInsert.length, Map.prototype.getOrInsertComputed.length, WeakMap.prototype.getOrInsert.length], [2, 2, 2]);
}

console.log('ok');
