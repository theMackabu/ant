function fibonacci(n, memo = {}) {
  if (n in memo) return memo[n];
  if (n <= 1) return n;
  memo[n] = fibonacci(n - 1, memo) + fibonacci(n - 2, memo);
  return memo[n];
}

const start = performance.now();
const result = fibonacci(40);
const end = performance.now();

console.log(`fibonacci(40) = ${result}`);
console.log(`Time: ${(end - start).toFixed(2)} ms`);
