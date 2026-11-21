const assert = require('node:assert');

function read(o, source$) { with (o) { return eval(source$); } }
function probe(o) { with (o) { return [typeof own, typeof inherited, typeof deep, typeof missing]; } }
function call(o) { with (o) { return method(); } }
function write(o, value) { with (o) { inherited = value; } return o; }

var missing;
const deepProto = { deep: 3, method() { return this; } };
const midProto = Object.create(deepProto, { inherited: { value: 2, writable: true, configurable: true, enumerable: true } });
const leaf = Object.create(midProto);
leaf.own = 1;

for (let round = 0; round < 4; round++) {
  assert.deepStrictEqual(probe(leaf), ['number', 'number', 'number', 'undefined']);
  assert.strictEqual(call(leaf), leaf);
  assert.strictEqual(read(leaf, 'deep'), 3);
  assert.strictEqual(read(Object.create(null), 'typeof deep'), 'undefined');
  assert.strictEqual(read(Object.create(Object.create(null, { deep: { value: 'bare' } })), 'deep'), 'bare');
  assert.strictEqual(read({}, 'typeof hasOwnProperty'), 'function');
  assert.strictEqual(read({}, 'toString') , Object.prototype.toString);
  assert.strictEqual(read([], 'typeof push'), 'function');
  assert.strictEqual(read(Object.create({ get g() { return 'getter'; } }), 'g'), 'getter');
  assert.strictEqual(read(Object.create(new Proxy({}, { has: (t, k) => k === 'viaProxy', get: (t, k) => (k === 'viaProxy' ? 'proxied' : undefined) })), 'viaProxy'), 'proxied');
  assert.strictEqual(read(Object.create({ [Symbol.unscopables]: { hidden: true }, hidden: 1 }), 'typeof hidden'), 'undefined');
  assert.strictEqual(read(Object.create(function named() {}), 'name'), 'named');
}

const target = Object.create(midProto);
assert.strictEqual(Object.prototype.hasOwnProperty.call(write(target, 9), 'inherited'), true);
assert.strictEqual(target.inherited, 9);
assert.strictEqual(midProto.inherited, 2);

const swapped = Object.create({ late: 'first' });
assert.strictEqual(read(swapped, 'late'), 'first');
Object.setPrototypeOf(swapped, { late: 'second' });
assert.strictEqual(read(swapped, 'late'), 'second');
Object.setPrototypeOf(swapped, null);
assert.strictEqual(read(swapped, 'typeof late'), 'undefined');

console.log('ok');
