import { test, testThrows, summary } from './helpers.js';

console.log('Response Tests\n');

test('Response typeof', typeof Response, 'function');
test('Response toStringTag', Object.prototype.toString.call(new Response()), '[object Response]');
testThrows('Response requires new', () => Response());

const res0 = new Response();
test('Response type default', res0.type, 'default');
test('Response url default', res0.url, '');
test('Response redirected default', res0.redirected, false);
test('Response status default', res0.status, 200);
test('Response ok default', res0.ok, true);
test('Response statusText default', res0.statusText, '');
test('Response body default', res0.body, null);
test('Response headers same object', res0.headers === res0.headers, true);
const responseHeadersGetter = Object.getOwnPropertyDescriptor(Response.prototype, 'headers').get;
testThrows('Response headers getter rejects incompatible receiver', () => responseHeadersGetter.call({}));

const res1 = new Response('hello', {
  status: 201,
  statusText: 'Created',
  headers: { 'X-Test': 'ok' }
});
test('Response init status', res1.status, 201);
test('Response init statusText', res1.statusText, 'Created');
test('Response init header', res1.headers.get('x-test'), 'ok');
test('Response init text', await res1.text(), 'hello');

const pendingOriginal = new Response('pending', { headers: { 'X-State': 'original' } });
const pendingClone = pendingOriginal.clone();
pendingOriginal.headers.set('X-State', 'mutated');
test('Response pending clone keeps independent headers', pendingClone.headers.get('x-state'), 'original');
test('Response pending clone original mutation visible', pendingOriginal.headers.get('x-state'), 'mutated');

const materializedOriginal = new Response('materialized', { headers: { 'X-State': 'before' } });
const materializedHeaders = materializedOriginal.headers;
const materializedClone = materializedOriginal.clone();
materializedHeaders.set('X-State', 'after');
test('Response materialized headers keep identity', materializedOriginal.headers === materializedHeaders, true);
test('Response materialized clone keeps independent headers', materializedClone.headers.get('x-state'), 'before');
test('Response materialized original mutation visible', materializedOriginal.headers.get('x-state'), 'after');

const initOrder = [];
const orderedInit = {
  get headers() { initOrder.push('headers'); return { 'X-Order': 'ok' }; },
  get status() { initOrder.push('status'); return 201; },
  get statusText() { initOrder.push('statusText'); return 'Created'; }
};
const orderedResponse = new Response(null, orderedInit);
test('Response init getter order', initOrder.join(','), 'headers,status,statusText');
test('Response init getter values', `${orderedResponse.status}:${orderedResponse.statusText}:${orderedResponse.headers.get('x-order')}`, '201:Created:ok');

const nestedInitOrder = [];
const nestedHeaders = {};
Object.defineProperty(nestedHeaders, 'X-Nested', {
  enumerable: true,
  get() { nestedInitOrder.push('header value'); return 'ok'; }
});
new Response(null, {
  get headers() { nestedInitOrder.push('headers'); return nestedHeaders; },
  get status() { nestedInitOrder.push('status'); return 201; },
  get statusText() { nestedInitOrder.push('statusText'); return 'Created'; }
});
test(
  'Response consumes headers before later init fields',
  nestedInitOrder.join(','),
  'headers,header value,status,statusText'
);

const mutatingHeaders = {};
Object.defineProperty(mutatingHeaders, 'X-Mutate', {
  enumerable: true,
  get() {
    Object.defineProperty(Object.prototype, 'status', {
      configurable: true,
      writable: true,
      value: 206
    });
    return 'ok';
  }
});
try {
  const mutatedInitResponse = new Response(null, { headers: mutatingHeaders });
  test('Response observes prototype mutation during HeadersInit', mutatedInitResponse.status, 206);
} finally {
  delete Object.prototype.status;
}

const throwingInitOrder = [];
try {
  new Response(null, {
    get headers() { throwingInitOrder.push('headers'); throw new Error('stop'); },
    get status() { throwingInitOrder.push('status'); return 201; }
  });
} catch {}
test('Response stops init after headers throws', throwingInitOrder.join(','), 'headers');

const inheritedInit = Object.create({
  get status() { return 202; },
  get statusText() { return 'Accepted'; }
});
inheritedInit.headers = { 'X-Inherited': 'ok' };
const inheritedResponse = new Response(null, inheritedInit);
test('Response inherited status observed', inheritedResponse.status, 202);
test('Response inherited statusText observed', inheritedResponse.statusText, 'Accepted');

new Response(null, { headers: { 'X-Warm': '1' } });
Object.defineProperty(Object.prototype, 'status', {
  configurable: true,
  writable: true,
  value: 203
});
try {
  const invalidatedResponse = new Response(null, { headers: { 'X-Epoch': 'ok' } });
  test('Response observes later Object.prototype status', invalidatedResponse.status, 203);
} finally {
  delete Object.prototype.status;
}

const proxyReads = [];
const proxyResponse = new Response(null, new Proxy({ headers: { 'X-Proxy': 'ok' } }, {
  get(target, key, receiver) {
    proxyReads.push(String(key));
    return Reflect.get(target, key, receiver);
  }
}));
test('Response Proxy init reads all fields', proxyReads.join(','), 'headers,status,statusText');
test('Response Proxy init header', proxyResponse.headers.get('x-proxy'), 'ok');

class DerivedResponse extends Response {}
const derivedResponse = new DerivedResponse('derived', { headers: { 'X-Derived': 'ok' } });
test('Response subclass prototype', derivedResponse instanceof DerivedResponse, true);
test('Response subclass header', derivedResponse.headers.get('x-derived'), 'ok');

const normalizedTypeResponse = new Response('typed', {
  headers: { 'content-type': 'text/plain' }
});
test(
  'Response adds charset to explicit plain-text content-type',
  normalizedTypeResponse.headers.get('content-type'),
  'text/plain;charset=UTF-8'
);
const customTextTypeResponse = new Response('typed', {
  headers: { 'content-type': 'text/custom' }
});
test(
  'Response preserves custom text content-type',
  customTextTypeResponse.headers.get('content-type'),
  'text/custom'
);

const cookieRes = new Response('cookie', {
  headers: { 'Set-Cookie': 'session=abc123' }
});
test('Response keeps set-cookie header', cookieRes.headers.get('set-cookie'), 'session=abc123');
test('Response getSetCookie returns set-cookie', cookieRes.headers.getSetCookie().join('|'), 'session=abc123');

testThrows('Response invalid status low', () => new Response('', { status: 199 }));
testThrows('Response invalid status high', () => new Response('', { status: 600 }));
testThrows('Response invalid statusText newline', () => new Response('', { statusText: '\n' }));
testThrows('Response invalid statusText non-ByteString', () => new Response('', { statusText: 'Ā' }));
testThrows('Response null body status rejects body', () => new Response('x', { status: 204 }));

const streamRes = new Response('stream me');
const bodyStream = streamRes.body;
test('Response body stream stable', streamRes.body === bodyStream, true);
test('Response bodyUsed before consume', streamRes.bodyUsed, false);
test('Response text after body access', await streamRes.text(), 'stream me');
test('Response bodyUsed after consume', streamRes.bodyUsed, true);

const cloned = new Response('clone me', {
  status: 202,
  statusText: 'Accepted',
  headers: { 'X-One': '1' }
}).clone();
test('Response clone status', cloned.status, 202);
test('Response clone statusText', cloned.statusText, 'Accepted');
test('Response clone header', cloned.headers.get('x-one'), '1');
test('Response clone text', await cloned.text(), 'clone me');

const jsonRes = Response.json({ hello: 'world' });
test('Response.json content-type', jsonRes.headers.get('content-type'), 'application/json');
test('Response.json body', (await jsonRes.json()).hello, 'world');
let jsonHeadersReads = 0;
const jsonCustomType = Response.json({}, {
  get headers() {
    jsonHeadersReads++;
    return { 'content-type': 'application/custom+json' };
  }
});
test('Response.json reads init headers once', jsonHeadersReads, 1);
test(
  'Response.json preserves explicit content-type',
  jsonCustomType.headers.get('content-type'),
  'application/custom+json'
);
testThrows('Response.json rejects symbol', () => Response.json(Symbol('x')));

testThrows('Response.json propagates serializer errors', () => {
  class CustomError extends Error {}
  Response.json({
    get hello() {
      throw new CustomError('boom');
    }
  });
});

testThrows('Response.json null body status rejects', () => Response.json('x', { status: 204 }));

const f16Body = new Float16Array([1.5, -0.5, 2.25]);
const f16Res = new Response(f16Body);
const f16Bytes = await f16Res.bytes();
const f16Expected = Array.from(new Uint8Array(f16Body.buffer, f16Body.byteOffset, f16Body.byteLength)).join(',');
test('Response bytes from Float16Array type', f16Bytes.constructor, Uint8Array);
test('Response bytes from Float16Array length', f16Bytes.byteLength, f16Body.byteLength);
test('Response bytes from Float16Array raw copy', Array.from(f16Bytes).join(','), f16Expected);

const redirectRes = Response.redirect('https://example.com/next', 301);
test('Response.redirect status', redirectRes.status, 301);
test('Response.redirect location', redirectRes.headers.get('location'), 'https://example.com/next');
test('Response.redirect ok false', redirectRes.ok, false);
testThrows('Response.redirect bad status', () => Response.redirect('https://example.com/', 200));

const errorRes = Response.error();
test('Response.error type', errorRes.type, 'error');
test('Response.error status', errorRes.status, 0);
test('Response.error body null', errorRes.body, null);
testThrows('Response.error headers immutable', () => errorRes.headers.append('x-test', '1'));

summary();
