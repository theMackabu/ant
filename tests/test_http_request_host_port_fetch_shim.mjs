import { request } from 'node:http';

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const originalFetch = globalThis.fetch;
const seenUrls = [];

try {
  globalThis.fetch = async url => {
    seenUrls.push(String(url));
    return new Response('ok', { status: 200 });
  };

  await new Promise((resolve, reject) => {
    const req = request({
      host: 'example.com:8080',
      path: '/resource',
      timeout: 500
    }, response => {
      response.resume();
      response.on('end', resolve);
    });

    req.on('error', reject);
    req.on('timeout', () => reject(new Error('request timed out unexpectedly')));
    req.end();
  });

  assert(seenUrls.length === 1, `expected one fetch call, got ${seenUrls.length}`);
  assert(seenUrls[0] === 'http://example.com:8080/resource',
    `unexpected request URL: ${seenUrls[0]}`);

  console.log('http.request host port fetch shim test passed');
} finally {
  globalThis.fetch = originalFetch;
}
