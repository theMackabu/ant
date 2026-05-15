import { test, summary } from './helpers.js';
import { startServer } from './fixtures/server_ready.mjs';

console.log('EventSource Tests\n');

const port = 32188;
const serverPath = new URL('./fixtures/eventsource_server.mjs', import.meta.url).pathname;

test('EventSource global exists', typeof EventSource, 'function');
test('EventSource CONNECTING constant', EventSource.CONNECTING, 0);
test('EventSource OPEN constant', EventSource.OPEN, 1);
test('EventSource CLOSED constant', EventSource.CLOSED, 2);

const server = await startServer(serverPath, port);

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

test('EventSource opened', result.opened, true);
test('EventSource event data', result.data, 'hello\nworld');
test('EventSource last event id', result.lastEventId, '42');
test('EventSource withCredentials', result.withCredentials, true);
test('EventSource ready before close', result.readyStateBeforeClose, EventSource.OPEN);
test('EventSource closed state', result.readyStateAfterClose, EventSource.CLOSED);

const retryResult = await new Promise(resolve => {
  const source = new EventSource(`http://127.0.0.1:${port}/retry`);
  const seen = [];
  const timeout = setTimeout(() => {
    source.close();
    resolve(seen.join(','));
  }, 1000);

  source.addEventListener('tick', event => {
    seen.push(event.data);
    if (event.data === 'second') {
      clearTimeout(timeout);
      source.close();
      resolve(seen.join(','));
    }
  });
});

test('EventSource retry controls reconnect delay', retryResult, 'first,second');

server.kill('SIGTERM');

summary();
