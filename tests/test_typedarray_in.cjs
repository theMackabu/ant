const assert = require('node:assert');

const bytes = new Uint8Array(2);
assert.deepStrictEqual([0 in bytes, '1' in bytes, 2 in bytes, -1 in bytes, '1.5' in bytes, '-0' in bytes], [true, true, false, false, false, false]);
assert.deepStrictEqual([Reflect.has(bytes, '0'), Reflect.has(bytes, 2), Reflect.has(bytes, 'length')], [true, false, true]);

const inherits = Object.create(new Float64Array(3));
assert.deepStrictEqual([2 in inherits, Reflect.has(inherits, '2'), 3 in inherits], [true, true, false]);

const backing = new ArrayBuffer(4);
const view = new Int32Array(backing);
backing.transfer();
assert.strictEqual(0 in view, false);

assert.throws(() => Reflect.has(1, 'a'), TypeError);

class Empty {}
class Derived extends Empty {}
assert.deepStrictEqual(Object.getOwnPropertyNames(Empty), ['length', 'name', 'prototype']);
assert.deepStrictEqual([Empty.length, Derived.length, (class { constructor(a, b) {} }).length], [0, 0, 2]);
assert.deepStrictEqual(Object.getOwnPropertyDescriptor(Empty, 'length'), { value: 0, writable: false, enumerable: false, configurable: true });
assert.strictEqual((class {}).name, '');
assert.strictEqual((class { static name = 's'; }).name, 's');
assert.deepStrictEqual(Object.keys(Empty), []);

const prototype = Object.getOwnPropertyDescriptor(Empty, 'prototype');
assert.deepStrictEqual([prototype.writable, prototype.enumerable, prototype.configurable], [false, false, false]);

// every canonical numeric string belongs to the typed array, valid index or not; other strings are ordinary
const keyed = new Uint8Array(2);
for (const key of ['1.5', '-0', '5', 'NaN', 'Infinity', '-1']) {
  keyed[key] = 3;
  assert.strictEqual(keyed[key], undefined, `${key} is not stored`);
  assert.throws(() => Object.defineProperty(keyed, key, { value: 1 }), TypeError, `defining ${key} throws`);
  assert.strictEqual(Reflect.defineProperty(keyed, key, { value: 1 }), false, `Reflect.defineProperty refuses ${key}`);
}
assert.deepStrictEqual(Object.keys(keyed), ['0', '1']);
keyed['01'] = 4;
keyed['1e3'] = 5;
assert.deepStrictEqual([keyed['01'], keyed['1e3'], keyed[1]], [4, 5, 0], 'non-canonical numeric strings are ordinary keys');

const shadowed = Object.setPrototypeOf(new Uint8Array(2), { '1.5': 'proto', '01': 'proto' });
assert.deepStrictEqual([shadowed['1.5'], shadowed['01']], [undefined, 'proto'], 'a numeric key stops at the typed array');
assert.strictEqual(Reflect.defineProperty(new Uint8Array(2), '1', { value: 9, writable: true, enumerable: true, configurable: true }), true);
assert.strictEqual(Reflect.defineProperty(new Uint8Array(2), '1', { value: 9, writable: false }), false, 'an element cannot become read-only');

const lengthed = [1];
assert.throws(() => Reflect.defineProperty(lengthed, 'length', { value: -1 }), RangeError, 'an invalid length throws even through Reflect');
Object.defineProperty(lengthed, 'length', { value: '0' });
assert.strictEqual(lengthed.length, 0, 'a string length converts');

const bigs = new BigInt64Array(2);
assert.throws(() => { bigs[5] = 1; }, TypeError, 'an out-of-range BigInt write still converts its value');
assert.throws(() => { bigs['1.5'] = 1; }, TypeError, 'an invalid BigInt key still converts its value');
assert.throws(() => { new Uint8Array(2)[5] = { valueOf() { throw new RangeError('v'); } }; }, RangeError);
bigs[5] = 1n;
assert.throws(() => Reflect.defineProperty(new BigInt64Array(2), '0', { value: 1 }), TypeError, 'a failed element conversion is not a refusal');
assert.deepStrictEqual([Reflect.defineProperty(bigs, '0', { value: 3n }), bigs[0]], [true, 3n]);
assert.throws(() => Reflect.defineProperty([], 'length', { value: { valueOf() { return null.x; } } }), TypeError, 'a throwing length conversion is abrupt');

const viaProto = Object.create(Object.setPrototypeOf(new Uint8Array(2), { '1.5': 'p', '9': 'q', '01': 'r' }));
assert.deepStrictEqual(['1.5' in viaProto, '9' in viaProto, '1' in viaProto, Reflect.has(viaProto, '1.5')], [false, false, true, false], 'numeric keys stop at an inherited typed array');
assert.deepStrictEqual([viaProto['1.5'], viaProto['9'], viaProto['01'], viaProto[1]], [undefined, undefined, 'r', 0], 'inherited reads see the elements');
const backingTa = new Uint8Array(2);
const writer = Object.create(backingTa);
writer['1.5'] = 3;
writer[1] = 7;
assert.deepStrictEqual([Object.keys(writer), backingTa[1], writer[1]], [['1'], 0, 7], 'an invalid key is dropped and a valid index lands on the receiver');

function FromTyped() {}
FromTyped.prototype = new Uint8Array(2);
for (const inheritor of [new FromTyped(), Object.setPrototypeOf({}, new Uint8Array(2)), { __proto__: new Uint8Array(2) }, Object.create(new Uint8Array(2))]) {
  inheritor['1.5'] = 9;
  assert.deepStrictEqual([inheritor['1.5'], inheritor[1], '1.5' in inheritor, '1' in inheritor, Object.keys(inheritor)], [undefined, 0, false, true, []], 'every way of inheriting from a typed array gets its key rules');
}

const shadowedRead = Object.setPrototypeOf(new Uint8Array(2), { 5: 'proto', 1.5: 'frac' });
assert.deepStrictEqual([Reflect.get(shadowedRead, '5', {}), Reflect.get(shadowedRead, '1.5', {}), Reflect.get(shadowedRead, '1', {})], [undefined, undefined, 0], 'a receiver read still stops at the typed array');

const own = new Uint8Array([7, 8]);
const ownDesc = Object.getOwnPropertyDescriptor(own, '0');
assert.deepStrictEqual([ownDesc.value, ownDesc.writable, ownDesc.enumerable, ownDesc.configurable, Object.getOwnPropertyDescriptor(own, '5')], [7, true, true, true, undefined], 'an element reads as a data descriptor');
assert.deepStrictEqual([delete own[0], delete own[5], delete own['1.5'], delete own.foo, own[0]], [false, true, true, true, 7], 'a valid index cannot be deleted');
assert.throws(() => { 'use strict'; delete own[1]; }, TypeError, 'strict delete of an element throws');
assert.deepStrictEqual((function (a) { return [delete arguments[0], arguments[0], delete arguments.foo]; })(1), [true, undefined, true]);

own.extra = 1;
const forInOwn = [];
for (const k in own) forInOwn.push(k);
assert.deepStrictEqual(forInOwn, ['0', '1', 'extra'], 'for-in lists elements first and skips prototype methods');
const forInInherited = [];
for (const k in Object.create(new Uint8Array(2))) forInInherited.push(k);
assert.deepStrictEqual(forInInherited, ['0', '1'], 'for-in walks into a typed array prototype');
const forInBuffers = [];
for (const k in new DataView(new ArrayBuffer(2))) forInBuffers.push(k);
for (const k in new ArrayBuffer(2)) forInBuffers.push(k);
assert.deepStrictEqual(forInBuffers, [], 'buffer and view accessors are not enumerable');
assert.deepStrictEqual([Object.keys({ ...own }), Object.keys(Object.assign({}, own))], [['0', '1', 'extra'], ['0', '1', 'extra']]);

console.log('typed array in OK');
