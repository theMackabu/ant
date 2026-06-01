const assert = require('node:assert');

function assertThrowsName(fn, name) {
  let thrown;
  try {
    fn();
  } catch (error) {
    thrown = error;
  }
  assert(thrown, `expected ${name}`);
  assert.strictEqual(thrown.name, name);
}

const memory = new WebAssembly.Memory({ initial: 1, maximum: 2 });
assert(memory instanceof WebAssembly.Memory);
assert(memory.buffer instanceof ArrayBuffer);
assert.strictEqual(memory.buffer.byteLength, 65536);

const initialBuffer = memory.buffer;
assert.strictEqual(memory.buffer, initialBuffer);

const bytes = new Uint8Array(initialBuffer);
bytes[0] = 42;
bytes[65535] = 7;
assert.strictEqual(new Uint8Array(memory.buffer)[0], 42);
assert.strictEqual(new Uint8Array(memory.buffer)[65535], 7);

assert.strictEqual(memory.grow(1), 1);
assert.strictEqual(initialBuffer.byteLength, 0);
assert.strictEqual(bytes[0], undefined);
assert.notStrictEqual(memory.buffer, initialBuffer);
assert.strictEqual(memory.buffer, memory.buffer);
assert.strictEqual(memory.buffer.byteLength, 131072);

const grownBytes = new Uint8Array(memory.buffer);
assert.strictEqual(grownBytes[0], 42);
assert.strictEqual(grownBytes[65535], 7);
assert.strictEqual(grownBytes[65536], 0);
assertThrowsName(() => memory.grow(1), 'RangeError');

const empty = new WebAssembly.Memory({ initial: 0, maximum: 1 });
assert.strictEqual(empty.buffer.byteLength, 0);
assert.strictEqual(empty.grow(1), 0);
assert.strictEqual(empty.buffer.byteLength, 65536);

assertThrowsName(() => WebAssembly.Memory({ initial: 1 }), 'TypeError');
assertThrowsName(() => new WebAssembly.Memory(), 'TypeError');
assertThrowsName(() => new WebAssembly.Memory({ initial: -1 }), 'TypeError');
assertThrowsName(() => new WebAssembly.Memory({ initial: 2, maximum: 1 }), 'RangeError');
assertThrowsName(() => new WebAssembly.Memory({ initial: 1, shared: true }), 'TypeError');

console.log('wasm:memory-constructor:ok');
