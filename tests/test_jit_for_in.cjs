const assert = require('node:assert');

const ROUNDS = 3000;

function keysOf(obj) {
  const out = [];
  for (const key in obj) out.push(key);
  return out.join();
}

function sumValues(obj) {
  let sum = 0;
  for (const key in obj) sum += obj[key];
  return sum;
}

const proto = { inherited: 1 };
const child = Object.create(proto);
child.own = 2;
Object.defineProperty(child, 'hidden', { value: 3, enumerable: false });

for (let i = 0; i < ROUNDS; i++) {
  assert.strictEqual(keysOf({ a: 1, b: 2, [i]: 3 }), i + ',a,b');
  assert.strictEqual(keysOf(child), 'own,inherited');
  assert.strictEqual(keysOf([7, 8]), '0,1');
  assert.strictEqual(keysOf('hi'), '0,1');
  assert.strictEqual(keysOf(null), '');
  assert.strictEqual(keysOf(undefined), '');
  assert.strictEqual(keysOf(i), '');
  assert.strictEqual(sumValues({ x: i, y: 1 }), i + 1);
}
assert.strictEqual(sumValues({ x: 'a', y: 'b' }), '0ab');

const throwing = new Proxy({}, { ownKeys() { throw new Error('no keys'); } });

function guarded(obj) {
  try {
    for (const key in obj) return key;
    return 'empty';
  } catch (e) {
    return e.message;
  }
}

function unguarded(obj) {
  for (const key in obj) return key;
  return 'empty';
}

for (let i = 0; i < ROUNDS; i++) {
  assert.strictEqual(guarded(i & 1 ? throwing : { k: i }), i & 1 ? 'no keys' : 'k');
  assert.strictEqual(unguarded({}), 'empty');
}
assert.throws(() => unguarded(throwing), /no keys/);

function addDuring(obj) {
  const seen = [];
  for (const key in obj) {
    seen.push(key);
    obj['extra' + seen.length] = true;
  }
  return seen.join();
}
for (let i = 0; i < ROUNDS; i++) assert.strictEqual(addDuring({ a: 1, b: 2 }), 'a,b');

console.log('jit for-in: ok');
