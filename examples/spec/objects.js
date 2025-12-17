import { test, testDeep, summary } from './helpers.js';

console.log('Object Tests\n');

let obj = { a: 1, b: 2, c: 3 };
test('object property access', obj.a, 1);
test('object bracket access', obj['b'], 2);
test('object property count', Object.keys(obj).length, 3);

obj.d = 4;
test('add property', obj.d, 4);

obj.a = 10;
test('modify property', obj.a, 10);

delete obj.d;
test('delete property', obj.d, undefined);

test('in operator true', 'a' in obj, true);
test('in operator false', 'z' in obj, false);

testDeep('Object.keys', Object.keys({ x: 1, y: 2 }), ['x', 'y']);
testDeep('Object.values', Object.values({ x: 1, y: 2 }), [1, 2]);
testDeep('Object.entries', Object.entries({ x: 1 }), [['x', 1]]);

let merged = Object.assign({}, { a: 1 }, { b: 2 });
test('Object.assign a', merged.a, 1);
test('Object.assign b', merged.b, 2);

let nested = { a: { b: { c: 1 } } };
test('nested access', nested.a.b.c, 1);

let computed = { ['key' + 1]: 'value' };
test('computed property', computed.key1, 'value');

let short = 'hello';
let shorthand = { short };
test('shorthand property', shorthand.short, 'hello');

let methodObj = {
  value: 42,
  getValue() {
    return this.value;
  }
};
test('method shorthand', methodObj.getValue(), 42);

let source = { a: 1, b: 2 };
let spread = { ...source, c: 3 };
test('spread a', spread.a, 1);
test('spread c', spread.c, 3);

let { a, b } = { a: 1, b: 2, c: 3 };
test('destructuring a', a, 1);
test('destructuring b', b, 2);

let { x: renamed } = { x: 5 };
test('destructuring rename', renamed, 5);

let { y = 10 } = {};
test('destructuring default', y, 10);

test('hasOwnProperty true', ({ a: 1 }).hasOwnProperty('a'), true);
test('hasOwnProperty false', ({ a: 1 }).hasOwnProperty('b'), false);

let frozen = Object.freeze({ a: 1 });
test('Object.isFrozen', Object.isFrozen(frozen), true);

let sealed = Object.seal({ a: 1 });
test('Object.isSealed', Object.isSealed(sealed), true);

let proto = { inherited: true };
let child = Object.create(proto);
test('Object.create inherits', child.inherited, true);

test('Object.getPrototypeOf', Object.getPrototypeOf([]) === Array.prototype, true);

testDeep('Object.fromEntries', Object.fromEntries([['a', 1], ['b', 2]]), { a: 1, b: 2 });

summary();
