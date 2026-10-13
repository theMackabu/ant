// detached buffers and views over them must not be cloned or leak their old bytes
const assert = require('node:assert');

const isDataCloneError = e => e && e.name === 'DataCloneError';

function detached() {
  const buffer = new ArrayBuffer(8);
  const bytes = new Uint8Array(buffer);
  const view = new DataView(buffer);
  bytes[0] = 7;
  const moved = structuredClone(buffer, { transfer: [buffer] });
  return { buffer, bytes, view, moved };
}

const { buffer, bytes, view, moved } = detached();
assert.strictEqual(buffer.byteLength, 0);
assert.strictEqual(moved.byteLength, 8);
assert.strictEqual(new Uint8Array(moved)[0], 7);

assert.throws(() => structuredClone(buffer), isDataCloneError);
assert.throws(() => structuredClone(bytes), isDataCloneError);
assert.throws(() => structuredClone(view), isDataCloneError);
assert.throws(() => structuredClone({ nested: [bytes] }), isDataCloneError);

const live = new Uint8Array([1, 2, 3]);
assert.deepStrictEqual(Array.from(structuredClone(live)), [1, 2, 3]);

console.log('structured clone detached OK');
