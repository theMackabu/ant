import assert from 'node:assert';
import { Sandbox } from 'ant:sandbox';

async function burst(count) {
  const sandbox = new Sandbox({ memory: '128mb' });
  const running = sandbox.run('tests/sandbox/fixtures/message_burst_guest.js');
  const seen = [];
  try {
    for await (const message of sandbox.messages) {
      if (message.type === 'ready') sandbox.send({ count });
      if (message.type === 'item') seen.push(message.index);
      if (message.type === 'done') {
        assert.strictEqual(message.count, count);
        break;
      }
    }
    assert.deepStrictEqual(seen, Array.from({ length: count }, (_, index) => index));
  } finally {
    await sandbox.terminate();
    await running.catch(() => {});
  }
}

async function queuedWhileNotWaiting() {
  const sandbox = new Sandbox({ memory: '128mb' });
  const running = sandbox.run('tests/sandbox/fixtures/message_burst_guest.js');
  try {
    assert.deepStrictEqual(await sandbox.once('ready'), { type: 'ready' });
    sandbox.send({ count: 3 });
    await new Promise(resolve => setTimeout(resolve, 300));

    assert.deepStrictEqual(await sandbox.once('done'), { type: 'done', count: 3 });
    assert.deepStrictEqual(await sandbox.receive(), { type: 'item', index: 0 });
    assert.deepStrictEqual(await sandbox.receive(), { type: 'item', index: 1 });
    assert.deepStrictEqual(await sandbox.receive(), { type: 'item', index: 2 });
  } finally {
    await sandbox.terminate();
    await running.catch(() => {});
  }
}

async function drainedAfterExit() {
  const sandbox = new Sandbox({ memory: '128mb' });
  const running = sandbox.run('tests/sandbox/fixtures/message_burst_guest.js');
  assert.deepStrictEqual(await sandbox.once('ready'), { type: 'ready' });
  sandbox.send({ count: 2, exit: true });
  await running.catch(() => {});

  const types = [];
  for await (const message of sandbox.messages) types.push(message.type);
  assert.deepStrictEqual(types, ['item', 'item', 'done']);
  await sandbox.terminate();
}

for (let round = 0; round < 8; round++) {
  await burst(1);
  await burst(40);
}
await queuedWhileNotWaiting();
await drainedAfterExit();

console.log('sandbox message burst: ok');
