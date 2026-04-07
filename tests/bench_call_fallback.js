// Benchmark: interpreter fallback-call overhead.
//
// This targets the paths in silver/engine.c that call sv_vm_call(...) and then
// continue executing in the current frame. It is useful for tracking the cost
// of refreshing cached frame/stack locals after re-entrant native calls.

const now =
  typeof performance !== "undefined" && performance && typeof performance.now === "function"
    ? () => performance.now()
    : () => Date.now();

let sink = 0;

function bench(name, iterations, fn) {
  for (let i = 0; i < 3; i++) fn(iterations >> 3 || 1);

  const start = now();
  const ops = fn(iterations);
  const elapsed = now() - start;
  const nsPerOp = (elapsed * 1e6) / ops;
  const opsPerSec = (ops * 1000) / elapsed;

  console.log(
    name +
      ": " +
      elapsed.toFixed(2) +
      "ms (" +
      ops +
      " ops, " +
      nsPerOp.toFixed(2) +
      " ns/op, " +
      opsPerSec.toFixed(0) +
      " ops/s)"
  );
}

function jsAdd(a, b) {
  return a + b;
}

bench("js direct call", 5000000, function(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += jsAdd(i, 1);
  sink = sum;
  return n;
});

bench("native method Math.abs", 5000000, function(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += Math.abs(-i);
  sink = sum;
  return n;
});

bench("native method Math.imul", 5000000, function(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += Math.imul(i, 3);
  sink = sum;
  return n;
});

bench("native method array push/pop", 3000000, function(n) {
  const arr = [];
  let sum = 0;
  for (let i = 0; i < n; i++) {
    arr.push(i);
    sum += arr.pop();
  }
  sink = sum + arr.length;
  return n * 2;
});

bench("native method string charCodeAt", 5000000, function(n) {
  const str = "fallback";
  let sum = 0;
  for (let i = 0; i < n; i++) sum += str.charCodeAt(i & 7);
  sink = sum;
  return n;
});

bench("native method Date.now", 1000000, function(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += Date.now() & 1;
  sink = sum;
  return n;
});

console.log("sink:", sink);
