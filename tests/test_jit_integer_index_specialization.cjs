const assert = require('node:assert');
function read(a, i) { return a[i | 0]; }
function write(a, i, value) { a[i | 0] = value; }
function unsignedRead(a, i) { return a[i >>> 0]; }
function unsignedWrite(a, i, value) { a[i >>> 0] = value; }
const array = [10, 20, 30];
for (let i = 0; i < 1000; i++) {
  assert.strictEqual(read(array, 1.75), 20);
  write(array, 1.75, 20);
  assert.strictEqual(unsignedRead(array, 1), 20);
  unsignedWrite(array, 1, 20);
}
array[-1] = 41;
array[4294967295] = 42;
assert.strictEqual(read(array, -1), 41);
assert.strictEqual(unsignedRead(array, -1), 42);
write(array, -1, 51);
unsignedWrite(array, -1, 52);
assert.strictEqual(array[-1], 51);
assert.strictEqual(array[4294967295], 52);
assert.strictEqual(array.length, 3);
const proto = Object.create(Array.prototype);
Object.defineProperty(proto, '0', { get() { return 73; } });
const holey = [, 2, 3];
Object.setPrototypeOf(holey, proto);
assert.strictEqual(read(holey, 0), 73);
console.log('PASS integer indices retain named-property and hole semantics');
