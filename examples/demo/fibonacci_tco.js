function fib(n, a = 0, b = 1) {
  if (n === 0) return a;
  return fib(n - 1, b, a + b);
}

const start = performance.now();
const result = fib(30);
const end = performance.now();

console.log(`fibonacci(30) = ${result}`);
console.log(`Time: ${(end - start).toFixed(2)} ms`);
