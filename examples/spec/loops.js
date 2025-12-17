import { test, testDeep, summary } from './helpers.js';

console.log('Loop Tests\n');

let sum = 0;
for (let i = 0; i < 5; i++) sum += i;
test('for loop sum', sum, 10);

let nested = 0;
for (let i = 0; i < 3; i++) {
  for (let j = 0; j < 3; j++) {
    nested++;
  }
}
test('nested for loops', nested, 9);

function findFirst(arr, target) {
  for (let i = 0; i < arr.length; i++) {
    if (arr[i] === target) return i;
  }
  return -1;
}
test('for loop return', findFirst([10, 20, 30], 20), 1);
test('for loop return not found', findFirst([10, 20, 30], 99), -1);

let breakSum = 0;
for (let i = 0; i < 10; i++) {
  if (i === 5) break;
  breakSum += i;
}
test('for loop break', breakSum, 10);

let evenSum = 0;
for (let i = 0; i < 10; i++) {
  if (i % 2 === 1) continue;
  evenSum += i;
}
test('for loop continue', evenSum, 20);

let k = 0;
let extCount = 0;
for (; k < 5; k++) extCount++;
test('for loop external init', extCount, 5);

let countdown = 0;
for (let i = 5; i > 0; i--) countdown += i;
test('for loop decrement', countdown, 15);

let arr = [1, 2, 3, 4, 5];
for (let i = 0; i < arr.length; i++) arr[i] *= 2;
testDeep('for loop modify array', arr, [2, 4, 6, 8, 10]);

let str = 'hello';
let chars = '';
for (let i = 0; i < str.length; i++) chars += str[i];
test('for loop string', chars, 'hello');

let whileSum = 0;
let j = 0;
while (j < 5) {
  whileSum += j;
  j++;
}
test('while loop', whileSum, 10);

let doSum = 0;
let d = 0;
do {
  doSum += d;
  d++;
} while (d < 5);
test('do-while loop', doSum, 10);

const obj = { a: 1, b: 2, c: 3 };
let keys = [];
for (let key in obj) keys.push(key);
test('for-in keys length', keys.length, 3);
test('for-in has a', keys.includes('a'), true);

const items = [10, 20, 30];
let forOfSum = 0;
for (let item of items) forOfSum += item;
test('for-of sum', forOfSum, 60);

let forOfChars = '';
for (let c of 'abc') forOfChars += c;
test('for-of string', forOfChars, 'abc');

let largeSum = 0;
for (let i = 0; i < 1000; i++) largeSum += i;
test('large loop', largeSum, 499500);

summary();
