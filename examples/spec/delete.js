import { test, summary } from './helpers.js';

console.log('Delete Operator Tests\n');

const obj = { a: 1, b: 2, c: 3 };

let result = delete obj.a;
test('delete existing property returns true', result, true);
test('property removed after delete', obj.a, undefined);
test('has property after delete', 'a' in obj, false);
test('other properties remain', obj.b, 2);

result = delete obj.nonexistent;
test('delete non-existent property returns true', result, true);

const nested = { outer: { inner: 42 } };
result = delete nested.outer.inner;
test('delete nested property', result, true);
test('nested property removed', nested.outer.inner, undefined);
test('parent still exists', typeof nested.outer, 'object');

const arr = [1, 2, 3, 4, 5];
result = delete arr[2];
test('delete array element returns true', result, true);
test('array element becomes undefined', arr[2], undefined);
test('array length unchanged', arr.length, 5);
test('array has hole', 2 in arr, false);

const withUndefined = { x: undefined };
result = delete withUndefined.x;
test('delete property set to undefined', result, true);
test('property gone after delete', 'x' in withUndefined, false);

const computed = { foo: 'bar', baz: 'qux' };
const key = 'foo';
result = delete computed[key];
test('delete with computed key', result, true);
test('computed key property removed', computed.foo, undefined);

const proto = { inherited: true };
const child = Object.create(proto);
child.own = 'value';
result = delete child.own;
test('delete own property', result, true);
test('own property removed', child.own, undefined);
test('inherited property still accessible', child.inherited, true);
result = delete child.inherited;
test('delete inherited returns true but keeps it', result, true);
test('inherited still accessible after delete', child.inherited, true);

const symbolKey = Symbol('test');
const withSymbol = { [symbolKey]: 'symbol value' };
result = delete withSymbol[symbolKey];
test('delete symbol property returns true', result, true);
test('symbol property removed', withSymbol[symbolKey], undefined);

summary();
