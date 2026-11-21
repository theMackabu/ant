import assert from 'node:assert';
import net from 'node:net';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const once = (emitter, event) => new Promise((resolve) => emitter.once(event, resolve));

if (process.argv[2] === 'echo') {
  const socket = new net.Socket({ fd: 3 });
  socket.on('data', (chunk) => socket.end(String(chunk).toUpperCase()));
} else if (process.argv[2] === 'idle') {
  setTimeout(() => {}, 50);
} else {
  const self = fileURLToPath(import.meta.url);

  const child = spawn(process.execPath, [self, 'echo'], { stdio: ['ignore', 'inherit', 'inherit', 'pipe', 'ignore'] });
  assert.strictEqual(child.stdio.length, 5);
  assert.ok(child.stdio[3] instanceof net.Socket);
  assert.strictEqual(child.stdio[4], null);

  let reply = '';
  child.stdio[3].setEncoding('utf8');
  child.stdio[3].on('data', (chunk) => (reply += chunk));
  child.stdio[3].write('ping');
  await Promise.all([once(child.stdio[3], 'end'), once(child, 'exit')]);
  assert.strictEqual(reply, 'PING');

  const idle = spawn(process.execPath, [self, 'idle'], { stdio: ['ignore', 'inherit', 'inherit', 'pipe'] });
  assert.deepStrictEqual(await once(idle, 'exit'), 0);

  const missing = spawn('ant-test-missing-binary', [], { stdio: ['ignore', 'ignore', 'ignore', 'pipe'] });
  assert.strictEqual((await once(missing, 'error')).code, 'ENOENT');

  console.log('ok');
}
