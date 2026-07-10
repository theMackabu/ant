const iterations = Number(process.argv[2] || 10_000_000);
const rounds = Number(process.argv[3] || 7);

function choose(value, fallback) {
  return value ?? fallback;
}

function loopDirect(value, n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += value;
  return sum;
}

function loopCoalesce(value, fallback, n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += value ?? fallback;
  return sum;
}

function loopInlineCoalesce(value, fallback, n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += choose(value, fallback);
  return sum;
}

function loopAlternating(fallback, n) {
  let value = null;
  let sum = 0;
  for (let i = 0; i < n; i++) {
    value = value === null ? 7 : null;
    sum += value ?? fallback;
  }
  return sum;
}

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

function bench(name, fn, expected) {
  for (let i = 0; i < 150; i++) {
    if (fn(100) !== expected(100)) throw new Error(name + " warmup mismatch");
  }

  const samples = [];
  for (let round = 0; round < rounds; round++) {
    const start = Date.now();
    const result = fn(iterations);
    const elapsed = Date.now() - start;
    if (result !== expected(iterations)) throw new Error(name + " result mismatch");
    samples.push(elapsed);
  }

  console.log(name + ": median=" + median(samples) + "ms samples=" + samples.join(","));
}

console.log("JMP_NOT_NULLISH benchmark: " + iterations + " iterations x " + rounds + " rounds");
bench("direct value baseline", n => loopDirect(7, n), n => n * 7);
bench("coalesce value", n => loopCoalesce(7, 3, n), n => n * 7);
bench("coalesce zero", n => loopCoalesce(0, 3, n), () => 0);
bench("coalesce null", n => loopCoalesce(null, 3, n), n => n * 3);
bench("coalesce undefined", n => loopCoalesce(undefined, 3, n), n => n * 3);
bench(
  "coalesce alternating",
  n => loopAlternating(3, n),
  n => Math.ceil(n / 2) * 7 + Math.floor(n / 2) * 3,
);
bench("inline coalesce value", n => loopInlineCoalesce(7, 3, n), n => n * 7);
bench("inline coalesce null", n => loopInlineCoalesce(null, 3, n), n => n * 3);
