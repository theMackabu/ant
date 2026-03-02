// Test self-tail-call optimization in JIT
// Without the optimization, deep recursion blows the native stack.

// 1. Simple countdown — verifies correctness + no stack overflow
function countdown(n) {
  if (n <= 0) return 0;
  return countdown(n - 1);
}

// 2. Accumulator pattern — tail-recursive sum
function sum(n, acc) {
  if (n <= 0) return acc;
  return sum(n - 1, acc + n);
}

// 3. Mutual-arg swap (ensures args are copied correctly, not aliased)
function gcd(a, b) {
  if (b === 0) return a;
  return gcd(b, a % b);
}

// 4. Deep recursion that would stack-overflow without TCO
const DEEP = 1_000_000;

console.time("countdown 1M");
const r1 = countdown(DEEP);
console.timeEnd("countdown 1M");
console.log("countdown:", r1);

console.time("sum 1M");
const r2 = sum(DEEP, 0);
console.timeEnd("sum 1M");
console.log("sum:", r2);

console.time("gcd");
const r3 = gcd(1071, 462);
console.timeEnd("gcd");
console.log("gcd(1071,462):", r3);

// 5. Stress: 10M iterations to measure loop vs call overhead
console.time("countdown 10M");
countdown(10_000_000);
console.timeEnd("countdown 10M");

// 6. Closure-based self-tail-call (upval self-reference)
const fib_tail = function f(n, a, b) {
  if (n <= 0) return a;
  return f(n - 1, b, a + b);
};

console.time("fib_tail 100");
const r4 = fib_tail(100, 0, 1);
console.timeEnd("fib_tail 100");
console.log("fib_tail(100):", r4);

console.time("fib_tail 1M");
const r5 = fib_tail(1_000_000, 0, 1);
console.timeEnd("fib_tail 1M");
console.log("fib_tail(1M):", typeof r5 === "number" ? "ok" : "FAIL");
