'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const v8 = require('node:v8');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '..', 'js', 'wire.js'), 'utf8');
const { encode, decode } = vm.runInThisContext(`(() => {\n${source}\nreturn { encode, decode };\n})()`);

const shared = { a: 1 };
const cyclic = { name: 'loop' };
cyclic.self = cyclic;
const withProps = [1, 2];
withProps.extra = 'x';
const error = new RangeError('bad', { cause: { code: 7 } });
error.stack = 'STACK';

const mixedKeys = [{ 1: 'a', b: 2, 4294967294: 'z', 4294967295: 'y' }];

const values = [
  undefined, null, true, false, 0, 1, -1, 2 ** 31 - 1, -(2 ** 31), 2 ** 31, -0, 1.5, NaN,
  '', 'hi', 'é', 'aπ', 'ab€', '😀', 'x'.repeat(200) + 'π',
  0n, 1n, -1n, 2n ** 64n, -(2n ** 70n),
  { a: 1, b: 'x' }, ...mixedKeys,
  [1, 'a', null], [1, , 3], withProps, [,],
  new Date(1e12), /a+b/dgimsuy, /x/v,
  new Map([[1, 'a'], ['k', { a: 1 }]]), new Set([1, 'a']),
  new Uint8Array([1, 2, 3]).buffer, new Uint8Array([1, 2, 3]), new Float64Array([1.5]), new Int16Array([1, -2]),
  new DataView(new Uint8Array([9, 8]).buffer), new BigInt64Array([1n]), new Uint8ClampedArray([5]),
  new Uint8Array(new Uint8Array([1, 2, 3, 4]).buffer, 1, 2),
  [shared, shared], [new Number(1.5), new Boolean(true), new Boolean(false), new String('s'), Object(5n)],
  { a: [{ b: new Map() }] }, error, cyclic
];

for (const value of values) {
  const label = typeof value === 'bigint' ? `${value}n` : require('node:util').inspect(value);
  const ours = Buffer.from(encode(value));
  if (!mixedKeys.includes(value)) assert.equal(ours.toString('hex'), v8.serialize(value).toString('hex'), `bytes: ${label}`);
  if (value === cyclic) {
    assert.equal(decode(v8.serialize(value)).self.self.name, 'loop');
    assert.equal(v8.deserialize(ours).self.self.name, 'loop');
    continue;
  }
  assert.deepEqual(decode(v8.serialize(value)), value, `decode: ${label}`);
  assert.deepEqual(v8.deserialize(ours), value, `node reads ours: ${label}`);
}

const errorBack = decode(v8.serialize(error));
assert.ok(errorBack instanceof RangeError);
assert.equal(errorBack.stack, 'STACK');
assert.deepEqual(errorBack.cause, { code: 7 });
assert.equal(decode(Buffer.from('ff0f53026869', 'hex')), 'hi');
assert.deepEqual([...decode(Buffer.from('ff0f4204010203045642010200', 'hex'))], [2, 3]);
assert.throws(() => encode(() => {}), /could not be cloned/);
assert.throws(() => decode(Buffer.from('ff0f6f2201', 'hex')), /Unable to deserialize/);

console.log('desktop-wire-ok');
