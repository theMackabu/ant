const assert = require('node:assert');

assert.deepStrictEqual(Object.keys(Buffer), ['poolSize', 'from', 'copyBytesFrom', 'of', 'alloc', 'allocUnsafe', 'allocUnsafeSlow', 'isBuffer', 'compare', 'isEncoding', 'concat', 'byteLength'], 'statics are listed in node order');
assert.deepStrictEqual([Object.getPrototypeOf(Buffer), Buffer.poolSize, Buffer.BYTES_PER_ELEMENT], [Uint8Array, 8192, 1], 'Buffer inherits from Uint8Array');

const of = Buffer.of(1, 256, -1, '7');
assert.deepStrictEqual([Buffer.isBuffer(of), [...of]], [true, [1, 0, 255, 7]], 'of truncates like a Uint8Array store');
assert.strictEqual(Buffer.of().length, 0);

const u16 = new Uint16Array([0x0102, 0x0304, 0x0506]);
const copy = Buffer.copyBytesFrom(u16);
u16[0] = 0;
assert.deepStrictEqual([Buffer.isBuffer(copy), [...copy]], [true, [...new Uint8Array(new Uint16Array([0x0102, 0x0304, 0x0506]).buffer)]], 'copyBytesFrom copies, not shares');
assert.deepStrictEqual([...Buffer.copyBytesFrom(new Uint16Array([1, 2, 3]), 1, 1)], [...new Uint8Array(new Uint16Array([2]).buffer)], 'offset and length count elements');
assert.deepStrictEqual([Buffer.copyBytesFrom(new Uint8Array(3), 5).length, Buffer.copyBytesFrom(new Uint8Array(3), 1, 9).length, Buffer.copyBytesFrom(new Uint8Array(0)).length], [0, 2, 0]);
assert.deepStrictEqual([...Buffer.copyBytesFrom(new Float32Array([1.5]))], [...new Uint8Array(new Float32Array([1.5]).buffer)]);
assert.throws(() => Buffer.copyBytesFrom([1, 2]), TypeError);
assert.throws(() => Buffer.copyBytesFrom(new Uint8Array(2), '1'), TypeError);
assert.throws(() => Buffer.copyBytesFrom(new Uint8Array(2), 1.5), RangeError);
assert.throws(() => Buffer.copyBytesFrom(new Uint8Array(2), 0, -1), RangeError);

console.log('buffer statics OK');
