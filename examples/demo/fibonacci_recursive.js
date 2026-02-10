function fib(n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}

const start = performance.now();
const result = fib(30);
const end = performance.now();

console.log(`fibonacci(30) = ${result}`);
console.log(`Time: ${(end - start).toFixed(2)} ms`);
