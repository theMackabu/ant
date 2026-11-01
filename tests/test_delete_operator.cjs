const assert = require('node:assert');

function throwsType(fn, message) {
  assert.throws(fn, (e) => e instanceof TypeError && e.message === message);
}

const nullish = 'Cannot convert undefined or null to object';
let u;
const n = null;

throwsType(() => delete u.y, nullish);
throwsType(() => delete n.y, nullish);
throwsType(() => delete u['k'], nullish);
throwsType(() => delete u[Symbol.iterator], nullish);
throwsType(() => (function () { 'use strict'; return delete u.y; })(), nullish);

const order = [];
assert.throws(() => delete u[{ toString() { order.push('key'); return 'x'; } }], TypeError);
assert.deepStrictEqual(order, [], 'base is checked before the key is converted');

assert.throws(() => delete ({})[{ toString() { throw new RangeError('key'); } }], RangeError);

assert.strictEqual(delete u?.y, true);
assert.strictEqual(delete 'abc'.length, false);
assert.strictEqual(delete (5).x, true);

const o = { 0: 1, a: 2, [Symbol.for('s')]: 3 };
assert.strictEqual(delete o[0], true);
assert.strictEqual(delete o['a'], true);
assert.strictEqual(delete o[Symbol.for('s')], true);
assert.deepStrictEqual(Reflect.ownKeys(o), []);

throwsType(() => { 'use strict'; delete Object.freeze({ a: 1 }).a; }, "Cannot delete property 'a' of #<Object>");
throwsType(() => { 'use strict'; delete Object.freeze([1])[0]; }, "Cannot delete property '0' of [object Array]");
class Foo { constructor() { Object.defineProperty(this, 'k', { value: 1 }); } }
throwsType(() => { 'use strict'; delete new Foo().k; }, "Cannot delete property 'k' of #<Foo>");

const sym = Symbol('s');
throwsType(() => { 'use strict'; delete Object.freeze({ [sym]: 1 })[sym]; }, "Cannot delete property 'Symbol(s)' of #<Object>");
throwsType(() => { 'use strict'; const o = {}; Object.defineProperty(o, Symbol.iterator, { value: 1 }); delete o[Symbol.iterator]; }, "Cannot delete property 'Symbol(Symbol.iterator)' of #<Object>");
assert.strictEqual((function () { 'use strict'; return delete Object.freeze({})[sym]; })(), true, 'missing symbol on a frozen object');
assert.strictEqual(delete Object.seal({ [sym]: 1 })[sym], false);
throwsType(() => { 'use strict'; delete Object.freeze(function named() { return 1; }).name; }, "Cannot delete property 'name' of function named() { return 1; }");
const long = eval('(function () { ' + 'x'.repeat(120) + ' })');
throwsType(() => { 'use strict'; delete Object.freeze(long).name; }, "Cannot delete property 'name' of " + String(long).slice(0, 111) + '...<omitted>... }');

function hot(obj, key) { return delete obj[key]; }
for (let i = 0; i < 5000; i++) assert.strictEqual(hot({ a: 1 }, 'a'), true);
throwsType(() => hot(undefined, 'a'), nullish);
assert.throws(() => hot({}, { toString() { throw new RangeError('hot'); } }), RangeError);

console.log('delete operator ok');
