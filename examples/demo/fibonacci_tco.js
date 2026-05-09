function fib(n, a = 0, b = 1) {
  if (n === 0) return a;
  return fib(n - 1, b, a + b);
}

let start = performance.now();
const result = fib(35);
const singleShotMs = performance.now() - start;

const warmup = 10000;
const iterations = 200000;

for (let i = 0; i < warmup; i++) fib(35);

let sink = 0;
start = performance.now();
for (let i = 0; i < iterations; i++) sink += i & 1;
const loopOverhead = performance.now() - start;

start = performance.now();
for (let i = 0; i < iterations; i++) sink += fib(35);
const elapsed = performance.now() - start;

const net = Math.max(0, elapsed - loopOverhead);
const perCallUs = (net * 1000) / iterations;

console.log(`fibonacci(35) = ${result}`);
console.log(`Single-shot: ${singleShotMs.toFixed(4)} ms (${(singleShotMs * 1000).toFixed(2)} µs)`);
console.log(`Time: ${perCallUs.toFixed(4)} µs/call (${iterations} iterations)`);
