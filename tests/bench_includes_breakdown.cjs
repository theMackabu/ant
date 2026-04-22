function runBench(label, iterations, fn, options) {
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

  const mean = total / samples;
  const opsPerMs = iterations / mean;

  console.log(label);
  console.log(`  iterations: ${iterations}`);
  console.log(`  result: ${last}`);
  console.log(`  mean: ${mean.toFixed(3)} ms`);
  console.log(`  min: ${min.toFixed(3)} ms`);
  console.log(`  max: ${max.toFixed(3)} ms`);
  console.log(`  ops/ms: ${opsPerMs.toFixed(1)}`);
}

const dense = [];
for (let i = 0; i < 1024; i++) dense.push(i);

const sparse = Array(1024);

const genericOwn = {
  get length() {
    return 1024;
  },
  get 1023() {
    return 1;
  }
};

const genericProtoBase = { 1023: 1 };
const genericProto = Object.create(genericProtoBase);
Object.defineProperty(genericProto, "length", {
  configurable: true,
  enumerable: true,
  get() {
    return 1024;
  }
});

const proxy = new Proxy(
  { length: 4, 0: NaN, 1: 0, 2: NaN, 3: 1 },
  {
    get(target, key) {
      return target[key];
    }
  }
);

const typed = new Uint32Array(1024);
typed[1023] = 1;

runBench("dense hit tail", 20000, () => {
  let hits = 0;
  for (let r = 0; r < 20000; r++) {
    if (dense.includes(1023)) hits++;
  }
  return hits;
});

runBench("dense miss", 20000, () => {
  let hits = 0;
  for (let r = 0; r < 20000; r++) {
    if (dense.includes(-1)) hits++;
  }
  return hits;
});

runBench("dense hit negative fromIndex", 20000, () => {
  let hits = 0;
  for (let r = 0; r < 20000; r++) {
    if (dense.includes(1023, -1)) hits++;
  }
  return hits;
});

runBench("sparse includes(undefined)", 50000, () => {
  let hits = 0;
  for (let r = 0; r < 50000; r++) {
    if (sparse.includes(undefined)) hits++;
  }
  return hits;
});

runBench("sparse miss non-undefined", 50000, () => {
  let hits = 0;
  for (let r = 0; r < 50000; r++) {
    if (sparse.includes(1)) hits++;
  }
  return hits;
});

runBench("generic own getter", 20000, () => {
  let hits = 0;
  for (let r = 0; r < 20000; r++) {
    if ([].includes.call(genericOwn, 1, 1023)) hits++;
  }
  return hits;
});

runBench("generic inherited value", 20000, () => {
  let hits = 0;
  for (let r = 0; r < 20000; r++) {
    if ([].includes.call(genericProto, 1, 1023)) hits++;
  }
  return hits;
});

runBench("proxy NaN fromIndex", 20000, () => {
  let hits = 0;
  for (let r = 0; r < 20000; r++) {
    if (Array.prototype.includes.call(proxy, NaN, 1)) hits++;
  }
  return hits;
});

runBench("typedarray hit", 50000, () => {
  let hits = 0;
  for (let r = 0; r < 50000; r++) {
    if (typed.includes(1)) hits++;
  }
  return hits;
});

runBench("typedarray miss", 50000, () => {
  let hits = 0;
  for (let r = 0; r < 50000; r++) {
    if (typed.includes(2)) hits++;
  }
  return hits;
});
