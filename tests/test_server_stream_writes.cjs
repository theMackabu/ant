const assert = require('node:assert');
const net = require('node:net');

const watchdog = setTimeout(() => {
  console.error('timed out');
  process.exit(1);
}, 15000);

const encoder = new TextEncoder();
const bytes = text => encoder.encode(text);
const LARGE_CHUNKS = 40;
const LARGE_CHUNK = 'y'.repeat(16 * 1024);
let releaseSilent = null;

function stream(start) {
  return new Response(new ReadableStream({ start }));
}

const routes = {
  '/silent': () =>
    new Response(
      new ReadableStream({
        start(c) {
          releaseSilent = () => {
            c.enqueue(bytes('late'));
            c.close();
          };
        },
      }),
    ),
  '/many': () =>
    stream(c => {
      for (const part of ['a', 'b', 'c', 'd']) c.enqueue(bytes(part));
      c.close();
    }),
  '/empty': () => stream(c => c.close()),
  '/large': () =>
    stream(c => {
      for (let i = 0; i < LARGE_CHUNKS; i++) c.enqueue(bytes(LARGE_CHUNK));
      c.close();
    }),
  '/mixed': () =>
    stream(async c => {
      c.enqueue(bytes('first;'));
      await new Promise(resolve => setTimeout(resolve, 30));
      c.enqueue(bytes('second;'));
      c.enqueue(bytes('third'));
      c.close();
    }),
  '/error': () =>
    stream(async c => {
      c.enqueue(bytes('before'));
      await new Promise(resolve => setTimeout(resolve, 10));
      c.error(new Error('boom'));
    }),
  '/error-now': () =>
    stream(c => {
      c.enqueue(bytes('queued'));
      c.error(new Error('boom'));
    }),
  '/pull': () => {
    let n = 0;
    return new Response(
      new ReadableStream({
        async pull(c) {
          await null;
          if (n++ < 3) c.enqueue(bytes(String(n)));
          else c.close();
        },
      }),
    );
  },
  '/plain': () => new Response('plain'),
  '/empty-mid': () =>
    stream(c => {
      c.enqueue(bytes('a'));
      c.enqueue(new Uint8Array(0));
      c.enqueue(bytes('b'));
      c.close();
    }),
  '/empty-first': () =>
    stream(c => {
      c.enqueue('');
      c.enqueue(bytes('x'));
      c.close();
    }),
  '/empty-only': () =>
    stream(c => {
      c.enqueue(new Uint8Array(0));
      c.close();
    }),
  '/empty-late': () =>
    stream(async c => {
      await new Promise(resolve => setTimeout(resolve, 5));
      c.enqueue(new Uint8Array(0));
      await new Promise(resolve => setTimeout(resolve, 5));
      c.enqueue(bytes('late'));
      c.close();
    }),
};

const server = Ant.serve({
  hostname: '127.0.0.1',
  port: 0,
  idleTimeout: 1,
  fetch(request) {
    return routes[new URL(request.url).pathname]();
  },
});

function decodeChunked(text) {
  const out = [];
  let rest = text;
  for (;;) {
    const line = rest.indexOf('\r\n');
    if (line < 0) return { body: out.join(''), complete: false };
    const size = parseInt(rest.slice(0, line), 16);
    if (size === 0) return { body: out.join(''), complete: true };
    out.push(rest.slice(line + 2, line + 2 + size));
    rest = rest.slice(line + 2 + size + 2);
  }
}

function get(path, method = 'GET') {
  return new Promise((resolve, reject) => {
    const socket = net.connect({ host: '127.0.0.1', port: server.port });
    let text = '';
    socket.on('data', chunk => (text += chunk));
    socket.on('error', reject);
    socket.on('close', () => {
      const split = text.indexOf('\r\n\r\n');
      resolve({ head: text.slice(0, split), ...decodeChunked(text.slice(split + 4)), raw: text });
    });
    socket.write(`${method} ${path} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n`);
  });
}

async function silentStreamSendsHeadersBeforeAnyChunk() {
  const socket = net.connect({ host: '127.0.0.1', port: server.port });
  let text = '';
  socket.on('data', chunk => (text += chunk));
  socket.write('GET /silent HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n');
  await new Promise(resolve => socket.once('data', resolve));
  assert.ok(text.startsWith('HTTP/1.1 200'), 'head arrives while the stream is silent');
  assert.ok(!text.includes('late'));
  const closed = new Promise(resolve => socket.on('close', resolve));
  releaseSilent();
  await closed;
  assert.strictEqual(decodeChunked(text.slice(text.indexOf('\r\n\r\n') + 4)).body, 'late');
}

async function bodiesArriveIntactAndInOrder() {
  let result = await get('/many');
  assert.strictEqual(result.body, 'abcd');
  assert.strictEqual(result.complete, true);

  result = await get('/empty');
  assert.strictEqual(result.body, '');
  assert.strictEqual(result.complete, true);

  result = await get('/mixed');
  assert.strictEqual(result.body, 'first;second;third');
  assert.strictEqual(result.complete, true);

  result = await get('/pull');
  assert.strictEqual(result.body, '123');
  assert.strictEqual(result.complete, true);

  result = await get('/large');
  assert.strictEqual(result.body.length, LARGE_CHUNKS * LARGE_CHUNK.length);
  assert.strictEqual(result.complete, true);
}

async function emptyChunksDoNotEndTheBody() {
  for (const [path, body] of [['/empty-mid', 'ab'], ['/empty-first', 'x'], ['/empty-only', ''], ['/empty-late', 'late']]) {
    const result = await get(path);
    assert.strictEqual(result.body, body, path);
    assert.strictEqual(result.complete, true, path);
    assert.ok(result.raw.endsWith('0\r\n\r\n'), path);
    assert.strictEqual(result.raw.split('0\r\n\r\n').length, 2, path);
  }
}

async function erroredStreamsDeliverWhatWasQueued() {
  const late = await get('/error');
  assert.ok(late.head.startsWith('HTTP/1.1 200'));
  assert.strictEqual(late.body, 'before');

  const now = await get('/error-now');
  assert.ok(now.head.startsWith('HTTP/1.1 200'));
}

async function headRequestHasNoBody() {
  const result = await get('/many', 'HEAD');
  assert.ok(result.raw.startsWith('HTTP/1.1 200'));
  assert.ok(!result.raw.includes('abcd'));
}

async function keepAliveConnectionServesStreamThenPlain() {
  const socket = net.connect({ host: '127.0.0.1', port: server.port });
  let text = '';
  socket.on('data', chunk => (text += chunk));
  const request = path => `GET ${path} HTTP/1.1\r\nHost: x\r\n\r\n`;
  socket.write(request('/many'));
  for (let i = 0; i < 100 && !text.includes('0\r\n\r\n'); i++) await new Promise(resolve => setTimeout(resolve, 10));
  socket.write(request('/plain'));
  for (let i = 0; i < 100 && !text.endsWith('plain'); i++) await new Promise(resolve => setTimeout(resolve, 10));
  assert.ok(text.endsWith('plain'), JSON.stringify(text.slice(-80)));
  socket.destroy();
}

async function idleTimeoutRestartsOnActivityThenFires() {
  const socket = net.connect({ host: '127.0.0.1', port: server.port });
  const request = 'GET /plain HTTP/1.1\r\nHost: x\r\n\r\n';
  let responses = 0;
  socket.on('data', () => responses++);
  const closed = new Promise(resolve => socket.on('close', resolve));

  socket.write(request);
  await new Promise(resolve => setTimeout(resolve, 700));
  socket.write(request);
  await new Promise(resolve => setTimeout(resolve, 700));
  assert.strictEqual(socket.destroyed, false, 'activity pushed the idle deadline out');
  socket.write(request);

  const start = Date.now();
  await closed;
  const elapsed = Date.now() - start;
  assert.strictEqual(responses, 3);
  assert.ok(elapsed >= 800 && elapsed < 2500, `idle close after ${elapsed} ms`);
}

async function main() {
  await silentStreamSendsHeadersBeforeAnyChunk();
  await bodiesArriveIntactAndInOrder();
  await emptyChunksDoNotEndTheBody();
  await erroredStreamsDeliverWhatWasQueued();
  await headRequestHasNoBody();
  await keepAliveConnectionServesStreamThenPlain();
  await idleTimeoutRestartsOnActivityThenFires();
  await server.stop();
  clearTimeout(watchdog);
  console.log('server-stream-writes:ok');
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
