const assert = require('node:assert');

const hello = Buffer.from('hello world');

// includes and lastIndexOf are Buffer-specific, not TypedArray.prototype's
assert.notStrictEqual(Buffer.prototype.includes, Uint8Array.prototype.includes);
assert.notStrictEqual(Buffer.prototype.lastIndexOf, Uint8Array.prototype.lastIndexOf);

assert.strictEqual(hello.includes('world'), true);
assert.strictEqual(hello.includes('world', 7), false);
assert.strictEqual(hello.includes('xyz'), false);
assert.strictEqual(hello.includes(''), true);
assert.strictEqual(hello.includes('h', -11), true);
assert.strictEqual(hello.includes('o', 5, 'utf8'), true);
assert.strictEqual(hello.includes('776f726c64', 'hex'), true);
assert.strictEqual(hello.includes('d29ybGQ', 'base64'), true);
assert.strictEqual(hello.includes('d29ybGQ', 'base64url'), true);
assert.strictEqual(hello.includes('wor', 8, 'latin1'), false);
assert.strictEqual(hello.includes(Buffer.from('lo')), true);
assert.strictEqual(hello.includes(new Uint8Array([0x6f])), true);
assert.strictEqual(hello.includes(111), true);
assert.strictEqual(hello.includes(0x16f), true);
assert.throws(() => hello.includes('x', 'bogus'), TypeError);
// the encoding is only validated for string needles
assert.strictEqual(hello.includes(111, 'bogus'), true);
assert.strictEqual(hello.includes(Buffer.from('lo'), 'bogus'), true);

assert.strictEqual(hello.lastIndexOf('o'), 7);
assert.strictEqual(hello.lastIndexOf('o', 5), 4);
assert.strictEqual(hello.lastIndexOf('o', 3), -1);
assert.strictEqual(hello.lastIndexOf('o', -4), 7);
assert.strictEqual(hello.lastIndexOf('o', -5), 4);
assert.strictEqual(hello.lastIndexOf('o', -99), -1);
assert.strictEqual(hello.lastIndexOf('o', 99), 7);
assert.strictEqual(hello.lastIndexOf('o', NaN), 7);
assert.strictEqual(hello.lastIndexOf('o', null), -1);
assert.strictEqual(hello.lastIndexOf('o', 'utf8'), 7);
assert.strictEqual(hello.lastIndexOf('world', 6), 6);
assert.strictEqual(hello.lastIndexOf('world', 5), -1);
assert.strictEqual(hello.lastIndexOf('l'), 9);
assert.strictEqual(hello.lastIndexOf('', 3), 3);
assert.strictEqual(hello.lastIndexOf(''), 11);
assert.strictEqual(hello.lastIndexOf('', -99), 0);
assert.strictEqual(hello.lastIndexOf(111), 7);
assert.strictEqual(hello.lastIndexOf(111, 5), 4);
assert.strictEqual(hello.lastIndexOf(111, -99), -1);
assert.strictEqual(hello.lastIndexOf(Buffer.from('l')), 9);
assert.strictEqual(hello.lastIndexOf('776f', 'hex'), 6);
assert.strictEqual(hello.lastIndexOf('6f', 5, 'hex'), 4);
assert.strictEqual(Buffer.from('').lastIndexOf('a'), -1);
assert.strictEqual(Buffer.from('').lastIndexOf(''), 0);

const ucs2 = Buffer.from('aéa', 'ucs2');
assert.strictEqual(ucs2.lastIndexOf('a', 'ucs2'), 4);
assert.strictEqual(ucs2.includes('é', 'ucs2'), true);
assert.strictEqual(ucs2.lastIndexOf('é', 1, 'ucs2'), -1);

function write(size, ...args) {
  const buf = Buffer.alloc(size).fill(0xee);
  const written = buf.write(...args);
  return `${written} ${buf.toString('hex')}`;
}

assert.strictEqual(write(5, 'abc', 'hex'), '1 abeeeeeeee');
assert.strictEqual(write(5, 'zz', 'hex'), '0 eeeeeeeeee');
assert.strictEqual(write(5, 'abzz12', 'hex'), '1 abeeeeeeee');
assert.strictEqual(write(5, 'a', 'hex'), '0 eeeeeeeeee');
assert.strictEqual(write(5, '0102030405060708', 'hex'), '5 0102030405');
assert.strictEqual(write(5, '01020304', 2, 'hex'), '3 eeee010203');
assert.strictEqual(write(5, '01020304', 2, 1, 'hex'), '1 eeee01eeee');
assert.strictEqual(write(5, 'ABCDEF', 'HEX'), '3 abcdefeeee');

assert.strictEqual(write(5, 'ab', 'ucs2'), '4 61006200ee');
assert.strictEqual(write(5, 'abc', 'ucs2'), '4 61006200ee');
assert.strictEqual(write(5, 'é😀', 'utf16le'), '4 e9003dd8ee');
assert.strictEqual(write(5, 'abc', 1, 'ucs2'), '4 ee61006200');
assert.strictEqual(write(5, 'abc', 0, 3, 'ucs2'), '2 6100eeeeee');
assert.strictEqual(write(5, '😀😀', 'ucs-2'), '4 3dd800deee');
assert.strictEqual(write(5, 'a', 4, 'ucs2'), '0 eeeeeeeeee');
assert.strictEqual(write(5, 'ab', 3, 'utf-16le'), '2 eeeeee6100');

assert.strictEqual(write(5, 'héllo', 'utf8'), '5 68c3a96c6c');
assert.strictEqual(write(5, 'héllo', 'latin1'), '5 68e96c6c6f');

console.log('ok');
