const assert = require('node:assert');

const N = 3000;

async function pendingReadsResolveInOrder() {
  let controller;
  const stream = new ReadableStream({ start(c) { controller = c; } }, { highWaterMark: 0 });
  const reader = stream.getReader();
  const reads = [];
  for (let i = 0; i < N; i++) reads.push(reader.read());
  for (let i = 0; i < N / 2; i++) controller.enqueue(i);
  controller.close();

  const results = await Promise.all(reads);
  for (let i = 0; i < N / 2; i++) assert.deepStrictEqual(results[i], { value: i, done: false });
  for (let i = N / 2; i < N; i++) assert.deepStrictEqual(results[i], { value: undefined, done: true });
}

async function pendingReadsRejectAfterPartialDrain() {
  let controller;
  const stream = new ReadableStream({ start(c) { controller = c; } }, { highWaterMark: 0 });
  const reader = stream.getReader();
  const reads = [];
  for (let i = 0; i < N; i++) reads.push(reader.read().then((r) => r.value, (e) => e));
  for (let i = 0; i < 1000; i++) controller.enqueue(i);
  const error = new Error('boom');
  controller.error(error);

  const results = await Promise.all(reads);
  for (let i = 0; i < 1000; i++) assert.strictEqual(results[i], i);
  for (let i = 1000; i < N; i++) assert.strictEqual(results[i], error);
}

async function releaseLockRejectsOnlyPendingReads() {
  let controller;
  const stream = new ReadableStream({ start(c) { controller = c; } }, { highWaterMark: 0 });
  const reader = stream.getReader();
  const reads = [];
  for (let i = 0; i < 500; i++) reads.push(reader.read().then((r) => r.value, () => 'released'));
  for (let i = 0; i < 200; i++) controller.enqueue(i);
  reader.releaseLock();

  const results = await Promise.all(reads);
  for (let i = 0; i < 200; i++) assert.strictEqual(results[i], i);
  for (let i = 200; i < 500; i++) assert.strictEqual(results[i], 'released');

  const next = stream.getReader();
  const pending = next.read();
  controller.enqueue('after');
  assert.deepStrictEqual(await pending, { value: 'after', done: false });
}

async function unawaitedWritesKeepOrder() {
  const seen = [];
  const stream = new WritableStream({ write(chunk) { seen.push(chunk); } }, { highWaterMark: Infinity });
  const writer = stream.getWriter();
  const writes = [];
  for (let i = 0; i < N; i++) writes.push(writer.write(i));
  await Promise.all(writes);
  await writer.close();
  assert.strictEqual(seen.length, N);
  for (let i = 0; i < N; i++) assert.strictEqual(seen[i], i);
}

async function writesRejectAfterPartialDrain() {
  let written = 0;
  const error = new Error('sink failed');
  const stream = new WritableStream({
    write() {
      if (++written === 1000) throw error;
    },
  }, { highWaterMark: Infinity });
  const writer = stream.getWriter();
  const writes = [];
  for (let i = 0; i < N; i++) writes.push(writer.write(i).then(() => 'ok', (e) => e));

  const results = await Promise.all(writes);
  for (let i = 0; i < 999; i++) assert.strictEqual(results[i], 'ok');
  for (let i = 999; i < N; i++) assert.strictEqual(results[i], error);
}

async function queuesSurviveGcDuringCreation() {
  const COUNT = 30000;
  const readers = [];
  for (let i = 0; i < COUNT; i++) {
    let controller;
    const stream = new ReadableStream({ start(c) { controller = c; } }, { highWaterMark: Infinity });
    readers.push({ controller, reader: stream.getReader() });
  }
  for (let round = 0; round < 3; round++) {
    for (const { controller, reader } of readers) {
      controller.enqueue(round);
      assert.strictEqual((await reader.read()).value, round);
    }
  }
  for (const { controller, reader } of readers) {
    const pending = reader.read();
    controller.enqueue('pending');
    assert.strictEqual((await pending).value, 'pending');
  }

  const writers = [];
  const sink = { write() {} };
  for (let i = 0; i < COUNT; i++) writers.push(new WritableStream(sink).getWriter());
  for (const writer of writers) await writer.write(1);

  const transforms = [];
  for (let i = 0; i < COUNT / 3; i++) {
    const transform = new TransformStream();
    transforms.push({ writer: transform.writable.getWriter(), reader: transform.readable.getReader() });
  }
  for (const { writer, reader } of transforms) {
    writer.write(7);
    assert.strictEqual((await reader.read()).value, 7);
  }
}

async function main() {
  await pendingReadsResolveInOrder();
  await pendingReadsRejectAfterPartialDrain();
  await releaseLockRejectsOnlyPendingReads();
  await unawaitedWritesKeepOrder();
  await writesRejectAfterPartialDrain();
  await queuesSurviveGcDuringCreation();
  console.log('stream-queue-depth:ok');
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
