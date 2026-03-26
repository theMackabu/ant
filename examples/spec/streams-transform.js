import { test, testDeep, testThrows, summary } from './helpers.js';

console.log('TransformStream / TransformStreamDefaultController Tests\n');

test('TS typeof', typeof TransformStream, 'function');
test('TS toStringTag', Object.prototype.toString.call(new TransformStream()), '[object TransformStream]');

testThrows('TS requires new', () => TransformStream());
testThrows('TS rejects readableType', () => new TransformStream({ readableType: 'bytes' }));
testThrows('TS rejects writableType', () => new TransformStream({ writableType: 'bytes' }));

const ts0 = new TransformStream();
test('TS readable is ReadableStream', ts0.readable instanceof ReadableStream, true);
test('TS writable is WritableStream', ts0.writable instanceof WritableStream, true);

testThrows('TSController cannot be constructed', () => new TransformStreamDefaultController());
test('TSController toStringTag', typeof TransformStreamDefaultController, 'function');

test('TS can be constructed with no transform', true, (() => { new TransformStream({}); return true; })());

let startCtrl = null;
const ts1 = new TransformStream({ start(c) { startCtrl = c; } });
test('start called with controller', startCtrl !== null, true);
test('controller toStringTag', Object.prototype.toString.call(startCtrl), '[object TransformStreamDefaultController]');

async function testIdentity() {
  const ts = new TransformStream();
  const writer = ts.writable.getWriter();
  writer.write('a');
  const reader = ts.readable.getReader();
  const result = await reader.read();
  test('identity value', result.value, 'a');
  test('identity done', result.done, false);
}

async function testTransformUppercase() {
  let c;
  const ts = new TransformStream({
    start(controller) { c = controller; },
    transform(chunk) { c.enqueue(chunk.toUpperCase()); }
  });
  const writer = ts.writable.getWriter();
  writer.write('hello');
  const reader = ts.readable.getReader();
  const result = await reader.read();
  test('uppercase transform', result.value, 'HELLO');
}

async function testTransformDoubler() {
  let c;
  const ts = new TransformStream({
    start(controller) { c = controller; },
    transform(chunk) {
      c.enqueue(chunk.toUpperCase());
      c.enqueue(chunk.toUpperCase());
    }
  });
  const writer = ts.writable.getWriter();
  writer.write('x');
  const reader = ts.readable.getReader();
  const r1 = await reader.read();
  const r2 = await reader.read();
  test('doubler chunk 1', r1.value, 'X');
  test('doubler chunk 2', r2.value, 'X');
}

async function testFlush() {
  let flushed = false;
  const ts = new TransformStream({
    transform() {},
    flush() { flushed = true; }
  });
  await ts.writable.getWriter().close();
  test('flush called on close', flushed, true);
}

async function testFlushEnqueue() {
  let c;
  const ts = new TransformStream({
    start(controller) { c = controller; },
    transform() {},
    flush() { c.enqueue('flushed'); }
  });
  const writer = ts.writable.getWriter();
  writer.write('a');
  writer.close();
  const reader = ts.readable.getReader();
  const r1 = await reader.read();
  test('flush enqueue value', r1.value, 'flushed');
}

async function testCloseClosesReadable() {
  const ts = new TransformStream({ transform() {} });
  const writer = ts.writable.getWriter();
  writer.close();
  await Promise.all([writer.closed, ts.readable.getReader().closed]);
  test('close propagates to readable', true, true);
}

async function testTransformError() {
  const err = new Error('transform boom');
  const ts = new TransformStream({
    transform() { throw err; }
  });
  const writer = ts.writable.getWriter();
  const reader = ts.readable.getReader();
  try {
    await writer.write('a');
    test('write should reject on transform error', false, true);
  } catch (e) {
    test('write rejects with transform error', e, err);
  }
  try {
    await reader.read();
    test('read should reject on transform error', false, true);
  } catch (e) {
    test('read rejects with transform error', e, err);
  }
}

async function testFlushError() {
  const err = new Error('flush boom');
  const ts = new TransformStream({
    transform() {},
    flush() { throw err; }
  });
  const writer = ts.writable.getWriter();
  await writer.write('a');
  try {
    await writer.close();
    test('close should reject on flush error', false, true);
  } catch (e) {
    test('close rejects with flush error', e, err);
  }
}

async function testControllerError() {
  let ctrl;
  const ts = new TransformStream({
    start(c) { ctrl = c; }
  });
  ctrl.error(new Error('controller error'));
  const reader = ts.readable.getReader();
  try {
    await reader.read();
    test('read should reject after controller.error', false, true);
  } catch (e) {
    test('controller.error errors readable', e.message, 'controller error');
  }
}

async function testControllerTerminate() {
  let ctrl;
  const ts = new TransformStream({
    start(c) { ctrl = c; }
  });
  ctrl.terminate();
  const reader = ts.readable.getReader();
  const result = await reader.read();
  test('terminate closes readable', result.done, true);
}

async function testDesiredSize() {
  let ctrl;
  const ts = new TransformStream({
    start(c) { ctrl = c; }
  });
  test('desiredSize initially 0', ctrl.desiredSize, 0);
}

async function testDefaultHWM() {
  const ts = new TransformStream();
  const writer = ts.writable.getWriter();
  test('writable default HWM is 1', writer.desiredSize, 1);
}

async function testCustomWritableHWM() {
  const ts = new TransformStream({}, { highWaterMark: 17 });
  const writer = ts.writable.getWriter();
  test('writable custom HWM', writer.desiredSize, 17);
}

async function testBackpressure() {
  const ts = new TransformStream(undefined, undefined, { highWaterMark: 0 });
  const writer = ts.writable.getWriter();
  const reader = ts.readable.getReader();
  const readPromise = reader.read();
  writer.write('a');
  const result = await readPromise;
  test('backpressure read value', result.value, 'a');
  test('backpressure read done', result.done, false);
}

async function testAsyncTransform() {
  let c;
  const ts = new TransformStream({
    start(controller) { c = controller; },
    transform(chunk) {
      return new Promise(resolve => {
        setTimeout(() => {
          c.enqueue(chunk.toUpperCase());
          resolve();
        }, 10);
      });
    }
  });
  const writer = ts.writable.getWriter();
  writer.write('a');
  const reader = ts.readable.getReader();
  const result = await reader.read();
  test('async transform value', result.value, 'A');
}

async function testStartThrows() {
  try {
    new TransformStream({
      start() { throw new URIError('start thrown'); }
    });
    test('TS ctor should throw on start error', false, true);
  } catch (e) {
    test('TS ctor throws start error', e instanceof URIError, true);
  }
}

async function testCancelCallable() {
  let cancelled = null;
  const reason = new Error('cancel reason');
  const ts = new TransformStream({
    cancel(r) { cancelled = r; }
  });
  await ts.readable.cancel(reason);
  test('cancel called with reason', cancelled, reason);
}

async function testAbortCallsCancel() {
  let aborted = null;
  const reason = new Error('abort reason');
  const ts = new TransformStream({
    cancel(r) { aborted = r; }
  });
  await ts.writable.abort(reason);
  test('abort calls cancel with reason', aborted, reason);
}

async function testTransformThisBinding() {
  let c;
  const ts = new TransformStream({
    suffix: '-suffix',
    start(controller) { c = controller; },
    transform(chunk) { c.enqueue(chunk + this.suffix); },
    flush() { c.enqueue('flushed' + this.suffix); }
  });
  const writer = ts.writable.getWriter();
  writer.write('a');
  writer.close();
  const reader = ts.readable.getReader();
  const r1 = await reader.read();
  test('this binding transform', r1.value, 'a-suffix');
  const r2 = await reader.read();
  test('this binding flush', r2.value, 'flushed-suffix');
}

async function testSubclass() {
  class MyTS extends TransformStream {
    myMethod() { return 42; }
  }
  const ts = new MyTS();
  test('subclass instanceof', ts instanceof TransformStream, true);
  test('subclass method', ts.myMethod(), 42);
  test('subclass readable', ts.readable instanceof ReadableStream, true);
}

async function testReadableHWM() {
  let ctrl;
  const ts = new TransformStream({
    start(c) { ctrl = c; }
  }, undefined, { highWaterMark: 9 });
  test('readable custom HWM desiredSize', ctrl.desiredSize, 9);
}

async function testNegativeHWMThrows() {
  testThrows('negative writable HWM', () => new TransformStream(undefined, { highWaterMark: -1 }));
  testThrows('negative readable HWM', () => new TransformStream(undefined, undefined, { highWaterMark: -1 }));
  testThrows('NaN writable HWM', () => new TransformStream(undefined, { highWaterMark: NaN }));
  testThrows('NaN readable HWM', () => new TransformStream(undefined, undefined, { highWaterMark: NaN }));
}

async function testCancelReadableErrorsWritable() {
  const err = new Error('cancel reason');
  const ts = new TransformStream();
  const writer = ts.writable.getWriter();
  ts.readable.cancel(err);
  try {
    await writer.closed;
    test('writer.closed should reject after cancel', false, true);
  } catch (e) {
    test('writer.closed rejects with cancel reason', e, err);
  }
}

async function testWritableStrategySize() {
  let writableSizeCalled = false;
  let readableSizeCalled = false;
  const ts = new TransformStream(
    {
      transform(chunk, controller) {
        controller.enqueue(chunk);
      }
    },
    {
      size() { writableSizeCalled = true; return 1; }
    },
    {
      size() { readableSizeCalled = true; return 1; },
      highWaterMark: Infinity
    }
  );
  await ts.writable.getWriter().write('x');
  test('writable size called', writableSizeCalled, true);
  test('readable size called', readableSizeCalled, true);
}

async function testPipeThrough() {
  const rs = new ReadableStream({
    start(c) { c.enqueue(1); c.enqueue(2); c.enqueue(3); c.close(); }
  });
  const ts = new TransformStream({
    transform(chunk, controller) {
      controller.enqueue(chunk * 10);
    }
  });
  const result = rs.pipeThrough(ts);
  test('pipeThrough returns readable', result instanceof ReadableStream, true);
  const reader = result.getReader();
  const chunks = [];
  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    chunks.push(value);
  }
  testDeep('pipeThrough transforms data', chunks, [10, 20, 30]);
}

async function testEnqueueAfterTerminateThrows() {
  let threw = false;
  new TransformStream({
    start(controller) {
      controller.terminate();
      try {
        controller.enqueue('x');
      } catch (e) {
        threw = true;
      }
    }
  });
  test('enqueue after terminate throws', threw, true);
}

await testIdentity();
await testTransformUppercase();
await testTransformDoubler();
await testFlush();
await testFlushEnqueue();
await testCloseClosesReadable();
await testTransformError();
await testFlushError();
await testControllerError();
await testControllerTerminate();
await testDesiredSize();
await testDefaultHWM();
await testCustomWritableHWM();
await testBackpressure();
await testAsyncTransform();
await testStartThrows();
await testCancelCallable();
await testAbortCallsCancel();
await testTransformThisBinding();
await testSubclass();
await testReadableHWM();
testNegativeHWMThrows();
await testCancelReadableErrorsWritable();
await testWritableStrategySize();
await testPipeThrough();
await testEnqueueAfterTerminateThrows();

summary();
