const assert = require('node:assert');
function read(array, index) { return array[index | 0]; }
function write(array, index, value) { array[index | 0] = value; }
function generic(array, index) { return array[index]; }
const a = [10, 20, 30];
for (let i = 0; i < 20000; i++) {
  assert.strictEqual(read(a, i % 3), (i % 3 + 1) * 10);
  write(a, i % 3, (i % 3 + 1) * 10);
}
// Growing the logical length must invalidate the dense capacity proof.
a.length = 0xffffffff;
assert.strictEqual(read(a, 1000000), undefined);
assert.strictEqual(read(a, -1), undefined);
assert.strictEqual(generic(a, 0xffffffff), undefined);
assert.strictEqual(generic(a, 2 ** 32), undefined);
assert.strictEqual(read(a, 0), 10);
write(a, 1000000, 77);
assert.strictEqual(read(a, 1000000), 77);
write(a, -1, 99);
assert.strictEqual(read(a, -1), 99);
a.length = 3;
assert.strictEqual(read(a, 1000000), undefined);
for (let i = 0; i < 20000; i++) assert.strictEqual(read(a, i % 3), (i % 3 + 1) * 10);
for (let i = 3; i < 1000; i++) a.push(i);
assert.strictEqual(read(a, 999), 999);
Object.defineProperty(a, 'length', { value: 10000000 });
assert.strictEqual(read(a, 9999999), undefined);
assert.strictEqual(read(a, 999), 999);
console.log('PASS JIT bounds after dense growth, sparse length changes and truncation');
