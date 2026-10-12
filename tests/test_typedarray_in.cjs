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

console.log('typed array in OK');
