function runBench(label, fn, options) {
  const warmup = options && options.warmup ? options.warmup : 5;
  const samples = options && options.samples ? options.samples : 10;

  for (let i = 0; i < warmup; i++) fn();

  let min = Infinity;
  let max = -Infinity;
  let total = 0;
  let last = 0;

  for (let i = 0; i < samples; i++) {
    const start = performance.now();
    last = fn();
    const elapsed = performance.now() - start;
    if (elapsed < min) min = elapsed;
    if (elapsed > max) max = elapsed;
    total += elapsed;
  }

  console.log(label);
  console.log(`  result: ${last}`);
  console.log(`  mean: ${ (total / samples).toFixed(3) } ms`);
  console.log(`  min: ${ min.toFixed(3) } ms`);
  console.log(`  max: ${ max.toFixed(3) } ms`);
}

const arr = [];
for (let i = 0; i < 1024; i++) arr.push(i);

runBench("dense includes", () => {
  let hits = 0;
  for (let r = 0; r < 20000; r++) {
    if (arr.includes(1023)) hits++;
    if (arr.includes(-1)) hits++;
  }
  return hits;
});
