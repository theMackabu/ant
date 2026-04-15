const assert = require('assert');

assert.equal(typeof ArrayBuffer.isView, 'function');

const buffer = new ArrayBuffer(16);
const u8 = new Uint8Array(buffer);
const dv = new DataView(buffer);

assert.equal(ArrayBuffer.isView(u8), true);
assert.equal(ArrayBuffer.isView(dv), true);
assert.equal(ArrayBuffer.isView(buffer), false);
assert.equal(ArrayBuffer.isView({}), false);
assert.equal(ArrayBuffer.isView(null), false);
assert.equal(ArrayBuffer.isView(undefined), false);

console.log('ArrayBuffer.isView regression checks passed');
