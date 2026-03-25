import { test, testDeep, testThrows, summary } from './helpers.js';

console.log('ReadableStream / DefaultController / DefaultReader Tests\n');

test('RS typeof', typeof ReadableStream, 'function');
test('RS toStringTag', Object.prototype.toString.call(new ReadableStream()), '[object ReadableStream]');

const rs = new ReadableStream();
test('RS locked initially false', rs.locked, false);

testThrows('RS requires new', () => ReadableStream());
testThrows('RS rejects null source', () => new ReadableStream(null));
testThrows('RS rejects invalid type', () => new ReadableStream({ type: 'invalid' }));
testThrows('RS rejects null type', () => new ReadableStream({ type: null }));

const rs2 = new ReadableStream({ type: undefined });
test('RS accepts undefined type', rs2.locked, false);

let startCalled = false;
let startController = null;
const rs3 = new ReadableStream({
  start(c) { startCalled = true; startController = c; }
});
test('start called synchronously', startCalled, true);
test('controller has desiredSize', startController.desiredSize, 1);
test('controller toStringTag', Object.prototype.toString.call(startController), '[object ReadableStreamDefaultController]');

testThrows('controller cannot be constructed', () => new ReadableStreamDefaultController());

const rs4 = new ReadableStream({
  start(c) { c.enqueue('a'); c.enqueue('b'); c.close(); }
});

const reader = rs4.getReader();
test('locked after getReader', rs4.locked, true);
test('reader toStringTag', Object.prototype.toString.call(reader), '[object ReadableStreamDefaultReader]');

testThrows('cannot get second reader', () => rs4.getReader());
testThrows('getReader rejects unknown mode', () => new ReadableStream().getReader({ mode: 'potato' }));

async function testReadSequence() {
  const r1 = await reader.read();
  test('read 1 value', r1.value, 'a');
  test('read 1 done', r1.done, false);

  const r2 = await reader.read();
  test('read 2 value', r2.value, 'b');
  test('read 2 done', r2.done, false);

  const r3 = await reader.read();
  test('read 3 done', r3.done, true);
  test('read 3 value', r3.value, undefined);
}

async function testClosedPromise() {
  const rs = new ReadableStream({
    start(c) { c.close(); }
  });
  const reader = rs.getReader();
  await reader.closed;
  test('closed resolves on close', true, true);
}

async function testCancel() {
  let cancelReason = null;
  const rs = new ReadableStream({
    cancel(reason) { cancelReason = reason; }
  });
  await rs.cancel('test reason');
  test('cancel passes reason', cancelReason, 'test reason');
}

async function testPullBackpressure() {
  let pullCount = 0;
  const rs = new ReadableStream({
    pull(c) {
      pullCount++;
      c.enqueue(pullCount);
    }
  }, { highWaterMark: 1 });

  await new Promise(r => setTimeout(r, 50));
  test('pull called once at start', pullCount, 1);

  const reader = rs.getReader();
  const r1 = await reader.read();
  test('pull backpressure value', r1.value, 1);

  await new Promise(r => setTimeout(r, 50));
  test('pull called again after read', pullCount, 2);
}

async function testReleaseLock() {
  const rs = new ReadableStream({
    start(c) { c.enqueue('x'); }
  });
  const reader = rs.getReader();
  reader.releaseLock();
  test('locked false after release', rs.locked, false);

  const reader2 = rs.getReader();
  const r = await reader2.read();
  test('new reader can read', r.value, 'x');
}

async function testDesiredSize() {
  let ctrl;
  new ReadableStream({
    start(c) {
      ctrl = c;
    }
  });
  await new Promise(r => setTimeout(r, 0));
  test('desiredSize initially 1', ctrl.desiredSize, 1);
  ctrl.enqueue('a');
  test('desiredSize after enqueue', ctrl.desiredSize, 0);
  ctrl.close();
  test('desiredSize after close', ctrl.desiredSize, 0);
}

async function testErrorStream() {
  const err = new Error('boom');
  const rs = new ReadableStream({
    start(c) { c.error(err); }
  });
  const reader = rs.getReader();
  try {
    await reader.read();
    test('errored stream should reject read', false, true);
  } catch (e) {
    test('errored stream rejects with error', e, err);
  }
}

async function testCustomStrategy() {
  const rs = new ReadableStream({
    start(c) {
      c.enqueue(new Uint8Array(10));
      c.enqueue(new Uint8Array(20));
    }
  }, {
    highWaterMark: 64,
    size(chunk) { return chunk.byteLength; }
  });

  const reader = rs.getReader();
  const r1 = await reader.read();
  test('custom strategy chunk size', r1.value.byteLength, 10);
}

async function testSubclass() {
  class MyStream extends ReadableStream {
    myMethod() { return 42; }
  }
  const ms = new MyStream({ start(c) { c.close(); } });
  test('subclass instanceof', ms instanceof ReadableStream, true);
  test('subclass method', ms.myMethod(), 42);
}

await testReadSequence();
await testClosedPromise();
await testCancel();
await testPullBackpressure();
await testReleaseLock();
await testDesiredSize();
await testErrorStream();
await testCustomStrategy();
await testSubclass();

summary();
