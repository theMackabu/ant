const assert = require('node:assert');
const v8 = require('node:v8');

const shared = { a: 1 };
const cyclic = {};
cyclic.self = cyclic;
const withProps = [1, 2];
withProps.foo = 'x';
const rangeError = new RangeError('bad', { cause: { x: 1 } });
rangeError.stack = 'STACK';
const plainError = new Error('m');
plainError.stack = 'S';

const fixtures = [
  ['undefined', undefined, 'ff0f5f'],
  ['null', null, 'ff0f30'],
  ['true', true, 'ff0f54'],
  ['false', false, 'ff0f46'],
  ['zero', 0, 'ff0f4900'],
  ['one', 1, 'ff0f4902'],
  ['minus one', -1, 'ff0f4901'],
  ['int32 max', 2 ** 31 - 1, 'ff0f49feffffff0f'],
  ['int32 min', -(2 ** 31), 'ff0f49ffffffff0f'],
  ['past int32', 2 ** 31, 'ff0f4e000000000000e041'],
  ['negative zero', -0, 'ff0f4e0000000000000080'],
  ['double', 1.5, 'ff0f4e000000000000f83f'],
  ['ascii string', 'hi', 'ff0f22026869'],
  ['latin1 string', 'é', 'ff0f2201e9'],
  ['two-byte string', 'aπ', 'ff0f63046100c003'],
  ['two-byte string, odd length', 'ab€', 'ff0f630661006200ac20'],
  ['astral string', '😀', 'ff0f63043dd800de'],
  ['empty string', '', 'ff0f2200'],
  ['bigint zero', 0n, 'ff0f5a00'],
  ['bigint one', 1n, 'ff0f5a100100000000000000'],
  ['bigint minus one', -1n, 'ff0f5a110100000000000000'],
  ['bigint two words', 2n ** 64n, 'ff0f5a2000000000000000000100000000000000'],
  ['bigint negative two words', -(2n ** 70n), 'ff0f5a2100000000000000004000000000000000'],
  ['object', { a: 1, b: 'x' }, 'ff0f6f22016149022201622201787b02'],
  ['array', [1, 'a', null], 'ff0f4103490222016130240003'],
  ['sparse array', [1, , 3], 'ff0f61034900490249044906400203'],
  ['array with properties', withProps, 'ff0f4102490249042203666f6f220178240102'],
  ['array of one hole', [,], 'ff0f6101400001'],
  ['date', new Date(1e12), 'ff0f44000000a2941a6d42'],
  ['regexp', /a+b/dgimsuy, 'ff0f522203612b62bf01'],
  ['regexp v flag', /x/v, 'ff0f522201788002'],
  ['map', new Map([[1, 'a'], ['k', { a: 1 }]]), 'ff0f3b490222016122016b6f22016149027b013a04'],
  ['set', new Set([1, 'a']), 'ff0f2749022201612c02'],
  ['ArrayBuffer', new Uint8Array([1, 2, 3]).buffer, 'ff0f4203010203'],
  ['Uint8Array', new Uint8Array([1, 2, 3]), 'ff0f5c0103010203'],
  ['Buffer', Buffer.from([1, 2]), 'ff0f5c0a020102'],
  ['Float64Array', new Float64Array([1.5]), 'ff0f5c0808000000000000f83f'],
  ['Int16Array', new Int16Array([1, -2]), 'ff0f5c03040100feff'],
  ['DataView', new DataView(new Uint8Array([9, 8]).buffer), 'ff0f5c09020908'],
  ['BigInt64Array', new BigInt64Array([1n]), 'ff0f5c0b080100000000000000'],
  ['BigUint64Array', new BigUint64Array([2n]), 'ff0f5c0c080200000000000000'],
  ['Uint8ClampedArray', new Uint8ClampedArray([5]), 'ff0f5c020105'],
  ['subarray view', new Uint8Array(new Uint8Array([1, 2, 3, 4]).buffer, 1, 2), 'ff0f5c01020203'],
  ['shared reference', [shared, shared], 'ff0f41026f22016149027b015e01240002'],
  [
    'boxed primitives',
    [new Number(1.5), new Boolean(true), new Boolean(false), new String('s'), Object(5n)],
    'ff0f41056e000000000000f83f7978732201737a100500000000000000240005'
  ],
  ['nested', { a: [{ b: new Map() }] }, 'ff0f6f22016141016f2201623b3a007b012400017b01'],
  ['repeated strings', ['same', 'same'], 'ff0f4102220473616d65220473616d65240002']
];

for (const [name, value, hex] of fixtures) {
  assert.strictEqual(v8.serialize(value).toString('hex'), hex, `${name}: bytes`);
  assert.deepStrictEqual(v8.deserialize(Buffer.from(hex, 'hex')), value, `${name}: decode`);
}

assert.strictEqual(v8.serialize(cyclic).toString('hex'), 'ff0f6f220473656c665e007b01');
const cyclicBack = v8.deserialize(Buffer.from('ff0f6f220473656c665e007b01', 'hex'));
assert.strictEqual(cyclicBack.self, cyclicBack);

const sharedBack = v8.deserialize(v8.serialize([shared, shared]));
assert.strictEqual(sharedBack[0], sharedBack[1]);

const sparseBack = v8.deserialize(v8.serialize([1, , 3]));
assert.strictEqual(sparseBack.length, 3);
assert.strictEqual(1 in sparseBack, false);

const bufferBack = v8.deserialize(v8.serialize(Buffer.from('hi')));
assert.ok(Buffer.isBuffer(bufferBack));
assert.strictEqual(bufferBack.toString(), 'hi');

assert.strictEqual(v8.serialize(rangeError).toString('hex'), 'ff0f72526d2203626164732205535441434b636f22017849027b012e');
const rangeBack = v8.deserialize(v8.serialize(rangeError));
assert.ok(rangeBack instanceof RangeError);
assert.strictEqual(rangeBack.message, 'bad');
assert.strictEqual(rangeBack.stack, 'STACK');
assert.deepStrictEqual(rangeBack.cause, { x: 1 });
assert.strictEqual(Object.keys(rangeBack).length, 0);

assert.strictEqual(v8.serialize(plainError).toString('hex'), 'ff0f726d22016d732201532e');
const plainBack = v8.deserialize(v8.serialize(plainError));
assert.strictEqual(Object.getPrototypeOf(plainBack), Error.prototype);

const indexKeys = { 1: 'a', b: 2, 4294967294: 'z', 4294967295: 'y' };
const indexKeysHex = 'ff0f6f49022201614e0000c0ffffffef4122017a2201624904220a343239343936373239352201797b04';
assert.deepStrictEqual(v8.deserialize(Buffer.from(indexKeysHex, 'hex')), indexKeys);
assert.deepStrictEqual(v8.deserialize(v8.serialize(indexKeys)), indexKeys);

assert.ok(Number.isNaN(v8.deserialize(v8.serialize(NaN))));
assert.strictEqual(v8.deserialize(Buffer.from('ff0f53026869', 'hex')), 'hi');
assert.strictEqual(v8.deserialize(Buffer.from('ff0f5507', 'hex')), 7);

const view = v8.deserialize(Buffer.from('ff0f4204010203045642010200', 'hex'));
assert.ok(view instanceof Uint8Array);
assert.deepStrictEqual([...view], [2, 3]);

assert.throws(() => v8.serialize(() => {}), /could not be cloned/);
assert.throws(() => v8.serialize({ nested: Symbol('s') }), /could not be cloned/);
assert.throws(() => v8.deserialize(Buffer.from('abcd00', 'hex')), /invalid or unsupported version/);
assert.throws(() => v8.deserialize(Buffer.from('ff0f6f2201', 'hex')), /Unable to deserialize cloned data/);
assert.throws(() => v8.deserialize(Buffer.from('ff0f5e05', 'hex')), /Unable to deserialize cloned data/);

console.log('v8 serialize format OK');
