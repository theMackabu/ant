const assert = require('node:assert');

const proto = Object.getPrototypeOf(Uint8Array.prototype);
for (const name of ['length', 'byteLength', 'byteOffset']) {
  const descriptor = Object.getOwnPropertyDescriptor(proto, name);
  assert.strictEqual(typeof descriptor.get, 'function');
  assert.strictEqual(descriptor.set, undefined);
  assert.strictEqual(descriptor.enumerable, false);
  assert.strictEqual(descriptor.configurable, true);
  assert.throws(() => descriptor.get.call({}), error => error.name === 'TypeError');
}

for (const Constructor of [Uint8Array, Uint16Array, Float64Array, BigInt64Array]) {
  const backing = new ArrayBuffer(64);
  const view = new Constructor(backing, 8, 4);
  assert.strictEqual(view.length, 4);
  assert.strictEqual(view.byteLength, 4 * Constructor.BYTES_PER_ELEMENT);
  assert.strictEqual(view.byteOffset, 8);
  const transferred = backing.transfer();
  assert.strictEqual(transferred.byteLength, 64);
  assert.strictEqual(view.length, 0);
  assert.strictEqual(view.byteLength, 0);
  assert.strictEqual(view.byteOffset, 0);
  assert.strictEqual(view[0], undefined);
  assert.strictEqual(view.buffer, backing);
}

const shared = new Uint16Array(new SharedArrayBuffer(32), 4, 3);
assert.strictEqual(shared.length, 3);
assert.strictEqual(shared.byteLength, 6);
assert.strictEqual(shared.byteOffset, 4);

console.log('typedarray:detached-dimensions:ok');
