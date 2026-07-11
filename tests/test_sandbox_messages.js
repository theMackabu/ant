import assert from 'node:assert';
import { Sandbox } from 'ant:sandbox';

const sandbox = new Sandbox({ memory: '128mb' });
const running = sandbox.run('examples/demo/ipc/guest.js');

try {
  assert.deepStrictEqual(await sandbox.once('ready'), { type: 'ready' });

  const pong = sandbox.receive();
  sandbox.send({ type: 'ping' });
  assert.deepStrictEqual(await pong, { type: 'pong', requests: 1 });

  const result = (async () => {
    for await (const message of sandbox.messages) {
      if (message.type === 'result') return message;
    }
  })();
  sandbox.send({ type: 'add', left: 20, right: 22 });
  assert.deepStrictEqual(await result, { type: 'result', value: 42, requests: 2 });

  await sandbox.close();
  assert.strictEqual(await running, 0);

  const closed = await sandbox.messages.next();
  assert.strictEqual(closed.done, true);
} finally {
  await sandbox.terminate();
  await running.catch(() => {});
}

console.log('sandbox messages: ok');
