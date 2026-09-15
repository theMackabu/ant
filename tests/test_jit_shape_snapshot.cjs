const assert = require('node:assert');

function readOwn(obj) { return obj.target; }
function readInline(obj) { return readOwn(obj); }
function readOptional(obj) { return obj?.target; }
function readOverflow(obj) { return obj.tail; }

const first = { target: 41 };
const second = { target: 42 };
const overflow = { a: 0, b: 1, c: 2, d: 3, e: 4, f: 5, g: 6, tail: 43 };
for (let i = 0; i < 20000; i++) {
  assert.strictEqual(readInline(first), 41);
  assert.strictEqual(readOptional(second), 42);
  assert.strictEqual(readOverflow(overflow), 43);
}

first.target = 44;
assert.strictEqual(readInline(first), 44);
Object.defineProperty(first, 'target', { get() { return 45; } });
for (let i = 0; i < 1000; i++) {
  assert.strictEqual(readInline(first), 45);
  assert.strictEqual(readInline(second), 42);
}
delete second.target;
Object.setPrototypeOf(second, { target: 46 });
assert.strictEqual(readOptional(second), 46);
second.target = 47;
assert.strictEqual(readOptional(second), 47);
assert.strictEqual(readOptional(null), undefined);
assert.strictEqual(readOptional(undefined), undefined);

delete overflow.c;
delete overflow.e;
delete overflow.a;
delete overflow.b;
delete overflow.d;
delete overflow.f;
assert.strictEqual(readOverflow(overflow), 43);
Object.defineProperty(overflow, 'tail', { get() { return 48; } });
assert.strictEqual(readOverflow(overflow), 48);

let traps = 0;
const proxy = new Proxy({ target: 49 }, {
  get(target, key) { traps++; return target[key] + 1; }
});
assert.strictEqual(readInline(proxy), 50);
assert.strictEqual(traps, 1);

const arr = [];
const fn = function () {};
const promise = Promise.resolve();
for (const obj of [arr, fn, promise, Object.create(null)]) {
  obj.target = 51;
  for (let i = 0; i < 1000; i++) assert.strictEqual(readInline(obj), 51);
  delete obj.target;
  assert.strictEqual(readInline(obj), undefined);
}
for (let i = 0; i < 10000; i++) {
  assert.strictEqual(readInline({ padding: i, target: i }), i);
  assert.strictEqual(readInline({ target: -i }), -i);
}
assert.strictEqual(readInline(first), 45);
console.log('PASS retained shape reads survive writes, descriptors, deletion, compaction and cache replacement');
