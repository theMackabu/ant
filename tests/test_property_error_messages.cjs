'use strict';
const assert = require('node:assert');

const cases = [
  ["read-only own", () => { Object.defineProperty({}, 'a', { value: 1 }).a = 2; }, "Cannot assign to read only property 'a' of object '#<Object>'"],
  ["frozen object", () => { Object.freeze({ a: 1 }).a = 2; }, "Cannot assign to read only property 'a' of object '#<Object>'"],
  ["inherited read-only", () => { Object.create(Object.freeze({ a: 1 })).a = 2; }, "Cannot assign to read only property 'a' of object '#<Object>'"],
  ["symbol read-only", () => { Object.freeze({ [Symbol.for('s')]: 1 })[Symbol.for('s')] = 2; }, "Cannot assign to read only property 'Symbol(s)' of object '#<Object>'"],
  ["class instance", () => { class K { constructor() { Object.defineProperty(this, 'a', { value: 1 }); } } new K().a = 2; }, "Cannot assign to read only property 'a' of object '#<K>'"],
  ["null prototype", () => { Object.freeze(Object.assign(Object.create(null), { a: 1 })).a = 2; }, "Cannot assign to read only property 'a' of object '[object Object]'"],
  ["frozen array element", () => { Object.freeze([1, 2])[1] = 5; }, "Cannot assign to read only property '1' of object '[object Array]'"],
  ["function name", () => { (function f() {}).name = 'x'; }, "Cannot assign to read only property 'name' of function 'function f() {}'"],
  ["builtin name", () => { Math.max.name = 'x'; }, "Cannot assign to read only property 'name' of function 'function max() { [native code] }'"],
  ["string length", () => { 'abc'.length = 1; }, "Cannot assign to read only property 'length' of string 'abc'"],
  ["string index", () => { 'abc'[0] = 'x'; }, "Cannot assign to read only property '0' of string 'abc'"],
  ["create on string", () => { 'abc'.foo = 1; }, "Cannot create property 'foo' on string 'abc'"],
  ["create on number", () => { (5).x = 1; }, "Cannot create property 'x' on number '5'"],
  ["create symbol on string", () => { 'abc'[Symbol.for('s')] = 1; }, "Cannot create property 'Symbol(s)' on string 'abc'"],
  ["getter only", () => { ({ get a() { return 1; } }).a = 2; }, "Cannot set property a of #<Object> which has only a getter"],
  ["getter only symbol", () => { ({ get [Symbol.for('s')]() { return 1; } })[Symbol.for('s')] = 2; }, "Cannot set property Symbol(s) of #<Object> which has only a getter"],
  ["frozen regexp lastIndex", () => { Object.freeze(/a/g).lastIndex = 1; }, "Cannot assign to read only property 'lastIndex' of object '[object RegExp]'"],
  ["frozen regexp exec", () => { Object.freeze(/a/g).exec('a'); }, "Cannot assign to read only property 'lastIndex' of object '[object RegExp]'"],
  ["frozen push", () => { Object.freeze([1, 2, 3]).push(4); }, "Cannot add property 3, object is not extensible"],
  ["frozen push no args", () => { Object.freeze([1]).push(); }, "Cannot assign to read only property 'length' of object '[object Array]'"],
  ["sealed unshift", () => { Object.seal([1, 2]).unshift(7, 8); }, "Cannot add property 3, object is not extensible"],
  ["non-extensible unshift empty", () => { Object.preventExtensions([]).unshift(1); }, "Cannot add property 0, object is not extensible"],
  ["frozen sort", () => { Object.freeze([1, 2, 3]).sort(); }, "Cannot assign to read only property '0' of object '[object Array]'"],
  ["frozen reverse", () => { Object.freeze([1, 2, 3]).reverse(); }, "Cannot assign to read only property '0' of object '[object Array]'"],
  ["frozen copyWithin", () => { Object.freeze([1, 2, 3]).copyWithin(0, 1); }, "Cannot assign to read only property '0' of object '[object Array]'"],
  ["frozen arguments push", () => { (function () { Object.freeze(arguments); Array.prototype.push.call(arguments, 1); })(1, 2); }, "Cannot add property 2, object is not extensible"],
  ["sealed arguments pop", () => { (function () { Object.seal(arguments); Array.prototype.pop.call(arguments); })(1, 2); }, "Cannot delete property '1' of #<Object>"],
  ["sealed arguments splice", () => { (function () { Object.seal(arguments); Array.prototype.splice.call(arguments, 0, 1); })(1, 2, 3); }, "Cannot delete property '2' of #<Object>"],
  ["proxy non-extensible target", () => { new Proxy(Object.preventExtensions({}), {}).q = 1; }, "Cannot define property q, object is not extensible"],
  ["proxy non-extensible symbol", () => { new Proxy(Object.preventExtensions({}), {})[Symbol.for('s')] = 1; }, "Cannot define property Symbol(s), object is not extensible"],
  ["proxy non-extensible index", () => { new Proxy(Object.preventExtensions([]), {})[3] = 1; }, "Cannot define property 3, object is not extensible"],
  ["proxy inherited writable", () => { new Proxy(Object.preventExtensions(Object.create({ q: 1 })), {}).q = 2; }, "Cannot define property q, object is not extensible"],
];

for (const [name, run, message] of cases) {
  assert.throws(run, (e) => e instanceof TypeError && e.message === message, name);
}


assert.strictEqual(Reflect.set(new Proxy(Object.preventExtensions({}), {}), 'q', 1), false);

console.log('property error messages ok');
