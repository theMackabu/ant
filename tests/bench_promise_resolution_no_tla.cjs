const asyncIterations = Number(process.argv[2] || 25_000);
const syncIterations = Number(process.argv[3] || 2_000_000);
const rounds = Number(process.argv[4] || 7);
const selectedCase = process.argv[5] || 'all';
const benchmarkCases = [
  'await-native',
  'resolve-native',
  'ordinary-store',
  'create-primitive',
  'await-primitive',
  'resolve-native-executor',
];
// Select one case to compare candidate and baseline in separate fresh processes.

if (
  !(asyncIterations > 0) ||
  !(syncIterations > 0) ||
  !(rounds > 0) ||
  (selectedCase !== 'all' && !benchmarkCases.includes(selectedCase))
) {
  throw new Error(
    "usage: bench_promise_resolution_no_tla.cjs [async iterations] [sync iterations] [rounds] [case]",
  );
}

function runs(name) {
  return selectedCase === 'all' || selectedCase === name;
}

const now =
  typeof performance !== "undefined" && typeof performance.now === "function"
    ? () => performance.now()
    : () => Date.now();

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

function printResult(name, iterations, samples) {
  console.log(
    name +
      ": median=" +
      median(samples).toFixed(2) +
      "ms iterations=" +
      iterations +
      " samples=" +
      samples.map(value => value.toFixed(2)).join(","),
  );
}

function checkResult(name, actual, expected) {
  if (actual !== expected) {
    throw new Error(name + " result mismatch: expected " + expected + ", got " + actual);
  }
}

function benchSync(name, fn, expected) {
  const warmupIterations = Math.min(syncIterations, 1_000);
  for (let i = 0; i < 150; i++) {
    checkResult(name + " warmup", fn(warmupIterations), expected(warmupIterations));
  }

  const samples = [];
  for (let round = 0; round < rounds; round++) {
    const start = now();
    const result = fn(syncIterations);
    samples.push(now() - start);
    checkResult(name, result, expected(syncIterations));
  }

  printResult(name, syncIterations, samples);
}

async function benchAsync(name, fn, expected) {
  const warmupIterations = Math.min(asyncIterations, 256);
  for (let i = 0; i < 150; i++) {
    checkResult(name + " warmup", await fn(warmupIterations), expected(warmupIterations));
  }

  const samples = [];
  for (let round = 0; round < rounds; round++) {
    const start = now();
    const result = await fn(asyncIterations);
    samples.push(now() - start);
    checkResult(name, result, expected(asyncIterations));
  }

  printResult(name, asyncIterations, samples);
}

// Alternating avoids adding a new handler to the Promise whose handlers are
// currently being processed, so this benchmark does not require reentrant
// same-Promise handler preservation.
const nativePromise0 = Promise.resolve(1);
const nativePromise1 = Promise.resolve(1);
let promiseSink = nativePromise0;

async function awaitNativePromise(iterations) {
  let sum = 0;
  let i = 0;
  for (; i + 1 < iterations; i += 2) {
    sum += await nativePromise0;
    sum += await nativePromise1;
  }
  if (i < iterations) sum += await nativePromise0;
  return sum;
}

function resolveNativePromise(iterations) {
  const nativePromise = nativePromise0;
  let matches = 0;
  for (let i = 0; i < iterations; i++) {
    promiseSink = Promise.resolve(nativePromise);
    if (promiseSink === nativePromise) matches++;
  }
  return matches;
}

const storeTarget = { value: 0 };

function storeOrdinaryProperty(iterations) {
  for (let i = 0; i < iterations; i++) storeTarget.value = i;
  return storeTarget.value;
}

function createPromise(iterations) {
  for (let i = 0; i < iterations; i++) promiseSink = Promise.resolve(i);
  return iterations;
}

async function awaitPrimitive(iterations) {
  let sum = 0;
  for (let i = 0; i < iterations; i++) sum += await 1;
  return sum;
}

function resolveWithNativePromise0(resolve) {
  resolve(nativePromise0);
}

function resolveWithNativePromise1(resolve) {
  resolve(nativePromise1);
}

async function resolvingFunctionWithNativePromise(iterations) {
  let sum = 0;
  let i = 0;
  for (; i + 1 < iterations; i += 2) {
    sum += await new Promise(resolveWithNativePromise0);
    sum += await new Promise(resolveWithNativePromise1);
  }
  if (i < iterations) sum += await new Promise(resolveWithNativePromise0);
  return sum;
}

async function runBenchmarks() {
  console.log(
    "Promise resolution benchmark (no TLA): async=" +
      asyncIterations +
      " sync=" +
      syncIterations +
      " rounds=" +
      rounds +
      " case=" +
      selectedCase,
  );

  if (runs('await-native')) {
    await benchAsync("await native Promise", awaitNativePromise, n => n);
  }
  if (runs('resolve-native')) {
    benchSync("Promise.resolve(native Promise)", resolveNativePromise, n => n);
  }
  if (runs('ordinary-store')) {
    benchSync("ordinary JIT property store", storeOrdinaryProperty, n => n - 1);
  }
  if (runs('await-primitive')) {
    await benchAsync("await primitive", awaitPrimitive, n => n);
  }
  if (runs('resolve-native-executor')) {
    await benchAsync(
      "resolving function with native Promise",
      resolvingFunctionWithNativePromise,
      n => n,
    );
  }
  if (runs('create-primitive')) {
    benchSync("Promise.resolve(primitive) creation", createPromise, n => n);
  }

  if (!(promiseSink instanceof Promise)) throw new Error("promise sink was not retained");
}

runBenchmarks().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exitCode = 1;
});
