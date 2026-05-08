import { test, summary } from './helpers.js';
import { spawn } from 'child_process';

console.log('EventSource Tests\n');

const port = 32188;
const serverPath = new URL('./fixtures/eventsource_server.mjs', import.meta.url).pathname;

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

test('EventSource global exists', typeof EventSource, 'function');
test('EventSource CONNECTING constant', EventSource.CONNECTING, 0);
test('EventSource OPEN constant', EventSource.OPEN, 1);
test('EventSource CLOSED constant', EventSource.CLOSED, 2);

const server = startServer(serverPath);
await sleep(150);

const result = await new Promise(resolve => {
  const source = new EventSource(`http://127.0.0.1:${port}/events`, { withCredentials: true });
  let opened = false;

  source.onopen = () => {
    opened = true;
  };

  source.addEventListener('greeting', event => {
    const readyStateBeforeClose = source.readyState;
    source.close();
    resolve({
      opened,
      data: event.data,
      lastEventId: event.lastEventId,
      withCredentials: source.withCredentials,
      readyStateBeforeClose,
      readyStateAfterClose: source.readyState
    });
  });

  source.onerror = () => {
    if (source.readyState === EventSource.CLOSED) return;
  };
});

server.kill('SIGTERM');

test('EventSource opened', result.opened, true);
test('EventSource event data', result.data, 'hello\nworld');
test('EventSource last event id', result.lastEventId, '42');
test('EventSource withCredentials', result.withCredentials, true);
test('EventSource ready before close', result.readyStateBeforeClose, EventSource.OPEN);
test('EventSource closed state', result.readyStateAfterClose, EventSource.CLOSED);

summary();
