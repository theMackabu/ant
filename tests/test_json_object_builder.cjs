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
