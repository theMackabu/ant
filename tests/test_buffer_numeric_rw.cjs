const assert = require('node:assert');

const P = Buffer.prototype;
const fixed = ['UInt8', 'UInt16LE', 'UInt16BE', 'UInt32LE', 'UInt32BE', 'Int8', 'Int16LE', 'Int16BE', 'Int32LE', 'Int32BE'];
const floats = ['FloatLE', 'FloatBE', 'DoubleLE', 'DoubleBE'];
const variable = ['UIntLE', 'UIntBE', 'IntLE', 'IntBE'];

for (const k of [...fixed, ...floats, ...variable]) {
  for (const op of ['read', 'write']) assert.strictEqual(typeof P[op + k], 'function', op + k);
}
assert.deepStrictEqual(
  [...fixed.map(k => P['read' + k].length), ...floats.map(k => P['read' + k].length), ...variable.map(k => P['read' + k].length)],
  [...fixed.map(() => 1), ...floats.map(() => 0), ...variable.map(() => 2)]
);
assert.deepStrictEqual([P.writeUInt8.length, P.writeDoubleBE.length, P.writeIntLE.length], [1, 1, 3]);
for (const k of ['UInt8', 'UInt16LE', 'UInt32BE', 'UIntLE', 'UIntBE']) {
  const alias = k.replace('UInt', 'Uint');
  assert.strictEqual(P['read' + alias], P['read' + k], 'read' + alias);
  assert.strictEqual(P['write' + alias], P['write' + k], 'write' + alias);
}

const b = Buffer.from([0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x88, 0xff, 0x80]);
assert.deepStrictEqual([b.readUInt8(7), b.readInt8(7), b.readUInt16LE(0), b.readUInt16BE(0), b.readInt16LE(8), b.readInt16BE(8)], [0x88, -120, 0x0201, 0x0102, -32513, -128]);
assert.deepStrictEqual([b.readUInt32LE(0), b.readUInt32BE(0), b.readInt32LE(6), b.readInt32BE(6)], [0x04030201, 0x01020304, -2130737145, 126418816]);
assert.deepStrictEqual([b.readUIntLE(1, 3), b.readUIntBE(1, 3), b.readIntLE(4, 6), b.readIntBE(4, 6), b.readUIntBE(0, 6)], [0x040302, 0x020304, -139639989533179, 5523454361472, 0x010203040506]);

const w = Buffer.alloc(8);
assert.deepStrictEqual([w.writeUInt16LE(0xabcd, 1), [...w.subarray(0, 4)]], [3, [0, 0xcd, 0xab, 0]]);
assert.deepStrictEqual([w.writeInt32BE(-2, 4), [...w.subarray(4)]], [8, [0xff, 0xff, 0xff, 0xfe]]);
assert.deepStrictEqual([w.writeIntLE(-0x123456, 0, 3), w.readIntLE(0, 3), w.writeUIntBE(2 ** 48 - 1, 0, 6), w.readUIntBE(0, 6)], [3, -0x123456, 6, 2 ** 48 - 1]);
assert.deepStrictEqual([w.writeUInt8(1.9), w[0], w.writeUInt8(NaN), w[0], w.writeUInt8('7'), w[0]], [1, 1, 1, 0, 1, 7], 'values truncate and coerce like node');
w.writeDoubleBE(-1234.5678); assert.strictEqual(w.readDoubleBE(0), -1234.5678);
w.writeFloatLE(3.25, 4); assert.strictEqual(w.readFloatLE(4), 3.25);
w.writeDoubleLE(NaN); assert.ok(Number.isNaN(w.readDoubleLE(0)));

assert.throws(() => w.writeUInt8(256), RangeError);
assert.throws(() => w.writeInt8(128), RangeError);
assert.throws(() => w.writeUInt16LE(-1), RangeError);
assert.throws(() => w.writeIntLE(2 ** 23, 0, 3), RangeError);
assert.throws(() => w.readUInt32LE(5), RangeError);
assert.throws(() => w.readUInt8(1.5), RangeError);
assert.throws(() => w.readUInt8('1'), TypeError);
assert.throws(() => w.readUIntLE(0, 7), RangeError);
assert.throws(() => w.readUIntLE(0, '2'), TypeError);
assert.throws(() => w.writeUInt32LE(1, 7), RangeError);
assert.throws(() => P.readUInt8.call({}), Error);

console.log('buffer numeric read/write OK');
