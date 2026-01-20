import { test, summary } from './helpers.js';

console.log('WeakRef Tests\n');

const obj1 = { name: 'test' };
const ref1 = new WeakRef(obj1);

test('weakref deref returns target', ref1.deref() === obj1, true);
test('weakref deref value', ref1.deref().name, 'test');

const obj2 = { value: 42 };
const ref2 = new WeakRef(obj2);
test('weakref deref number value', ref2.deref().value, 42);

const obj3 = {};
const ref3 = new WeakRef(obj3);
test('weakref has deref method', typeof ref3.deref, 'function');
test('weakref instanceof', ref3 instanceof WeakRef, true);
test('weakref prototype', Object.getPrototypeOf(ref3) === WeakRef.prototype, true);

summary();
