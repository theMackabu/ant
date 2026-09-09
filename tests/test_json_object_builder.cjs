const assert = require('node:assert');

const records = JSON.parse('[{"a":1,"b":2},{"a":3,"b":4},{"b":5,"a":6},{"a":7,"a":8,"b":9}]');
assert.deepStrictEqual(records, [{ a: 1, b: 2 }, { a: 3, b: 4 }, { b: 5, a: 6 }, { a: 8, b: 9 }]);
assert.deepStrictEqual(Object.keys(records[2]), ['b', 'a']);
assert.deepStrictEqual(Object.keys(records[3]), ['a', 'b']);
Object.defineProperty(records[0], 'a', { get() { return 99; } });
assert.strictEqual(records[1].a, 3);

const special = JSON.parse('[{"__proto__":{"polluted":true},"a\\u0000b":1,"10":10,"2":2},{"__proto__":null,"a\\u0000b":2,"10":20,"2":4}]');
assert.strictEqual(Object.getPrototypeOf(special[0]), Object.prototype);
assert.strictEqual(Object.prototype.hasOwnProperty.call(special[0], '__proto__'), true);
assert.strictEqual(special[0].polluted, undefined);
assert.strictEqual(special[0]['a\0b'], 1);
assert.deepStrictEqual(Object.keys(special[1]), ['2', '10', '__proto__', 'a\0b']);

const wide = {};
for (let i = 0; i < 160; i++) wide['key' + i] = { i, text: 'value' + i };
const input = JSON.stringify(Array.from({ length: 300 }, () => wide));
const parsed = JSON.parse(input);
assert.strictEqual(parsed[299].key159.i, 159);
assert.strictEqual(parsed[0].key159.text, 'value159');
delete parsed[0].key1;
assert.strictEqual(parsed[1].key1.i, 1);
parsed[0].key1 = 'new';
assert.strictEqual(Object.keys(parsed[0]).at(-1), 'key1');

const duplicateWide = '{' + Array.from({ length: 50 }, (_, i) => '"p' + i + '":' + i).join(',') + ',"p0":-0.0}';
const duplicate = JSON.parse(duplicateWide);
assert.strictEqual(Object.is(duplicate.p0, -0), true);
assert.strictEqual(Object.keys(duplicate).length, 50);
assert.strictEqual(Object.keys(duplicate)[0], 'p0');

const visits = [];
const revived = JSON.parse('[{"a":1,"b":2},{"a":3,"b":4}]', function(key, value) {
  if (key === 'a') { visits.push(value); return undefined; }
  if (key === 'b') return value * 2;
  return value;
});
assert.deepStrictEqual(visits, [1, 3]);
assert.deepStrictEqual(revived, [{ b: 4 }, { b: 8 }]);
assert.deepStrictEqual(JSON.parse('[{},null,{},[],{"a":1},0,{"a":2}]'), [{}, null, {}, [], { a: 1 }, 0, { a: 2 }]);
console.log('PASS JSON layouts, duplicates, special keys, revivers and GC');

// Records below the private-layout width share transitions across parses.
const medium = '{' + Array.from({ length: 100 }, (_, i) => '"m' + i + '":' + i).join(',') + '}';
const first = JSON.parse(medium);
const second = JSON.parse(medium);
assert.deepStrictEqual(first, second);
assert.strictEqual(Object.keys(first)[99], 'm99');
delete first.m50;
assert.strictEqual(second.m50, 50);
first.m50 = 'later';
assert.strictEqual(Object.keys(first).at(-1), 'm50');
assert.strictEqual(Object.keys(second)[50], 'm50');
console.log('PASS medium JSON records share layouts safely');

// Bulk layouts are reused across standalone parses. Mutations and revivers
// must detach metadata without affecting other records or future cache hits.
for (const count of [127, 128, 256, 500]) {
  const pairs = Array.from({ length: count }, (_, i) => '"cache' + i + '":' + i);
  const text = '{' + pairs.join(',') + '}';
  const a = JSON.parse(text);
  const b = JSON.parse(text);
  delete a.cache1;
  Object.defineProperty(a, 'cache2', { get() { return 999; } });
  Object.defineProperty(a, 'cache3', { writable: false });
  a.extra = 'new';
  a[Symbol('extra')] = 1;
  assert.strictEqual(b.cache1, 1);
  assert.strictEqual(b.cache2, 2);
  assert.strictEqual(Object.getOwnPropertyDescriptor(b, 'cache3').writable, true);
  const c = JSON.parse(text);
  assert.deepStrictEqual(c, b);
  assert.strictEqual(c.extra, undefined);
  assert.strictEqual(Reflect.ownKeys(c).length, count);

  const reversed = JSON.parse('{' + pairs.slice().reverse().join(',') + '}');
  assert.strictEqual(Object.keys(reversed)[0], 'cache' + (count - 1));
  const duplicate = JSON.parse('{' + pairs.join(',') + ',"cache0":-0.0}');
  assert.strictEqual(Object.is(duplicate.cache0, -0), true);
  assert.strictEqual(Object.keys(duplicate).length, count);
  assert.strictEqual(Object.keys(duplicate)[0], 'cache0');

  const revived = JSON.parse(text, (key, value) => key === 'cache1' ? undefined : value);
  assert.strictEqual('cache1' in revived, false);
  assert.strictEqual(JSON.parse(text).cache1, 1);
  Object.freeze(b);
  const d = JSON.parse(text);
  d.cache0 = 'mutable';
  assert.strictEqual(d.cache0, 'mutable');
}

const bulkSpecial = '{"__proto__":null,"a\\u0000b":1,"10":10,"2":2,' +
  Array.from({ length: 128 }, (_, i) => '"special' + i + '":' + i).join(',') + '}';
for (let i = 0; i < 3; i++) {
  const record = JSON.parse(bulkSpecial);
  assert.strictEqual(Object.getPrototypeOf(record), Object.prototype);
  assert.strictEqual(record.__proto__, null);
  assert.strictEqual(record['a\0b'], 1);
  assert.deepStrictEqual(Object.keys(record).slice(0, 4), ['2', '10', '__proto__', 'a\0b']);
}

// Nested bulk objects can evict the parent's cached layout while values are
// being filled. The parent's rooted object must keep that layout alive.
const nested = {};
for (let i = 0; i < 128; i++) {
  const child = {};
  for (let j = 0; j < 128; j++) child['child' + i + '_' + j] = j;
  nested['parent' + i] = child;
}
const nestedText = JSON.stringify(nested);
for (let i = 0; i < 2; i++) assert.deepStrictEqual(JSON.parse(nestedText), nested);
console.log('PASS cached bulk JSON layouts preserve mutation and recursive parsing semantics');
