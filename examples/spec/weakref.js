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

let noNewError;
try {
  WeakRef({});
} catch (error) {
  noNewError = error;
}
test('weakref constructor requires new', noNewError instanceof TypeError, true);

const subclassTarget = {};
class WeakRefSubclass extends WeakRef {}
const subclassRef = new WeakRefSubclass(subclassTarget);
test('weakref subclass instanceof subclass', subclassRef instanceof WeakRefSubclass, true);
test('weakref subclass prototype', Object.getPrototypeOf(subclassRef) === WeakRefSubclass.prototype, true);
test('weakref subclass deref', subclassRef.deref() === subclassTarget, true);

let incompatibleReceiverError;
try {
  WeakRef.prototype.deref.call({});
} catch (error) {
  incompatibleReceiverError = error;
}
test(
  'weakref deref invalid receiver throws TypeError',
  incompatibleReceiverError instanceof TypeError,
  true
);

summary();
