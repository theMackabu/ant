import { test, testDeep, summary } from './helpers.js';

console.log('Spread Tests\n');

const arr1 = [1, 2, 3];
const arr2 = [...arr1, 4, 5];
testDeep('spread array', arr2, [1, 2, 3, 4, 5]);

const arr3 = [0, ...arr1];
testDeep('spread at start', arr3, [0, 1, 2, 3]);

const arr4 = [0, ...arr1, 4];
testDeep('spread in middle', arr4, [0, 1, 2, 3, 4]);

const copy = [...arr1];
testDeep('array copy', copy, [1, 2, 3]);
test('array copy is new ref', copy !== arr1, true);

const merged = [...[1, 2], ...[3, 4]];
testDeep('merge arrays', merged, [1, 2, 3, 4]);

const obj1 = { a: 1, b: 2 };
const obj2 = { ...obj1, c: 3 };
test('spread object a', obj2.a, 1);
test('spread object c', obj2.c, 3);

const obj3 = { ...obj1, b: 10 };
test('spread override', obj3.b, 10);

const objCopy = { ...obj1 };
test('object copy a', objCopy.a, 1);
test('object copy is new ref', objCopy !== obj1, true);

const mergedObj = { ...{ x: 1 }, ...{ y: 2 } };
test('merge objects x', mergedObj.x, 1);
test('merge objects y', mergedObj.y, 2);

function sum(a, b, c) {
  return a + b + c;
}
test('spread in call', sum(...[1, 2, 3]), 6);

const chars = [...'hello'];
testDeep('spread string', chars, ['h', 'e', 'l', 'l', 'o']);

summary();
