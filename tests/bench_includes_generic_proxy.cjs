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

const generic = {
  get length() {
    return 1024;
  },
  get 1023() {
    return 1;
  }
};

const proxy = new Proxy(
  { length: 4, 0: NaN, 1: 0, 2: NaN, 3: 1 },
  {
    get(target, key) {
      return target[key];
    }
  }
);

runBench("generic array-like with getter", () => {
  let hits = 0;
  for (let r = 0; r < 20000; r++) {
    if ([].includes.call(generic, 1, 1023)) hits++;
  }
  return hits;
});

runBench("proxy includes", () => {
  let hits = 0;
  for (let r = 0; r < 20000; r++) {
    if (Array.prototype.includes.call(proxy, NaN, 1)) hits++;
  }
  return hits;
});
