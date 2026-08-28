const iterations = Number(process.argv[2] || 100_000);
const rounds = Number(process.argv[3] || 11);
const selectedCase = process.argv[4] || 'all';
const benchmarkCases = [
  'sync-array',
  'sync-generic',
  'async-sync-result',
  'async-fulfilled-result',
];

if (
  !Number.isSafeInteger(iterations) || iterations <= 0 ||
  !Number.isSafeInteger(rounds) || rounds <= 0 ||
  (selectedCase !== 'all' && !benchmarkCases.includes(selectedCase))
) {
  throw new Error(
    'usage: bench_for_of_completion.mjs [iterations] [rounds] [case]',
  );
}

const doneResult = { done: true, value: undefined };
const fulfilledDoneResult = Promise.resolve(doneResult);
const emptyArray = [];
const syncIterator = {
  next() {
    return doneResult;
  },
  [Symbol.iterator]() {
    return this;
  },
};
const asyncSyncResultIterator = {
  next() {
    return doneResult;
  },
  [Symbol.asyncIterator]() {
    return this;
  },
};
const asyncFulfilledResultIterator = {
  next() {
    return fulfilledDoneResult;
  },
  [Symbol.asyncIterator]() {
    return this;
  },
};

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

function completeSyncArray(count) {
  let completed = 0;
  for (let i = 0; i < count; i++) {
    for (const _value of emptyArray) {}
    completed++;
  }
  return completed;
}

function completeSyncGeneric(count) {
  let completed = 0;
  for (let i = 0; i < count; i++) {
    for (const _value of syncIterator) {}
    completed++;
  }
  return completed;
}

async function completeAsyncSyncResult(count) {
  let completed = 0;
  for (let i = 0; i < count; i++) {
    for await (const _value of asyncSyncResultIterator) {}
    completed++;
  }
  return completed;
}

async function completeAsyncFulfilledResult(count) {
  let completed = 0;
  for (let i = 0; i < count; i++) {
    for await (const _value of asyncFulfilledResultIterator) {}
    completed++;
  }
  return completed;
}

function benchmarkFunction(name) {
  if (name === 'sync-array') return completeSyncArray;
  if (name === 'sync-generic') return completeSyncGeneric;
  if (name === 'async-sync-result') return completeAsyncSyncResult;
  return completeAsyncFulfilledResult;
}

async function benchmark(name) {
  const fn = benchmarkFunction(name);
  const warmupIterations = Math.min(iterations, 2_000);
  for (let i = 0; i < 15; i++) {
    const completed = await fn(warmupIterations);
    if (completed !== warmupIterations) throw new Error(`${name} warmup mismatch`);
  }

  const samples = [];
  for (let round = 0; round < rounds; round++) {
    const start = performance.now();
    const completed = await fn(iterations);
    samples.push(performance.now() - start);
    if (completed !== iterations) throw new Error(`${name} result mismatch`);
  }

  const medianMs = median(samples);
  console.log(
    `${name}: median=${medianMs.toFixed(3)}ms ` +
    `ns/completion=${((medianMs * 1e6) / iterations).toFixed(2)} ` +
    `samples=${samples.map(sample => sample.toFixed(3)).join(',')}`,
  );
}

for (const name of benchmarkCases) {
  if (selectedCase === 'all' || selectedCase === name) await benchmark(name);
}
