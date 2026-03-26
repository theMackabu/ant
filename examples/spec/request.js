import { test, testThrows, summary } from './helpers.js';

console.log('Request Tests\n');

test('Request typeof', typeof Request, 'function');
test('Request toStringTag', Object.prototype.toString.call(new Request('https://example.com/')), '[object Request]');
testThrows('Request requires new', () => Request('https://example.com/'));

const req0 = new Request('https://example.com/');
test('Request method default', req0.method, 'GET');
test('Request url', req0.url, 'https://example.com/');
test('Request headers same object', req0.headers === req0.headers, true);
test('Request keepalive default', req0.keepalive, false);
test('Request duplex getter', req0.duplex, 'half');

const hdrReq = new Request('https://example.com/', {
  headers: {
    'Accept-Charset': 'blocked',
    'X-Custom': 'ok',
  },
});
test('Request filters forbidden headers', hdrReq.headers.get('accept-charset'), null);
test('Request keeps allowed headers', hdrReq.headers.get('x-custom'), 'ok');

const noCorsReq = new Request('https://example.com/', {
  mode: 'no-cors',
  headers: {
    'Content-Type': 'text/plain;charset=UTF-8',
    'X-Blocked': 'nope',
  },
});
test('Request no-cors keeps safelisted content-type', noCorsReq.headers.get('content-type'), 'text/plain;charset=UTF-8');
test('Request no-cors strips non-safelisted header', noCorsReq.headers.get('x-blocked'), null);

testThrows(
  'Request forbids keepalive with stream body',
  () => new Request('https://example.com/', {
    method: 'POST',
    body: new ReadableStream(),
    duplex: 'half',
    keepalive: true,
  })
);

testThrows(
  'Request requires duplex for stream body',
  () => new Request('https://example.com/', {
    method: 'POST',
    body: new ReadableStream(),
  })
);

testThrows(
  'Request rejects duplex full',
  () => new Request('https://example.com/', {
    method: 'POST',
    body: 'hello',
    duplex: 'full',
  })
);

testThrows(
  'Request rejects forbidden method',
  () => new Request('https://example.com/', { method: 'TRACE' })
);

testThrows(
  'Request rejects GET with direct body',
  () => new Request('https://example.com/', { method: 'GET', body: 'hello' })
);

const bodyReq = new Request('https://example.com/', {
  method: 'POST',
  body: 'hello world',
});
const bodyStream = bodyReq.body;
test('Request body is a ReadableStream', bodyStream instanceof ReadableStream, true);
test('Request body getter is stable', bodyReq.body === bodyStream, true);
test('Request bodyUsed before consume', bodyReq.bodyUsed, false);
test('Request text after body access', await bodyReq.text(), 'hello world');
test('Request bodyUsed after consume', bodyReq.bodyUsed, true);
test('Request body object preserved after consume', bodyReq.body === bodyStream, true);

const srcReq = new Request('https://example.com/', {
  method: 'POST',
  body: 'copied body',
});
const srcBody = srcReq.body;
const copiedReq = new Request(srcReq);
test('Request-from-Request disturbs source', srcReq.bodyUsed, true);
test('Request-from-Request keeps source body object', srcReq.body === srcBody, true);
test('Request-from-Request creates new body object', copiedReq.body === srcBody, false);
test('Request-from-Request text', await copiedReq.text(), 'copied body');

const failReq = new Request('https://example.com/', {
  method: 'POST',
  body: 'will stay unused',
});
testThrows(
  'Request GET from Request with body throws',
  () => new Request(failReq, { method: 'GET' })
);
test('Request failed construction does not disturb source', failReq.bodyUsed, false);

const usedReq = new Request('https://example.com/', {
  method: 'POST',
  body: 'replace me',
});
await usedReq.text();
const replacedReq = new Request(usedReq, {
  body: 'replacement body',
  method: 'POST',
});
test('Request override from disturbed request succeeds', await replacedReq.text(), 'replacement body');

const initReq = new Request('https://example.com/', {
  method: 'POST',
  body: 'init body',
});
const reqFromInitReq = new Request('https://example.com/', initReq);
test('Request init from Request copies method', reqFromInitReq.method, 'POST');
test('Request init from Request copies body', await reqFromInitReq.text(), 'init body');

const emptyTypeReq = new Request('https://example.com/', {
  method: 'POST',
  headers: [['Content-Type', '']],
  body: 'typed body',
});
const emptyTypeBlob = await emptyTypeReq.blob();
test('Request empty content-type preserved on blob', emptyTypeBlob.type, '');

summary();
