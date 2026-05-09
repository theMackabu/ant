const assert = require('node:assert');

const headers = Buffer.from('abc\r\n\r\ndef');
assert.strictEqual(headers.indexOf('\r\n\r\n'), 3);
assert.strictEqual(headers.indexOf(Buffer.from('\r\n\r\n')), 3);
assert.strictEqual(headers.indexOf(new Uint8Array([13, 10, 13, 10])), 3);
assert.strictEqual(headers.indexOf(0x0d), 3);

const repeated = Buffer.from('abcabc');
assert.strictEqual(repeated.indexOf('abc', 1), 3);
assert.strictEqual(repeated.indexOf('abc', -3), 3);
assert.strictEqual(repeated.indexOf('', 99), 6);
assert.strictEqual(repeated.indexOf('', -99), 0);

assert.strictEqual(Buffer.from('aabbcc', 'hex').indexOf('bb', 'hex'), 1);
assert.strictEqual(Buffer.from('aabbcc', 'hex').indexOf('bb', 0, 'hex'), 1);
assert.strictEqual(Buffer.from('aabbcc', 'hex').indexOf('bbz', 0, 'hex'), 1);
assert.strictEqual(Buffer.from('aabbcc', 'hex').indexOf('b', 0, 'hex'), 0);
assert.strictEqual(Buffer.from('a\0b\0').indexOf('b', 0, 'ucs2'), 2);
assert.strictEqual(Buffer.from([0xe9, 0]).indexOf('é', 0, 'ucs2'), 0);
assert.deepStrictEqual([...Buffer.from('é', 'ucs2')], [0xe9, 0]);
assert.strictEqual(Buffer.from([0x81, 0x7e]).readUInt16BE(0), 33150);

const shortFrameHeader = Buffer.alloc(2);
assert.strictEqual(shortFrameHeader.writeUInt16BE(0x817e, 0), 2);
assert.deepStrictEqual([...shortFrameHeader], [0x81, 0x7e]);

const longFrameHeader = Buffer.alloc(10);
assert.strictEqual(longFrameHeader.writeUIntBE(0x010203040506, 4, 6), 10);
assert.deepStrictEqual([...longFrameHeader.slice(4)], [1, 2, 3, 4, 5, 6]);

function assertThrowsTypeError(fn) {
  try {
    fn();
  } catch (error) {
    assert.strictEqual(error instanceof TypeError, true);
    return;
  }
  assert.fail('expected TypeError');
}

assertThrowsTypeError(() => Buffer.from('abc').indexOf({}));
assertThrowsTypeError(() => Buffer.from('abc').indexOf('a', 0, 'not-an-encoding'));

console.log('buffer:indexof:ok');
