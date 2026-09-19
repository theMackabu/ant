import assert from 'node:assert';

async function main() {
  const promise = Promise.resolve({ answer: 42 });
  const order = [];
  async function read() {
    order.push('start');
    const value = await promise;
    order.push('resume');
    return value;
  }
  queueMicrotask(() => order.push('earlier'));
  const result = read();
  order.push('caller');
  queueMicrotask(() => order.push('later'));
  assert.strictEqual((await result).answer, 42);
  assert.deepStrictEqual(order, ['start', 'caller', 'earlier', 'resume', 'later']);

  // Existing queued reactions keep their position relative to the await.
  const queued = [];
  promise.then(() => queued.push('reaction'));
  const afterReaction = (async () => {
    await promise;
    queued.push('await');
  })();
  queueMicrotask(() => queued.push('microtask'));
  await afterReaction;
  assert.deepStrictEqual(queued, ['reaction', 'await', 'microtask']);

  const interleaved = [];
  const firstWait = (async () => { await promise; interleaved.push('first'); })();
  queueMicrotask(() => interleaved.push('between'));
  const secondWait = (async () => { await promise; interleaved.push('second'); })();
  await firstWait;
  await secondWait;
  assert.deepStrictEqual(interleaved, ['first', 'between', 'second']);

  // Reuse the same fulfilled promise, including while its reaction is running.
  let sum = 0;
  await promise.then(async () => {
    for (let i = 0; i < 256; i++) sum += (await promise).answer;
  });
  assert.strictEqual(sum, 256 * 42);

  const failure = { reason: 7 };
  try { await Promise.reject(failure); assert.fail('expected rejection'); }
  catch (error) { assert.strictEqual(error, failure); }
  assert.strictEqual(await Promise.resolve(5), 5);

  const generator = (async function* () {
    yield await promise;
    return (await promise).answer;
  })();
  const first = generator.next();
  const second = generator.next();
  assert.deepStrictEqual(await first, { value: { answer: 42 }, done: false });
  assert.deepStrictEqual(await second, { value: 42, done: true });
  console.log('fulfilled await jobs passed');
}

main().catch(error => {
  console.error(error);
  process.exit(1);
});
