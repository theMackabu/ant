const assert = require('node:assert');

const source = Buffer.from('HTTP/1.1 200 OK\r\nX-Test: yes\r\n\r\nbody');
const FastBuffer = Buffer[Symbol.species];

assert.strictEqual(typeof FastBuffer, 'function');

const status = new FastBuffer(source.buffer, source.byteOffset + 13, 2);
assert(Buffer.isBuffer(status));
assert.strictEqual(status.toString(), 'OK');

const shared = Buffer.from(source.buffer, source.byteOffset + 17, 6);
assert(Buffer.isBuffer(shared));
assert.strictEqual(shared.toString(), 'X-Test');

source[17] = 'Y'.charCodeAt(0);
assert.strictEqual(shared.toString(), 'Y-Test');
assert.strictEqual(source.utf8Slice(13, 15), 'OK');
assert.strictEqual(Buffer.from('aéz').utf8Slice(1, 3), 'é');
assert.strictEqual(Buffer.from([0x61, 0xc3, 0x28, 0x62]).utf8Slice(0, 4), 'a�(b');
assert(Buffer.isBuffer(Buffer.allocUnsafeSlow(4)));

console.log('buffer:species-arraybuffer:ok');
