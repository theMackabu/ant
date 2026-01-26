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

let nested = [
  [1, 2],
  [3, 4]
];
test('nested array access', nested[0][0], 1);
test('nested array access deep', nested[1][1], 4);

let arr2 = Array(3);
test('Array(n) creates sparse', arr2.length, 3);

let arr3 = Array(10, 20, 30);
test('Array with elements', arr3.length, 3);
test('Array with elements value', arr3[1], 20);

test('instanceof Array true', [1, 2] instanceof Array, true);
test('instanceof Array false for object', {} instanceof Array, false);

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

testDeep(
  'map',
  [1, 2, 3].map(x => x * 2),
  [2, 4, 6]
);
testDeep(
  'filter',
  [1, 2, 3, 4].filter(x => x % 2 === 0),
  [2, 4]
);
test(
  'reduce sum',
  [1, 2, 3].reduce((a, b) => a + b, 0),
  6
);
test(
  'find',
  [1, 2, 3].find(x => x > 1),
  2
);
test(
  'findIndex',
  [1, 2, 3].findIndex(x => x > 1),
  1
);
test(
  'every true',
  [2, 4, 6].every(x => x % 2 === 0),
  true
);
test(
  'every false',
  [2, 3, 6].every(x => x % 2 === 0),
  false
);
test(
  'some true',
  [1, 2, 3].some(x => x > 2),
  true
);
test(
  'some false',
  [1, 2, 3].some(x => x > 5),
  false
);

testDeep('Array.from string', Array.from('abc'), ['a', 'b', 'c']);
testDeep('Array.of', Array.of(1, 2, 3), [1, 2, 3]);
test('instanceof Array true', [] instanceof Array, true);
test('instanceof Array false', {} instanceof Array, false);

testDeep('flat', [1, [2, 3]].flat(), [1, 2, 3]);
testDeep(
  'flatMap',
  [1, 2].flatMap(x => [x, x * 2]),
  [1, 2, 2, 4]
);

let filled = new Array(3).fill(0);
testDeep('fill', filled, [0, 0, 0]);

let sorted = [3, 1, 2].sort();
testDeep('sort default', sorted, [1, 2, 3]);

let sortedDesc = [1, 2, 3].sort((a, b) => b - a);
testDeep('sort descending', sortedDesc, [3, 2, 1]);

let orig = [3, 1, 4, 1, 5];
testDeep('toSorted returns sorted copy', orig.toSorted(), [1, 1, 3, 4, 5]);
testDeep('toSorted does not mutate', orig, [3, 1, 4, 1, 5]);
testDeep(
  'toSorted with compareFn',
  [3, 1, 2].toSorted((a, b) => b - a),
  [3, 2, 1]
);

testDeep('toReversed returns reversed copy', [1, 2, 3].toReversed(), [3, 2, 1]);
let orig2 = [1, 2, 3];
orig2.toReversed();
testDeep('toReversed does not mutate', orig2, [1, 2, 3]);

testDeep('toSpliced removes elements', [1, 2, 3, 4, 5].toSpliced(1, 2), [1, 4, 5]);
testDeep('toSpliced inserts elements', [1, 2, 3].toSpliced(1, 0, 99), [1, 99, 2, 3]);
testDeep('toSpliced replaces elements', [1, 2, 3, 4, 5].toSpliced(1, 2, 99), [1, 99, 4, 5]);
let orig3 = [1, 2, 3];
orig3.toSpliced(1, 1);
testDeep('toSpliced does not mutate', orig3, [1, 2, 3]);

testDeep('with replaces element', [1, 2, 3].with(1, 99), [1, 99, 3]);
testDeep('with negative index', [1, 2, 3].with(-1, 99), [1, 2, 99]);
let orig4 = [1, 2, 3];
orig4.with(0, 99);
testDeep('with does not mutate', orig4, [1, 2, 3]);

test(
  'findLast',
  [1, 2, 3, 2, 1].findLast(x => x === 2),
  2
);
test(
  'findLast returns last match',
  [1, 2, 3, 4, 5].findLast(x => x > 2),
  5
);
test(
  'findLast not found',
  [1, 2, 3].findLast(x => x > 5),
  undefined
);
test(
  'findLastIndex',
  [1, 2, 3, 2, 1].findLastIndex(x => x === 2),
  3
);
test(
  'findLastIndex not found',
  [1, 2, 3].findLastIndex(x => x > 5),
  -1
);

test('at positive', [1, 2, 3].at(1), 2);
test('at negative', [1, 2, 3].at(-1), 3);
test('at out of bounds', [1, 2, 3].at(5), undefined);

let forEachResult = [];
[1, 2, 3].forEach(x => forEachResult.push(x * 2));
testDeep('forEach', forEachResult, [2, 4, 6]);

test(
  'reduceRight',
  [1, 2, 3].reduceRight((acc, x) => acc + x, 0),
  6
);
test(
  'reduceRight order',
  ['a', 'b', 'c'].reduceRight((acc, x) => acc + x, ''),
  'cba'
);

test('lastIndexOf found', [1, 2, 3, 2, 1].lastIndexOf(2), 3);
test('lastIndexOf not found', [1, 2, 3].lastIndexOf(5), -1);

testDeep('copyWithin', [1, 2, 3, 4, 5].copyWithin(0, 3), [4, 5, 3, 4, 5]);
testDeep('copyWithin with end', [1, 2, 3, 4, 5].copyWithin(1, 3, 4), [1, 4, 3, 4, 5]);

let spliceArr = [1, 2, 3, 4, 5];
let removed = spliceArr.splice(1, 2);
testDeep('splice returns removed', removed, [2, 3]);
testDeep('splice mutates array', spliceArr, [1, 4, 5]);

let spliceArr2 = [1, 2, 3];
spliceArr2.splice(1, 0, 99, 100);
testDeep('splice insert', spliceArr2, [1, 99, 100, 2, 3]);

let entriesArr = ['a', 'b', 'c'];
let entriesResult = [];
for (let [i, v] of entriesArr.entries()) entriesResult.push([i, v]);
testDeep('entries', entriesResult, [
  [0, 'a'],
  [1, 'b'],
  [2, 'c']
]);

let keysResult = [];
for (let k of ['a', 'b', 'c'].keys()) keysResult.push(k);
testDeep('keys', keysResult, [0, 1, 2]);

let valuesResult = [];
for (let v of ['a', 'b', 'c'].values()) valuesResult.push(v);
testDeep('values', valuesResult, ['a', 'b', 'c']);

test('Array.isArray true', Array.isArray([1, 2, 3]), true);
test('Array.isArray false object', Array.isArray({}), false);
test('Array.isArray false string', Array.isArray('abc'), false);

summary();
