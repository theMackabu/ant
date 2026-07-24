const iterations = Number(process.argv[2] || 1_000_000);
const rounds = Number(process.argv[3] || 7);

function makeSmall(i) {
  return { a: i, b: i + 1, c: i + 2, d: i + 3 };
}

function makeContext(i) {
  return {
    request: i,
    store: i + 1,
    set: i + 2,
    path: i + 3,
    qi: i + 4,
    error: i + 5,
    redirect: i + 6,
    status: i + 7,
  };
}

function runSmall(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    const value = makeSmall(i);
    sum += value.a + value.d;
  }
  return sum;
}

function runContext(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    const context = makeContext(i);
    sum += context.request + context.redirect;
  }
  return sum;
}

function median(values) {
  const sorted = [];
  for (const value of values) {
    let index = sorted.length;
    while (index > 0 && sorted[index - 1] > value) index--;
    sorted.splice(index, 0, value);
  }
  return sorted[Math.floor(sorted.length / 2)];
}

function bench(name, fn, expected) {
  for (let i = 0; i < 150; i++) {
    if (fn(100) !== expected(100)) throw new Error(name + " warmup mismatch");
  }

  const samples = [];
  for (let round = 0; round < rounds; round++) {
    const start = performance.now();
    const result = fn(iterations);
    const elapsed = performance.now() - start;
    if (result !== expected(iterations)) throw new Error(name + " result mismatch");
    samples.push(elapsed);
  }

  console.log(
    name + ": median=" + median(samples).toFixed(2) +
    "ms samples=" + samples.map(value => value.toFixed(2)).join(",")
  );
}

console.log("object literal JIT benchmark: " + iterations + " iterations x " + rounds + " rounds");
bench("4 fields (in-object)", runSmall, n => n * n + 2 * n);
bench("8 fields (context)", runContext, n => n * n + 5 * n);
