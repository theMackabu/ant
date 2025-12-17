import { test, testDeep, summary } from './helpers.js';

console.log('Array Tests\n');

let arr = [1, 2, 3];
test('array literal creates array', arr instanceof Array, true);
test('array index 0', arr[0], 1);
test('array index 1', arr[1], 2);
test('array index 2', arr[2], 3);
test('array length', arr.length, 3);

let empty = [];
test('empty array is array', empty instanceof Array, true);
test('empty array length', empty.length, 0);

let mixed = [1, 'hello', true, null];
test('mixed array length', mixed.length, 4);
test('mixed array string', mixed[1], 'hello');
test('mixed array bool', mixed[2], true);
test('mixed array null', mixed[3], null);

arr[0] = 10;
test('array assignment', arr[0], 10);

arr.push(4);
test('push adds element', arr[arr.length - 1], 4);
test('push increases length', arr.length, 4);

arr.push(5, 6);
test('push multiple', arr.length, 6);

let popped = arr.pop();
test('pop returns last', popped, 6);
test('pop decreases length', arr.length, 5);

let nested = [[1, 2], [3, 4]];
test('nested array access', nested[0][0], 1);
test('nested array access deep', nested[1][1], 4);

let arr2 = Array(3);
test('Array(n) creates sparse', arr2.length, 3);

let arr3 = Array(10, 20, 30);
test('Array with elements', arr3.length, 3);
test('Array with elements value', arr3[1], 20);

test('instanceof Array true', [1,2] instanceof Array, true);
test('instanceof Array false for object', ({}) instanceof Array, false);

let obj = [1, 2, 3];
obj['foo'] = 'bar';
test('array string key', obj.foo, 'bar');

let sumArr = [1, 2, 3, 4];
let sum = 0;
for (let i = 0; i < sumArr.length; i++) sum += sumArr[i];
test('array iteration sum', sum, 10);

let a = [1, 2, 3];
testDeep('concat arrays', a.concat([4, 5]), [1, 2, 3, 4, 5]);
test('join with comma', a.join(','), '1,2,3');
test('indexOf found', a.indexOf(2), 1);
test('indexOf not found', a.indexOf(5), -1);
test('includes true', a.includes(2), true);
test('includes false', a.includes(5), false);
testDeep('slice', a.slice(1, 3), [2, 3]);
testDeep('reverse', [1, 2, 3].reverse(), [3, 2, 1]);

let shifted = [1, 2, 3];
test('shift returns first', shifted.shift(), 1);
test('shift decreases length', shifted.length, 2);

let unshifted = [2, 3];
unshifted.unshift(1);
test('unshift adds to front', unshifted[0], 1);
test('unshift increases length', unshifted.length, 3);

testDeep('map', [1, 2, 3].map(x => x * 2), [2, 4, 6]);
testDeep('filter', [1, 2, 3, 4].filter(x => x % 2 === 0), [2, 4]);
test('reduce sum', [1, 2, 3].reduce((a, b) => a + b, 0), 6);
test('find', [1, 2, 3].find(x => x > 1), 2);
test('findIndex', [1, 2, 3].findIndex(x => x > 1), 1);
test('every true', [2, 4, 6].every(x => x % 2 === 0), true);
test('every false', [2, 3, 6].every(x => x % 2 === 0), false);
test('some true', [1, 2, 3].some(x => x > 2), true);
test('some false', [1, 2, 3].some(x => x > 5), false);

testDeep('Array.from string', Array.from('abc'), ['a', 'b', 'c']);
testDeep('Array.of', Array.of(1, 2, 3), [1, 2, 3]);
test('instanceof Array true', [] instanceof Array, true);
test('instanceof Array false', ({}) instanceof Array, false);

testDeep('flat', [1, [2, 3]].flat(), [1, 2, 3]);
testDeep('flatMap', [1, 2].flatMap(x => [x, x * 2]), [1, 2, 2, 4]);

let filled = new Array(3).fill(0);
testDeep('fill', filled, [0, 0, 0]);

let sorted = [3, 1, 2].sort();
testDeep('sort default', sorted, [1, 2, 3]);

let sortedDesc = [1, 2, 3].sort((a, b) => b - a);
testDeep('sort descending', sortedDesc, [3, 2, 1]);

summary();
