import assert from 'node:assert';
import net from 'node:net';

const tick = () => new Promise((resolve) => setTimeout(resolve, 20));
const once = (emitter, event) => new Promise((resolve) => emitter.once(event, resolve));

async function withServer(onConnection, fn) {
  const server = net.createServer(onConnection);
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  try { await fn(server.address().port); } finally { server.close(); }
}

await withServer((s) => { s.write('abcdef'); setTimeout(() => s.end('ghij'), 30); }, async (port) => {
  const sock = net.connect(port, '127.0.0.1');
  const events = [];
  sock.on('end', () => events.push('end'));
  sock.on('close', () => events.push('close'));
  await once(sock, 'readable');
  assert.strictEqual(String(sock.read(2)), 'ab');
  assert.strictEqual(sock.read(100), null);
  assert.strictEqual(String(sock.read()), 'cdef');
  sock.unshift(Buffer.from('XY'));
  assert.strictEqual(String(sock.read(1)), 'X');
  await tick(); await tick();
  assert.strictEqual(String(sock.read(100)), 'Yghij');
  assert.strictEqual(sock.read(), null);
  await tick(); await tick();
  assert.deepStrictEqual(events, ['end', 'close']);
});

await withServer((s) => s.end('héllo wörld'), async (port) => {
  const sock = net.connect(port, '127.0.0.1');
  sock.setEncoding('utf8');
  const parts = [];
  sock.on('data', (d) => parts.push(d));
  await once(sock, 'end');
  assert.deepStrictEqual(parts, ['héllo wörld']);
});

await withServer((s) => { s.write('one'); setTimeout(() => s.end('two'), 30); }, async (port) => {
  const sock = net.connect(port, '127.0.0.1');
  const parts = [];
  sock.on('data', (d) => parts.push(String(d)));
  sock.pause();
  await tick(); await tick(); await tick();
  assert.strictEqual(parts.length, 0);
  assert.ok(sock.readableLength > 0);
  sock.resume();
  await once(sock, 'end');
  assert.strictEqual(parts.join(''), 'onetwo');
});

const big = Buffer.alloc(4 * 1024 * 1024, 7);
await withServer((s) => { s.on('error', () => {}); s.end(big); }, async (port) => {
  const sock = net.connect(port, '127.0.0.1');
  await tick(); await tick(); await tick(); await tick();
  assert.ok(sock.readableLength <= 1024 * 1024, `paused socket buffered ${sock.readableLength} bytes`);
  let total = 0;
  for await (const chunk of sock) total += chunk.length;
  assert.strictEqual(total, big.length);
});

await withServer((s) => { s.on('end', () => s.end('bye')); }, async (port) => {
  const sock = net.connect({ port, host: '127.0.0.1', allowHalfOpen: true });
  const got = [];
  sock.on('data', (d) => got.push(String(d)));
  sock.on('connect', () => sock.end());
  await once(sock, 'close');
  assert.strictEqual(got.join(''), 'bye');
});

await withServer((s) => {
  let request = '';
  s.on('data', (d) => (request += d));
  s.on('end', () => s.end(`response to ${request}`));
}, async (port) => {
  const sock = net.connect(port, '127.0.0.1');
  const got = [];
  const events = [];
  sock.on('data', (d) => got.push(String(d)));
  sock.on('end', () => events.push('end'));
  sock.on('close', () => events.push('close'));
  sock.end('ping');
  await once(sock, 'close');
  assert.strictEqual(got.join(''), 'response to ping');
  assert.deepStrictEqual(events, ['end', 'close']);
});

await withServer((s) => s.end('abcdefghijklmnopqrstuvwxyz'), async (port) => {
  const sock = net.connect(port, '127.0.0.1');
  const got = [];
  await once(sock, 'readable');
  await tick();
  const head = sock.read(5);
  assert.strictEqual(String(head), 'abcde');
  assert.strictEqual(sock.readableLength, 21);
  sock.unshift(head);
  assert.strictEqual(sock.readableLength, 26);
  sock.on('data', (d) => got.push(String(d)));
  await once(sock, 'close');
  assert.strictEqual(got.join(''), 'abcdefghijklmnopqrstuvwxyz');
});

console.log('net socket readable: ok');
