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

function ternaryBothBare(n) {
  return n <= 0 ? ternaryBothBare_done(n) : ternaryBothBare(n - 1);
}
function ternaryBothBare_done(_) {
  return 'done';
}
test('ternary both bare calls', ternaryBothBare(100000), 'done');

function ternaryThenBinop(n) {
  return n <= 0 ? identity(0) + 1 : ternaryThenBinop(n - 1);
}
function identity(x) {
  return x;
}
test('ternary then-branch binop', ternaryThenBinop(5), 1);

function ternaryElseBinop(n) {
  return n <= 0 ? identity(42) : identity(n) * 2;
}
test('ternary else-branch binop', ternaryElseBinop(0), 42);
test('ternary else-branch binop non-zero', ternaryElseBinop(3), 6);

function ternaryBothBinop(n) {
  return n <= 0 ? identity(10) + 5 : identity(n) - 1;
}
test('ternary both branches binop', ternaryBothBinop(0), 15);
test('ternary both branches binop non-zero', ternaryBothBinop(7), 6);

function ternaryNestedArith(n) {
  return n <= 0 ? (identity(2) + 3) * identity(4) : ternaryNestedArith(n - 1);
}
test('ternary nested arith in then', ternaryNestedArith(5), 20);

function ternaryParenBareCall(n) {
  return n <= 0 ? ternaryParenBareCall_end() : ternaryParenBareCall(n - 1);
}
function ternaryParenBareCall_end() {
  return 'end';
}
test('ternary paren bare call', ternaryParenBareCall(100000), 'end');

function ternaryCallWithBinopArg(n) {
  return n <= 0 ? identity(2 + 3) : ternaryCallWithBinopArg(n - 1);
}
test('ternary call with binop arg', ternaryCallWithBinopArg(100000), 5);

function ternaryNested(n, branch) {
  return branch === 0 ? ternaryNested_a(n) : branch === 1 ? ternaryNested_b(n) : ternaryNested_c(n);
}
function ternaryNested_a(_) {
  return 'a';
}
function ternaryNested_b(_) {
  return 'b';
}
function ternaryNested_c(_) {
  return 'c';
}
test('nested ternary branch 0', ternaryNested(1, 0), 'a');
test('nested ternary branch 1', ternaryNested(1, 1), 'b');
test('nested ternary branch 2', ternaryNested(1, 2), 'c');

function ternaryNestedUnsafe(n, branch) {
  return branch === 0 ? identity(n) + 1 : branch === 1 ? identity(n) : identity(n) * 3;
}
test('nested ternary unsafe then', ternaryNestedUnsafe(5, 0), 6);
test('nested ternary safe middle', ternaryNestedUnsafe(5, 1), 5);
test('nested ternary unsafe else', ternaryNestedUnsafe(5, 2), 15);

summary();
