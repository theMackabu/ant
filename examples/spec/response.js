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

const res1 = new Response('hello', {
  status: 201,
  statusText: 'Created',
  headers: { 'X-Test': 'ok' }
});
test('Response init status', res1.status, 201);
test('Response init statusText', res1.statusText, 'Created');
test('Response init header', res1.headers.get('x-test'), 'ok');
test('Response init text', await res1.text(), 'hello');

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
testThrows('Response.json rejects symbol', () => Response.json(Symbol('x')));
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
