const assert = require('node:assert');
const net = require('node:net');

const watchdog = setTimeout(() => {
  console.error('timed out');
  process.exit(1);
}, 8000);

const LARGE = 8 * 1024 * 1024;
const encoder = new TextEncoder();
let cancelled = null;
let aborted = false;
const onCancelled = new Promise(resolve => (cancelled = resolve));
let seen = 0;

const server = Ant.serve({
  hostname: '127.0.0.1',
  port: 0,
  fetch(request) {
    const { pathname } = new URL(request.url);
    if (pathname === '/large') return new Response('x'.repeat(LARGE));
    if (pathname === '/seq') return new Response(String(++seen));

    request.signal.addEventListener('abort', () => (aborted = true));
    return new Response(
      new ReadableStream({
        start(controller) {
          controller.enqueue(encoder.encode('data: one\n\n'));
        },
        cancel(reason) {
          cancelled(reason);
        },
      }),
      { headers: { 'content-type': 'text/event-stream' } },
    );
  },
});

function connect(options = {}) {
  return net.connect({ host: '127.0.0.1', port: server.port, ...options });
}

function request(path, extra = '') {
  return `GET ${path} HTTP/1.1\r\nHost: x\r\n${extra}\r\n`;
}

async function clientDisconnectCancelsNeverEndingBody() {
  const socket = connect();
  socket.write(request('/stream'));
  await new Promise(resolve => socket.once('data', resolve));
  socket.destroy();

  await onCancelled;
  assert.strictEqual(aborted, true);
}

async function halfCloseStillFlushesFinishedResponse() {
  const socket = connect({ allowHalfOpen: true });
  let bytes = 0;
  const closed = new Promise(resolve => socket.on('close', resolve));
  socket.on('data', chunk => (bytes += chunk.length));
  socket.write(request('/large'));
  socket.end();
  await closed;
  assert.ok(bytes >= LARGE, `received ${bytes} of ${LARGE}`);
}

async function pipelinedRequestsAnswerInOrder() {
  const socket = connect();
  let text = '';
  socket.on('data', chunk => (text += chunk));
  socket.write(request('/seq') + request('/seq') + request('/seq', 'Connection: close\r\n'));
  await new Promise(resolve => socket.on('close', resolve));
  const bodies = text.split('HTTP/1.1 200 OK').slice(1).map(part => part.split('\r\n\r\n')[1].trim());
  assert.deepStrictEqual(bodies, ['1', '2', '3']);
}

async function main() {
  await clientDisconnectCancelsNeverEndingBody();
  await halfCloseStillFlushesFinishedResponse();
  await pipelinedRequestsAnswerInOrder();
  await server.stop();
  clearTimeout(watchdog);
  console.log('server-client-disconnect:ok');
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
