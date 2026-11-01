const assert = require('node:assert');

const max = Math.max;
assert.strictEqual(Object.isFrozen(Math.floor), false, 'untouched builtin is not frozen');
assert.strictEqual(Object.isSealed(Math.floor), false, 'untouched builtin is not sealed');
assert.strictEqual(Object.isExtensible(Math.floor), true, 'untouched builtin is extensible');

assert.strictEqual(Object.freeze(max), max, 'freeze returns the builtin itself');
assert.strictEqual(Object.isFrozen(max), true);
assert.strictEqual(Object.isExtensible(max), false);
assert.strictEqual(Object.getOwnPropertyDescriptor(max, 'name').configurable, false);
assert.strictEqual(Object.getOwnPropertyDescriptor(max, 'length').writable, false);
assert.strictEqual(delete max.name, false);
assert.strictEqual(max.name, 'max');
assert.strictEqual(Reflect.deleteProperty(max, 'length'), false);
assert.throws(() => { 'use strict'; delete max.name; }, { name: 'TypeError', message: "Cannot delete property 'name' of function max() { [native code] }" });
assert.throws(() => { 'use strict'; max.extra = 1; }, { name: 'TypeError', message: 'Cannot add property extra, object is not extensible' });
assert.throws(() => Object.defineProperty(max, 'z', { value: 1 }), TypeError);
assert.strictEqual(max(1, 3, 2), 3, 'frozen builtin still calls');

const min = Math.min;
Object.seal(min);
assert.strictEqual(Object.isSealed(min), true);
assert.strictEqual(Object.isFrozen(min), true, "sealed builtin with read-only own properties is frozen");
assert.throws(() => { 'use strict'; delete min.name; }, TypeError);

const abs = Math.abs;
Object.preventExtensions(abs);
assert.strictEqual(Object.isExtensible(abs), false);
assert.throws(() => { 'use strict'; abs.q = 1; }, { name: 'TypeError', message: 'Cannot add property q, object is not extensible' });

const sym = Symbol('k');
assert.throws(() => { 'use strict'; Object.preventExtensions({})[sym] = 1; }, { message: 'Cannot add property Symbol(k), object is not extensible' });
assert.throws(() => { 'use strict'; Object.freeze([1, 2])[7] = 1; }, { message: 'Cannot add property 7, object is not extensible' });
assert.throws(() => { 'use strict'; Object.seal({ a: 1 }).b = 1; }, { message: 'Cannot add property b, object is not extensible' });

console.log('builtin integrity ok');
