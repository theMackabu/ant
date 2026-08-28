import assert from 'node:assert';
import { spawnSync } from 'node:child_process';
import { createServer } from 'node:http';
import { fileURLToPath } from 'node:url';

const childMarker = 'ANT_FETCH_STREAM_ABORT_CHILD';

async function runChild() {
  let returnCalled = false;
  const rejectingIterator = {
    next() {
      return Promise.reject(new Error('next failed'));
    },
    return() {
      returnCalled = true;
      return Promise.resolve({ done: true });
    },
    [Symbol.asyncIterator]() {
      return this;
    },
  };

  let nextError;
  try {
    for await (const _value of rejectingIterator) {}
  } catch (error) {
    nextError = error;
  }
  assert.strictEqual(nextError?.message, 'next failed');
  assert.strictEqual(returnCalled, false);

  let normalReturnCalled = false;
  const completedIterator = {
    next() {
      return Promise.resolve({ done: true });
    },
    return() {
      normalReturnCalled = true;
      return Promise.resolve({ done: true });
    },
    [Symbol.asyncIterator]() {
      return this;
    },
  };
  for await (const _value of completedIterator) {}
  assert.strictEqual(normalReturnCalled, false);

  const invalidReturnIterator = {
    next() {
      return Promise.resolve({ done: false, value: 1 });
    },
    return: 1,
    [Symbol.asyncIterator]() {
      return this;
    },
  };
  let invalidReturnError;
  try {
    for await (const _value of invalidReturnIterator) break;
  } catch (error) {
    invalidReturnError = error;
  }
  assert(invalidReturnError instanceof TypeError);

  const server = createServer((_request, response) => {
    response.writeHead(200, {
      'content-type': 'text/event-stream',
      connection: 'keep-alive',
    });
    response.write('data: started\n\n');
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));

  const address = server.address();
  if (!address || typeof address === 'string') throw new Error('missing server address');

  const controller = new AbortController();

  try {
    const response = await fetch(`http://127.0.0.1:${address.port}`, {
      signal: controller.signal,
    });
    setTimeout(() => controller.abort(), 10);
    for await (const _chunk of response.body) {}
    controller.signal.throwIfAborted();
  } catch (error) {
    if (!(error instanceof Error) || error.name !== 'AbortError') throw error;
  }

  await new Promise(resolve => server.close(resolve));
  console.log('caught abort without an unhandled rejection');
}

if (process.env[childMarker] === '1') {
  await runChild();
} else {
  const result = spawnSync(process.execPath, [fileURLToPath(import.meta.url)], {
    encoding: 'utf8',
    env: { ...process.env, [childMarker]: '1' },
    timeout: 10_000,
  });

  try {
    if (result.error) throw result.error;
    assert.strictEqual(result.status, 0, result.stderr || result.stdout);
    assert.match(result.stdout, /caught abort without an unhandled rejection/);
    assert.doesNotMatch(result.stderr, /Uncaught \(in promise\)/);
  } catch (error) {
    console.error(error);
    process.exit(1);
  }

  console.log('streaming fetch abort is caught by for-await');
}
