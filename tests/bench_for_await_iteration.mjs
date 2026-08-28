const iterations = Number(process.argv[2] || 250_000);
const rounds = Number(process.argv[3] || 9);
const selectedCase = process.argv[4] || 'all';
const benchmarkCases = [
  'sync-result',
  'fulfilled-promise',
  'async-next',
  'readable-stream',
  'sync-result-control',
  'fulfilled-promise-control',
  'async-next-control',
  'readable-stream-control',
];

if (
  !Number.isSafeInteger(iterations) || iterations <= 0 ||
  !Number.isSafeInteger(rounds) || rounds <= 0 ||
  (selectedCase !== 'all' && !benchmarkCases.includes(selectedCase))
) {
  throw new Error(
    'usage: bench_for_await_iteration.mjs [iterations] [rounds] [case]',
  );
}

const now = () => performance.now();

function median(values) {
  const sorted = values.slice();
  for (let i = 1; i < sorted.length; i++) {
    const value = sorted[i];
    let j = i - 1;
    while (j >= 0 && sorted[j] > value) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = value;
  }
  return sorted[Math.floor(sorted.length / 2)];
}

function makeIterator(kind, count) {
  let remaining = count;
  const valueResult = { done: false, value: 1 };
  const doneResult = { done: true, value: undefined };

  if (kind === 'sync-result') {
    return {
      next() {
        return remaining-- > 0 ? valueResult : doneResult;
      },
      [Symbol.asyncIterator]() {
        return this;
      },
    };
  }

  if (kind === 'fulfilled-promise') {
    const valuePromises = [Promise.resolve(valueResult), Promise.resolve(valueResult)];
    const donePromise = Promise.resolve(doneResult);
    return {
      next() {
        if (remaining-- <= 0) return donePromise;
        return valuePromises[remaining & 1];
      },
      [Symbol.asyncIterator]() {
        return this;
      },
    };
  }

  return {
    async next() {
      return remaining-- > 0 ? valueResult : doneResult;
    },
    [Symbol.asyncIterator]() {
      return this;
    },
  };
}

async function consume(kind, count) {
  let sum = 0;
  for await (const value of makeIterator(kind, count)) sum += value;
  return sum;
}

function consumeSyncResultControl(count) {
  const iterator = makeIterator('sync-result', count);
  let sum = 0;
  for (;;) {
    const result = iterator.next();
    if (result.done) return sum;
    sum += result.value;
  }
}

async function consumeAsyncControl(kind, count) {
  const iterator = makeIterator(kind, count);
  let sum = 0;
  for (;;) {
    const result = await iterator.next();
    if (result.done) return sum;
    sum += result.value;
  }
}

function makeReadableStream(count) {
  let remaining = count;
  return new ReadableStream({
    pull(controller) {
      if (remaining-- > 0) controller.enqueue(1);
      else controller.close();
    },
  });
}

async function consumeReadableStream(count) {
  let sum = 0;
  for await (const value of makeReadableStream(count)) sum += value;
  return sum;
}

async function consumeReadableStreamControl(count) {
  const reader = makeReadableStream(count).getReader();
  let sum = 0;
  for (;;) {
    const result = await reader.read();
    if (result.done) {
      reader.releaseLock();
      return sum;
    }
    sum += result.value;
  }
}

function benchmarkFunction(name) {
  if (name === 'sync-result-control') return consumeSyncResultControl;
  if (name === 'fulfilled-promise-control') {
    return count => consumeAsyncControl('fulfilled-promise', count);
  }
  if (name === 'async-next-control') {
    return count => consumeAsyncControl('async-next', count);
  }
  if (name === 'readable-stream') return consumeReadableStream;
  if (name === 'readable-stream-control') return consumeReadableStreamControl;
  return count => consume(name, count);
}

async function benchmark(name) {
  const fn = benchmarkFunction(name);
  const warmupIterations = Math.min(iterations, 2_000);
  for (let i = 0; i < 25; i++) {
    const sum = await fn(warmupIterations);
    if (sum !== warmupIterations) throw new Error(`${name} warmup mismatch`);
  }

  const samples = [];
  for (let round = 0; round < rounds; round++) {
    const start = now();
    const sum = await fn(iterations);
    samples.push(now() - start);
    if (sum !== iterations) throw new Error(`${name} result mismatch`);
  }

  const medianMs = median(samples);
  console.log(
    `${name}: median=${medianMs.toFixed(3)}ms ` +
    `ns/iteration=${((medianMs * 1e6) / iterations).toFixed(2)} ` +
    `samples=${samples.map(sample => sample.toFixed(3)).join(',')}`,
  );
}

for (const kind of benchmarkCases) {
  if (selectedCase === 'all' || selectedCase === kind) await benchmark(kind);
}
