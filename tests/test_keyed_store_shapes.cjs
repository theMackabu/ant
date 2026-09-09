const assert = require('node:assert');

// Computed-key stores stop sharing shape transitions after a few properties.
// Semantics must be identical either side of that boundary: order, values,
// deletion, re-adding, symbols, accessors and JSON round-trips.
function dictionary(prefix, count) {
  const object = {};
  for (let i = 0; i < count; i++) object[prefix + i] = i;
  return object;
}

for (const count of [1, 31, 32, 33, 64, 500]) {
  const a = dictionary('k', count);
  const b = dictionary('k', count);
  assert.deepStrictEqual(Object.keys(a), Array.from({ length: count }, (_, i) => 'k' + i));
  assert.deepStrictEqual(a, b);
  assert.strictEqual(a['k' + (count - 1)], count - 1);
  assert.strictEqual('k' + count in a, false);
  delete a.k0;
  assert.strictEqual('k0' in a, false);
  assert.strictEqual(b.k0, 0);
  a.k0 = 'again';
  assert.strictEqual(Object.keys(a).at(-1), 'k0');
  assert.strictEqual(Object.keys(b)[0], 'k0');
  a.named = true;
  assert.strictEqual(b.named, undefined);
  Object.defineProperty(a, 'k1', { get() { return 'getter'; } });
  assert.strictEqual(a.k1, 'getter');
  assert.strictEqual(b.k1, count > 1 ? 1 : undefined);
  assert.deepStrictEqual(JSON.parse(JSON.stringify(b)), b);
}

// Objects with distinct key sets never observe each other's properties.
const kept = [];
for (let j = 0; j < 300; j++) {
  const object = {};
  for (let i = 0; i < 100; i++) object['u' + j + '_' + i] = i;
  kept.push(object);
}
for (let j = 0; j < kept.length; j++) {
  assert.strictEqual(kept[j]['u' + j + '_99'], 99);
  assert.strictEqual('u' + ((j + 1) % kept.length) + '_0' in kept[j], false);
  assert.strictEqual(Object.keys(kept[j]).length, 100);
}

// Object.fromEntries builds dictionaries through the same policy.
const entries = Array.from({ length: 80 }, (_, i) => ['e' + i, i]);
const fromEntries = Object.fromEntries(entries);
assert.deepStrictEqual(Object.entries(fromEntries), entries);
const symbol = Symbol('s');
const mixed = dictionary('m', 40);
mixed[symbol] = 'symbol';
assert.strictEqual(mixed[symbol], 'symbol');
assert.strictEqual(Object.keys(mixed).length, 40);

// Named stores after a keyed prefix keep working and stay private to the object.
class Bag { constructor(seed) { for (let i = 0; i < 40; i++) this['b' + i] = seed + i; this.total = seed; } }
const bags = [new Bag(1), new Bag(2)];
assert.strictEqual(bags[0].total, 1);
assert.strictEqual(bags[1].total, 2);
assert.strictEqual(bags[1].b39, 41);
bags[0].extra = 'x';
assert.strictEqual(bags[1].extra, undefined);
// Warm numeric and symbol element stores independently, including fromEntries.
function fill(keys) {
  const object = {};
  for (let i = 0; i < keys.length; i++) object[keys[i]] = i;
  return object;
}
for (const keys of [
  Array.from({ length: 65 }, (_, i) => 50000 + i),
  Array.from({ length: 65 }, (_, i) => Symbol('key' + i))
]) {
  for (let run = 0; run < 100; run++) {
    const object = fill(keys);
    for (let i = 0; i < keys.length; i++) assert.strictEqual(object[keys[i]], i);
    assert.strictEqual(Reflect.ownKeys(object).length, keys.length);
  }
  const object = Object.fromEntries(keys.map((key, i) => [key, i]));
  for (let i = 0; i < keys.length; i++) assert.strictEqual(object[keys[i]], i);
}
console.log('PASS keyed store shapes preserve dictionary semantics');
