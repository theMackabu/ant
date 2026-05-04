function add(a, b) {
  return a + b;
}

function now() {
  if (typeof performance === 'object' && performance && typeof performance.now === 'function') {
    return performance.now();
  }
  return Date.now();
}

function benchEmpty(iterations) {
  let result = 0;
  for (let i = 0; i < iterations; i++) {
    result = i;
  }
  return result;
}

function benchInline(iterations) {
  let result = 0;
  for (let i = 0; i < iterations; i++) {
    result += i + (i + 1);
  }
  return result;
}

function benchCall(iterations) {
  let result = 0;
  for (let i = 0; i < iterations; i++) {
    result += add(i, i + 1);
  }
  return result;
}

function time(name, iterations, fn) {
  let start = now();
  let result = fn(iterations);
  let elapsed = now() - start;
  console.log(`${name}: ${elapsed.toFixed(3)}ms (result: ${result})`);
}

const iterations = Number(process.argv[2] || 2000000);
const warmupIterations = Math.min(iterations, 200000);

console.log(`Running ${iterations} iterations...`);

time('cold call add', iterations, benchCall);

benchEmpty(warmupIterations);
benchInline(warmupIterations);
benchCall(warmupIterations);

time('warm empty loop', iterations, benchEmpty);
time('warm inline add', iterations, benchInline);
time('warm call add', iterations, benchCall);
console.log('Done!');
