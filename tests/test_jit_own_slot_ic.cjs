'use strict';
function same(actual, expected, label) {
  if (!Object.is(actual, expected)) throw new Error(label || 'own property value');
}
function read(object) { return object.value; }
const object = { value: 1 };
for (let i = 0; i < 1000; i++) same(read(object), 1);
object.value = -0;
same(read(object), -0, 'read latest value');
const later = { first: true, value: 2 };
for (let i = 0; i < 1000; i++) same(read(later), 2, 'changed slot');
same(read(object), -0, 'original slot');
delete object.value;
same(read(object), undefined, 'deleted slot');
const prototype = { value: 3 };
Object.setPrototypeOf(object, prototype);
for (let i = 0; i < 1000; i++) same(read(object), 3, 'inherited slot');
let gets = 0;
Object.defineProperty(object, 'value', {
  configurable: true,
  get() { gets++; return this === object ? 4 : 0; },
});
for (let i = 0; i < 1000; i++) same(read(object), 4, 'accessor receiver');
same(gets, 1000, 'one getter call per read');
delete object.value;
object.value = 5;
same(read(object), 5, 'restored own data');
const wide = { a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, value: 6 };
for (let i = 0; i < 1000; i++) same(read(wide), 6, 'overflow storage');
function callable() {}
callable.value = 7;
same(read(callable), 7, 'function property');
const array = [];
array.value = 8;
same(read(array), 8, 'array property');
const proxy = new Proxy({ value: 9 }, { get(target, key) { return key === 'value' ? 10 : target[key]; } });
same(read(proxy), 10, 'proxy trap');
let threw = false;
try { read(null); } catch (error) { threw = error instanceof TypeError; }
same(threw, true, 'null receiver');
console.log('PASS guarded own-slot field cache');
