const assert = require('node:assert');
for (const endian of ['LE', 'BE']) {
  for (const signed of [false, true]) {
    const suffix = `Big${signed ? 'Int' : 'UInt'}64${endian}`;
    const write = `write${suffix}`, read = `read${suffix}`;
    const min = signed ? -(1n << 63n) : 0n;
    const max = signed ? (1n << 63n) - 1n : (1n << 64n) - 1n;
    for (const value of [min, max, 0n, 1n, 0x123456789abcdefn]) {
      const backing = Buffer.alloc(16).fill(0xaa);
      const view = Buffer.from(backing.buffer, backing.byteOffset + 3, 10);
      assert.strictEqual(view[write](value, 1), 9);
      assert.strictEqual(view[read](1), value);
      let bits = BigInt.asUintN(64, value);
      for (let i = 0; i < 8; i++) {
        assert.strictEqual(backing[4 + (endian === 'LE' ? i : 7 - i)], Number(bits & 255n));
        bits >>= 8n;
      }
      assert.strictEqual(backing[3], 0xaa);
      assert.strictEqual(backing[12], 0xaa);
    }
    const b = Buffer.alloc(8);
    assert.strictEqual(b[write](Object(1n)), 8);
    assert.strictEqual(b[read](undefined), 1n);
    for (const value of [min - 1n, max + 1n, 1n << 128n, -(1n << 128n)]) {
      assert.throws(() => b[write](value), RangeError);
      assert.strictEqual(b[read](), 1n);
    }
    for (const value of [undefined, 1, '1', null, true]) assert.throws(() => b[write](value), TypeError);
    for (const offset of [null, '0', 0n, {}]) {
      assert.throws(() => b[write](1n, offset), TypeError);
      assert.throws(() => b[read](offset), TypeError);
    }
    for (const offset of [-1, 0.5, NaN, Infinity, 1, 2 ** 64]) {
      assert.throws(() => b[write](1n, offset), RangeError);
      assert.throws(() => b[read](offset), RangeError);
    }
    for (const length of [0, 1, 7]) {
      assert.throws(() => Buffer.alloc(length)[write](0n), RangeError);
      assert.throws(() => Buffer.alloc(length)[read](), RangeError);
    }
    const ab = new ArrayBuffer(8);
    const detached = Buffer.from(ab);
    ab.transfer();
    assert.throws(() => detached[write](1n));
    assert.throws(() => detached[read]());
  }
  for (const op of ['read', 'write']) {
    const canonical = `${op}BigUInt64${endian}`;
    assert.strictEqual(Buffer.prototype[canonical].name, canonical);
    assert.strictEqual(Buffer.prototype[`${op}BigUint64${endian}`].name, canonical);
  }
  assert.strictEqual(Buffer.prototype[`writeBigUint64${endian}`], Buffer.prototype[`writeBigUInt64${endian}`]);
  assert.strictEqual(Buffer.prototype[`readBigUint64${endian}`], Buffer.prototype[`readBigUInt64${endian}`]);
}
console.log('buffer:bigint64:ok');
