const assert = require('node:assert');

// (memory (export "memory") 1 4), plus size, grow, load and store functions.
const bytes = new Uint8Array([
  0, 97, 115, 109, 1, 0, 0, 0,
  1, 15, 3, 96, 0, 1, 127, 96, 1, 127, 1, 127, 96, 2, 127, 127, 0,
  3, 5, 4, 0, 1, 1, 2,
  5, 4, 1, 1, 1, 4,
  7, 39, 5,
  6, 109, 101, 109, 111, 114, 121, 2, 0,
  4, 115, 105, 122, 101, 0, 0,
  4, 103, 114, 111, 119, 0, 1,
  4, 108, 111, 97, 100, 0, 2,
  5, 115, 116, 111, 114, 101, 0, 3,
  10, 31, 4,
  4, 0, 63, 0, 11,
  6, 0, 32, 0, 64, 0, 11,
  7, 0, 32, 0, 45, 0, 0, 11,
  9, 0, 32, 0, 32, 1, 58, 0, 0, 11,
]);

for (const initial of [1, 0]) {
  const fixture = bytes.slice();
  fixture[36] = initial;
  const exports = new WebAssembly.Instance(new WebAssembly.Module(fixture)).exports;
  const memory = exports.memory;
  const initialBuffer = memory.buffer;
  const initialView = new Uint8Array(initialBuffer);
  const offsetView = initial ? new Uint16Array(initialBuffer, 8, 4) : null;
  assert.strictEqual(exports.size(), initial);
  if (initial) initialView[65535] = 42;

  // This is the host-side growth used by es-module-lexer for larger sources.
  assert.strictEqual(memory.grow(1), initial);
  assert.strictEqual(initialBuffer.byteLength, 0);
  assert.strictEqual(initialView.byteLength, 0);
  assert.strictEqual(initialView.length, 0);
  assert.strictEqual(initialView[0], undefined);
  if (offsetView) {
    assert.strictEqual(offsetView.byteLength, 0);
    assert.strictEqual(offsetView.length, 0);
    assert.strictEqual(offsetView.byteOffset, 0);
  }
  assert.notStrictEqual(memory.buffer, initialBuffer);
  assert.strictEqual(memory.buffer, memory.buffer);
  assert.strictEqual(memory.buffer.byteLength, (initial + 1) * 65536);
  assert.strictEqual(exports.size(), initial + 1);
  if (initial) assert.strictEqual(exports.load(65535), 42);

  const address = initial * 65536;
  const view = new Uint8Array(memory.buffer);
  assert.strictEqual(exports.load(address), 0);
  assert.strictEqual(view[view.length - 1], 0);
  view[address] = 73;
  assert.strictEqual(exports.load(address), 73);
  exports.store(address + 1, 91);
  assert.strictEqual(view[address + 1], 91);

  // Opcode growth and later host growth must use the same memory instance.
  assert.strictEqual(exports.grow(1), initial + 1);
  assert.strictEqual(memory.buffer.byteLength, (initial + 2) * 65536);
  assert.strictEqual(memory.grow(2 - initial), initial + 2);
  assert.strictEqual(exports.size(), 4);
  assert.strictEqual(exports.load(address), 73);
  assert.strictEqual(exports.load(address + 1), 91);
  const finalBuffer = memory.buffer;
  assert.strictEqual(finalBuffer.byteLength, 4 * 65536);

  assert.throws(() => memory.grow(1), error => error.name === 'RangeError');
  assert.strictEqual(memory.buffer, finalBuffer);
  assert.strictEqual(finalBuffer.byteLength, 4 * 65536);
  assert.strictEqual(exports.size(), 4);
  assert.strictEqual(exports.load(address), 73);
}

// es-module-lexer exports memory but does not use memory.size/memory.grow
// opcodes. The loader must preserve its 64 KiB pages and declared maximum.
const memoryOnly = new Uint8Array([
  0, 97, 115, 109, 1, 0, 0, 0,
  5, 4, 1, 1, 2, 10,
  7, 10, 1, 6, 109, 101, 109, 111, 114, 121, 2, 0,
]);
const exported = new WebAssembly.Instance(new WebAssembly.Module(memoryOnly)).exports.memory;
assert.strictEqual(exported.buffer.byteLength, 2 * 65536);
assert.strictEqual(exported.grow(6), 2);
assert.strictEqual(exported.buffer.byteLength, 8 * 65536);
assert.strictEqual(exported.grow(2), 8);
assert.throws(() => exported.grow(1), error => error.name === 'RangeError');

console.log('wasm:exported-memory-grow:ok');
