import { get } from 'node:https';

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const originalFetch = globalThis.fetch;
let seenUrl = null;

try {
  globalThis.fetch = async (url, init) => {
    seenUrl = String(url);
    assert(seenUrl === 'https://registry.npmjs.org/serve', `unexpected request URL: ${seenUrl}`);
    assert(init.method === 'GET', `expected GET method, got ${init.method}`);
    assert(init.headers.accept.includes('application/json'),
      `unexpected accept header: ${init.headers.accept}`);

    return new Response('{"ok":true}', {
      status: 200,
      headers: {
        'content-type': 'application/json'
      }
    });
  };

  const body = await new Promise((resolve, reject) => {
    const req = get({
      host: 'registry.npmjs.org',
      path: '/serve',
      timeout: 500,
      headers: {
        accept: 'application/json'
      }
    }, response => {
      assert(response.statusCode === 200, `expected 200 status, got ${response.statusCode}`);
      assert(response.headers['content-type'] === 'application/json',
        `unexpected content-type: ${response.headers['content-type']}`);

      response.setEncoding('utf8');

      let rawData = '';
      response.on('data', chunk => {
        rawData += chunk;
      });
      response.on('end', () => {
        resolve(rawData);
      });
    });

    req.on('error', reject);
    req.on('timeout', () => reject(new Error('request timed out unexpectedly')));
  });

  assert(body === '{"ok":true}', `unexpected body: ${body}`);

  for (const invalidInput of [
    'http://registry.npmjs.org/serve',
    { protocol: 'http:', host: 'registry.npmjs.org', path: '/serve' }
  ]) {
    let invalidError = null;
    try {
      get(invalidInput);
    } catch (error) {
      invalidError = error;
    }

    assert(invalidError, `expected invalid protocol error for ${String(invalidInput)}`);
    assert(invalidError.name === 'TypeError',
      `expected TypeError for invalid protocol, got ${invalidError && invalidError.name}`);
    assert(invalidError.code === 'ERR_INVALID_PROTOCOL',
      `expected ERR_INVALID_PROTOCOL, got ${invalidError && invalidError.code}`);
  }

  console.log('https.get fetch shim test passed');
} finally {
  globalThis.fetch = originalFetch;
}
