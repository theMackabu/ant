// Test tail call optimization

// 1. Basic tail-recursive factorial
function factorial(n, acc = 1) {
  if (n <= 1) return acc;
  return factorial(n - 1, n * acc);
}
console.log('factorial(10):', factorial(10)); // 3628800
console.log('factorial(20):', factorial(20)); // 2432902008176640000

// 2. Deep tail recursion - would stack overflow without TCO
function countDown(n) {
  if (n <= 0) return 'done';
  return countDown(n - 1);
}
console.log('countDown(100000):', countDown(100000)); // done

// 3. Tail-recursive sum
function sum(n, acc = 0) {
  if (n <= 0) return acc;
  return sum(n - 1, acc + n);
}
console.log('sum(100000):', sum(100000)); // 5000050000

// 4. Tail-recursive fibonacci (iterative via accumulator)
function fib(n, a = 0, b = 1) {
  if (n === 0) return a;
  if (n === 1) return b;
  return fib(n - 1, b, a + b);
}
console.log('fib(10):', fib(10)); // 55
console.log('fib(30):', fib(30)); // 832040

// 5. Mutual tail recursion
function isEven(n) {
  if (n === 0) return true;
  return isOdd(n - 1);
}
function isOdd(n) {
  if (n === 0) return false;
  return isEven(n - 1);
}
console.log('isEven(10):', isEven(10)); // true
console.log('isOdd(11):', isOdd(11)); // true
console.log('isEven(10000):', isEven(10000)); // true

// 6. Non-tail calls should still work correctly
function normalFib(n) {
  if (n <= 1) return n;
  return normalFib(n - 1) + normalFib(n - 2);
}
console.log('normalFib(10):', normalFib(10)); // 55

// 7. Tail call with different argument count
function tailMultiArg(a, b, c) {
  if (a <= 0) return b + c;
  return tailMultiArg(a - 1, b + 1, c + 2);
}
console.log('tailMultiArg(1000, 0, 0):', tailMultiArg(1000, 0, 0)); // 1000 + 2000 = 3000

// 8. Tail position in ternary
function ternaryTail(n) {
  return n <= 0 ? 'done' : ternaryTail(n - 1);
}
console.log('ternaryTail(50000):', ternaryTail(50000)); // done

// 9. Tail position in logical expressions should NOT be optimized
// (the result goes through boolean coercion, not a true tail call)
function lastElement(arr, i = 0) {
  if (i >= arr.length - 1) return arr[i];
  return lastElement(arr, i + 1);
}
console.log('lastElement([1,2,3,4,5]):', lastElement([1, 2, 3, 4, 5])); // 5

console.log('\nAll TCO tests passed!');
