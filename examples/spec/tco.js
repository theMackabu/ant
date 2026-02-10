import { test, summary } from './helpers.js';

console.log('Tail Call Optimization Tests\n');

function factorial(n, acc = 1) {
  if (n <= 1) return acc;
  return factorial(n - 1, n * acc);
}
test('tail factorial(10)', factorial(10), 3628800);
test('tail factorial(20)', factorial(20), 2432902008176640000);

function countDown(n) {
  if (n <= 0) return 'done';
  return countDown(n - 1);
}
test('deep tail recursion', countDown(100000), 'done');

function sum(n, acc = 0) {
  if (n <= 0) return acc;
  return sum(n - 1, acc + n);
}
test('tail sum(100000)', sum(100000), 5000050000);

function fib(n, a = 0, b = 1) {
  if (n === 0) return a;
  if (n === 1) return b;
  return fib(n - 1, b, a + b);
}
test('tail fib(10)', fib(10), 55);
test('tail fib(30)', fib(30), 832040);

function isEven(n) {
  if (n === 0) return true;
  return isOdd(n - 1);
}
function isOdd(n) {
  if (n === 0) return false;
  return isEven(n - 1);
}
test('mutual isEven(10)', isEven(10), true);
test('mutual isOdd(11)', isOdd(11), true);
test('mutual isEven(10000)', isEven(10000), true);

function normalFib(n) {
  if (n <= 1) return n;
  return normalFib(n - 1) + normalFib(n - 2);
}
test('non-tail recursion', normalFib(10), 55);

function normalFactorial(n) {
  if (n <= 1) return 1;
  return n * normalFactorial(n - 1);
}
test('non-tail factorial', normalFactorial(5), 120);

function tailMultiArg(a, b, c) {
  if (a <= 0) return b + c;
  return tailMultiArg(a - 1, b + 1, c + 2);
}
test('tail multi-arg', tailMultiArg(1000, 0, 0), 3000);

function ternaryTail(n) {
  return n <= 0 ? 'done' : ternaryTail(n - 1);
}
test('ternary tail', ternaryTail(50000), 'done');

function lastElement(arr, i = 0) {
  if (i >= arr.length - 1) return arr[i];
  return lastElement(arr, i + 1);
}
test('tail lastElement', lastElement([1, 2, 3, 4, 5]), 5);

summary();
