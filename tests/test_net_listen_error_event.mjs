import assert from 'node:assert';
import net from 'node:net';

const once = (emitter, event) => new Promise((resolve) => emitter.once(event, resolve));

const holder = net.createServer();
holder.listen(0, '127.0.0.1');
await once(holder, 'listening');
const { port } = holder.address();

const server = net.createServer();
assert.strictEqual(server.listen(port, '127.0.0.1'), server);

const error = await once(server, 'error');
assert.ok(error instanceof Error);
assert.strictEqual(error.code, 'EADDRINUSE');
assert.strictEqual(error.syscall, 'listen');
assert.strictEqual(error.address, '127.0.0.1');
assert.strictEqual(error.port, port);
assert.strictEqual(typeof error.errno, 'number');
assert.strictEqual(error.message, `listen EADDRINUSE: address already in use 127.0.0.1:${port}`);
assert.strictEqual(server.listening, false);

server.listen(0, '127.0.0.1');
await once(server, 'listening');
assert.notStrictEqual(server.address().port, port);

for (let attempt = 0; attempt < 3; attempt++) {
  const retry = net.createServer();
  retry.listen(port, '127.0.0.1');
  assert.strictEqual((await once(retry, 'error')).code, 'EADDRINUSE');
}

server.close();
holder.close();
console.log('ok');
