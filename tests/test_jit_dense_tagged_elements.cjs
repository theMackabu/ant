const assert = require('node:assert');
function read(array, index) { return array[index]; }
function twice(array, index) { return array[index] === array[index]; }
const object = { kept: 42 };
const values = [object, 'text', Symbol('key'), 123n, undefined, null, true, () => 42];
for (let i = 0; i < 2000; i++) {
  const index = i % values.length;
  assert.strictEqual(read(values, index), values[index]);
  assert.strictEqual(twice(values, index), true);
}
let calls = 0;
const proto = Object.create(Array.prototype);
Object.defineProperty(proto, '0', { get() { calls++; return { call: calls }; } });
const holey = [, 'tail'];
Object.setPrototypeOf(holey, proto);
assert.strictEqual(read(holey, 0).call, 1);
assert.strictEqual(twice(holey, 0), false);
assert.strictEqual(calls, 3);
const accessor = [];
Object.defineProperty(accessor, '0', { get() { return { call: ++calls }; } });
assert.strictEqual(twice(accessor, 0), false);
assert.strictEqual(calls, 5);
let traps = 0;
const proxy = new Proxy(values, { get(target, key) { traps++; return target[key]; } });
assert.strictEqual(twice(proxy, 0), true);
assert.strictEqual(traps, 2);
function readAcrossGetter(array, other) {
  const before = array[0];
  const ignored = other[0];
  return [before, array[0]];
}
const mutable = [object];
const effect = Object.defineProperty([], '0', { get() {
  mutable[0] = { changed: true };
  const churn = [];
  for (let i = 0; i < 60000; i++) churn.push({ nested: { i } });
  return 0;
} });
for (let i = 0; i < 1000; i++) readAcrossGetter(mutable, [0]);
mutable[0] = { kept: 'only the first read retains this object across GC' };
const pair = readAcrossGetter(mutable, effect);
assert.strictEqual(pair[0].kept, 'only the first read retains this object across GC');
assert.strictEqual(pair[1].changed, true);
// A removed element must not be mistaken for a cached tagged value.
delete values[0];
assert.strictEqual(read(values, 0), undefined);
console.log('PASS dense tagged elements, holes, accessors and cache invalidation');
