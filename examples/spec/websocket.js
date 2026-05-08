import { test, summary } from './helpers.js';
import { spawn } from 'child_process';

console.log('WebSocket Tests\n');

const port = 32187;
const serverPath = new URL('./fixtures/websocket_server.mjs', import.meta.url).pathname;

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

function startServer(path) {
  const child = spawn(process.execPath, [path, String(port)]);
  child.on('stderr', data => {
    if (String(data).trim()) console.log(String(data).trim());
  });
  return child;
}

test('WebSocket global exists', typeof WebSocket, 'function');
test('WebSocket CONNECTING constant', WebSocket.CONNECTING, 0);
test('WebSocket OPEN constant', WebSocket.OPEN, 1);
test('MessageEvent global exists', typeof MessageEvent, 'function');
test('CloseEvent global exists', typeof CloseEvent, 'function');

const server = startServer(serverPath);
await sleep(150);

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
    if (event.data === 'echo:hello') socket.send('close');
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
test('WebSocket round trip', result.seen.includes('echo:hello'), true);
test('WebSocket close code', result.code, 1000);
test('WebSocket closed state', result.readyState, WebSocket.CLOSED);

summary();
