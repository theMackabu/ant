import { test, summary } from './helpers.js';
import { startServer } from './fixtures/server_ready.mjs';

console.log('WebSocket Tests\n');

const port = 32187;
const serverPath = new URL('./fixtures/websocket_server.mjs', import.meta.url).pathname;

test('WebSocket global exists', typeof WebSocket, 'function');
test('WebSocket CONNECTING constant', WebSocket.CONNECTING, 0);
test('WebSocket OPEN constant', WebSocket.OPEN, 1);
test('MessageEvent global exists', typeof MessageEvent, 'function');
test('CloseEvent global exists', typeof CloseEvent, 'function');

const server = await startServer(serverPath, port);

const result = await new Promise(resolve => {
  const seen = [];
  const socket = new WebSocket(`ws://127.0.0.1:${port}/ws`);
  socket.binaryType = 'arraybuffer';

  socket.onopen = () => {
    seen.push('open');
    socket.send('hello');
  };

  socket.addEventListener('message', event => {
    seen.push(event.data);
    if (event.data === 'string:hello') socket.send('close');
  });

  socket.onclose = event => {
    resolve({
      seen,
      code: event.code,
      reason: event.reason,
      readyState: socket.readyState
    });
  };

  socket.onerror = () => {
    resolve({ seen, code: -1, reason: 'error', readyState: socket.readyState });
  };
});

server.kill('SIGTERM');

test('WebSocket opened', result.seen.includes('open'), true);
test('WebSocket text send preserves opcode', result.seen.includes('string:hello'), true);
test('WebSocket close code', result.code, 1000);
test('WebSocket closed state', result.readyState, WebSocket.CLOSED);

summary();
