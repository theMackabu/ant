import { test, summary } from './helpers.js';

console.log('Fetch Tests\n');

async function testRejects(name, fn) {
  try {
    await fn();
    test(`${name} rejects`, false, true);
  } catch (_err) {
    test(`${name} rejects`, true, true);
  }
}

test('fetch typeof', typeof fetch, 'function');

const textResponse = await fetch('data:,hello%20world');
test('fetch data url returns Response', textResponse instanceof Response, true);
test('fetch data url status', textResponse.status, 200);
test('fetch data url ok', textResponse.ok, true);
test('fetch data url url', textResponse.url, 'data:,hello%20world');
test('fetch data url text', await textResponse.text(), 'hello world');

const jsonResponse = await fetch('data:application/json,%7B%22ok%22%3Atrue%7D');
test('fetch data url json content-type', jsonResponse.headers.get('content-type'), 'application/json');
test('fetch data url json body', (await jsonResponse.json()).ok, true);

test(
  'fetch response headers immutable',
  (() => {
    try {
      jsonResponse.headers.append('x-test', '1');
      return false;
    } catch (_err) {
      return true;
    }
  })(),
  true
);

const requestInput = new Request('data:text/plain,request-body');
const requestResponse = await fetch(requestInput);
test('fetch accepts Request input', await requestResponse.text(), 'request-body');

const encoder = new TextEncoder();
const streamBody = new ReadableStream({
  start(controller) {
    controller.enqueue(encoder.encode('{"runtime":"ant",'));
    controller.enqueue(encoder.encode('"stream":true}'));
    controller.close();
  }
});
const streamedUploadResponse = await fetch('https://httpbingo.org/post', {
  method: 'POST',
  body: streamBody,
  duplex: 'half',
  headers: {
    'Content-Type': 'application/json'
  }
});
const streamedUploadJson = await streamedUploadResponse.json();
test('fetch streamed body status', streamedUploadResponse.status, 200);
test('fetch streamed body json runtime', streamedUploadJson.json.runtime, 'ant');
test('fetch streamed body json stream', streamedUploadJson.json.stream, true);

const abortedController = new AbortController();
abortedController.abort(new Error('aborted'));
await testRejects('fetch with aborted signal', () => fetch('data:,ignored', { signal: abortedController.signal }));

await testRejects('fetch unsupported scheme', () => fetch('file:///tmp/ant-fetch-unsupported'));

summary();
