const assert = require('node:assert');

const hex = (buf) => buf.toString('hex');

// Buffer.alloc(size, fill, encoding)
assert.strictEqual(hex(Buffer.alloc(5, 0xee)), 'eeeeeeeeee');
assert.strictEqual(hex(Buffer.alloc(3, -1)), 'ffffff');
assert.strictEqual(hex(Buffer.alloc(3, 257)), '010101');
assert.strictEqual(hex(Buffer.alloc(3, 3.9)), '030303');
assert.strictEqual(hex(Buffer.alloc(3, true)), '010101');
assert.strictEqual(hex(Buffer.alloc(3, NaN)), '000000');
assert.strictEqual(hex(Buffer.alloc(3, null)), '000000');
assert.strictEqual(hex(Buffer.alloc(5, 'ab')), '6162616261');
assert.strictEqual(hex(Buffer.alloc(5, 'é')), 'c3a9c3a9c3');
assert.strictEqual(hex(Buffer.alloc(5, 'aabb', 'hex')), 'aabbaabbaa');
assert.strictEqual(hex(Buffer.alloc(5, 'YWI=', 'base64')), '6162616261');
assert.strictEqual(hex(Buffer.alloc(5, 'é', 'ucs2')), 'e900e900e9');
assert.strictEqual(hex(Buffer.alloc(4, 'é', 'latin1')), 'e9e9e9e9');
assert.strictEqual(hex(Buffer.alloc(3, '')), '000000');
assert.strictEqual(hex(Buffer.alloc(5, Buffer.from([1, 2]))), '0102010201');
assert.strictEqual(hex(Buffer.alloc(5, new Uint8Array([3]))), '0303030303');
assert.strictEqual(hex(Buffer.alloc(3, 5, 'bogus')), '050505');
assert.strictEqual(hex(Buffer.alloc(0, 'a', 'bogus')), '');
assert.throws(() => Buffer.alloc(3, 'zz', 'hex'), { name: 'TypeError', code: 'ERR_INVALID_ARG_VALUE' });
assert.throws(() => Buffer.alloc(3, Buffer.alloc(0)), { name: 'TypeError', code: 'ERR_INVALID_ARG_VALUE' });
assert.throws(() => Buffer.alloc(3, 'a', 'bogus'), { name: 'TypeError', code: 'ERR_UNKNOWN_ENCODING' });

// Buffer.prototype.fill(value[, offset[, end]][, encoding])
const filled = (value, ...rest) => hex(Buffer.alloc(6).fill(0xee).fill(value, ...rest));
assert.notStrictEqual(Buffer.prototype.fill, Uint8Array.prototype.fill);
assert.strictEqual(filled('ab'), '616261626162');
assert.strictEqual(filled('ab', 1, 4), 'ee616261eeee');
assert.strictEqual(filled('6162', 1, 'hex'), 'ee6162616261');
assert.strictEqual(filled('6162', 1, 4, 'hex'), 'ee616261eeee');
assert.strictEqual(filled('ab', 'Utf-8'), '616261626162');
assert.strictEqual(filled('ab', ''), '616261626162');
assert.strictEqual(filled(0x1ff, 2), 'eeeeffffffff');
assert.strictEqual(filled(1, 3, 1), 'eeeeeeeeeeee');
assert.strictEqual(filled(new Uint16Array([0x0102])), '020102010201');
const self = Buffer.from([1, 2, 3, 4, 5]);
assert.strictEqual(hex(self.fill(self.subarray(0, 2))), '0102010201');
assert.throws(() => Buffer.alloc(3).fill('a', 'bogus'), { name: 'TypeError', code: 'ERR_UNKNOWN_ENCODING' });
assert.throws(() => Buffer.alloc(3).fill(1, -1), { name: 'RangeError', code: 'ERR_OUT_OF_RANGE' });
assert.throws(() => Buffer.alloc(3).fill(1, 1.5), { name: 'RangeError', code: 'ERR_OUT_OF_RANGE' });
assert.throws(() => Buffer.alloc(3).fill(1, 0, 4), { name: 'RangeError', code: 'ERR_OUT_OF_RANGE' });
assert.throws(() => Buffer.alloc(3).fill(1, '1'), { name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE' });

// unknown encodings throw ERR_UNKNOWN_ENCODING instead of silently using utf8
const hi = Buffer.from('hi');
const unknown = (fn, message) => assert.throws(fn, (error) => {
  assert.ok(error instanceof TypeError);
  assert.strictEqual(error.code, 'ERR_UNKNOWN_ENCODING');
  if (message) assert.strictEqual(error.message, message);
  return true;
});

unknown(() => hi.toString('bogus'), 'Unknown encoding: bogus');
unknown(() => hi.toString(null), 'Unknown encoding: null');
unknown(() => hi.toString(''), 'Unknown encoding: ');
unknown(() => Buffer.from('hi', 'bogus'));
unknown(() => Buffer.alloc(4).write('hi', 'bogus'));
unknown(() => Buffer.alloc(4).write('hi', 0, 'bogus'));
unknown(() => Buffer.alloc(4).write('hi', 0, 2, 'bogus'));
unknown(() => Buffer.alloc(4).write('hi', 0, 2, 5), 'Unknown encoding: 5');
unknown(() => hi.indexOf('h', 'bogus'), 'Unknown encoding: bogus');
unknown(() => hi.includes('h', 0, 'bogus'));
unknown(() => hi.lastIndexOf('h', 'bogus'));

assert.strictEqual(hi.toString(), 'hi');
assert.strictEqual(hi.toString(undefined), 'hi');
assert.strictEqual(hi.toString('bogus', 1, 1), '');
assert.strictEqual(hi.toString('UTF8'), 'hi');
assert.strictEqual(hex(Buffer.from('hi', null)), '6869');
assert.strictEqual(hex(Buffer.from('hi', '')), '6869');
assert.strictEqual(Buffer.alloc(4).write('hi', ''), 2);
assert.strictEqual(Buffer.alloc(4).write('hi', 0, 2, null), 2);
assert.strictEqual(hi.indexOf(104, 'bogus'), 0);
assert.strictEqual(Buffer.byteLength('hi', 'bogus'), 2);

// write validates offset/length like Node
assert.throws(() => Buffer.alloc(4).write('hi', null), { name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE' });
assert.throws(() => Buffer.alloc(4).write('hi', 5), { name: 'RangeError', code: 'ERR_OUT_OF_RANGE' });
assert.throws(() => Buffer.alloc(4).write('hi', 0, 5), { name: 'RangeError', code: 'ERR_OUT_OF_RANGE' });
assert.strictEqual(Buffer.alloc(4).write('hi', 4), 0);
assert.strictEqual(Buffer.alloc(4).write('hello', 1, 2), 2);

// hex decoding keeps the valid prefix, as Node
assert.strictEqual(hex(Buffer.from('hi', 'hex')), '');
assert.strictEqual(hex(Buffer.from('abc', 'hex')), 'ab');
assert.strictEqual(hex(Buffer.from('abzz12', 'Hex')), 'ab');

console.log('ok');
