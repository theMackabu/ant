function fib(n) {
  let a = 0n;
  let b = 1n;

  for (let i = 0; i < n; i++) {
    const next = a + b;
    a = b;
    b = next;
  }

  return a;
}

const start = performance.now();
const result = fib(5000);
const end = performance.now();

console.log(`fibonacci(5000) = ${result}`);
console.log(`Time: ${(end - start).toFixed(4)} ms (${((end - start) * 1000).toFixed(2)} µs)`);
