const assert = require('node:assert');
const fs = require('node:fs');
const { Readable } = require('node:stream');
function bad(reason, value) {
  const promise = Promise.resolve(value);
  Object.defineProperty(promise, 'constructor', { get() { throw reason; } });
  return promise;
}
async function rejected(promise, reason) {
  let caught = false;
  try { await promise; } catch (error) { caught = true; assert.strictEqual(error, reason); }
  assert.ok(caught, 'operation did not reject');
}
const deadline = setTimeout(() => { console.error('continuation operation remained pending'); process.exit(1); }, 5000);
async function main() {
  for (const reason of [undefined, null, new Error('continuation')]) {
    const readable = new ReadableStream({ start() { return bad(reason); } });
    const reader = readable.getReader();
    await Promise.all([rejected(reader.read(), reason), rejected(reader.closed, reason)]);
    const writable = new WritableStream({ start() { return bad(reason); } });
    const writer = writable.getWriter();
    await Promise.all([rejected(writer.write('x'), reason), rejected(writer.closed, reason), rejected(writer.ready, reason)]);
    const transform = new TransformStream({ start() { return bad(reason); } });
    const tr = transform.readable.getReader();
    const tw = transform.writable.getWriter();
    await Promise.all([rejected(tr.read(), reason), rejected(tr.closed, reason), rejected(tw.write('x'), reason), rejected(tw.closed, reason), rejected(tw.ready, reason)]);
    await rejected(new ReadableStream({ cancel() { return bad(reason); } }).cancel(), reason);
    const stack = new AsyncDisposableStack();
    let remaining = false;
    stack.defer(() => { remaining = true; });
    stack.defer(() => bad(reason));
    await rejected(stack.disposeAsync(), reason);
    assert.ok(remaining, 'disposal stopped before remaining records');
  }
  const reason = new Error('node stream continuation');
  await new Promise((resolve, reject) => {
    const readable = Readable.from({ [Symbol.asyncIterator]() { return this; }, next() { return bad(reason, { done: true }); } });
    readable.on('error', error => { try { assert.strictEqual(error, reason); resolve(); } catch (error) { reject(error); } });
    readable.on('data', () => reject(new Error('unexpected data')));
    readable.on('end', () => reject(new Error('error became end')));
  });
  let callbackCalls = 0;
  let returned = false;
  let finish;
  const callbackDone = new Promise(resolve => { finish = resolve; });
  const descriptor = Object.getOwnPropertyDescriptor(Promise.prototype, 'constructor');
  Object.defineProperty(Promise.prototype, 'constructor', { configurable: true, get() { throw reason; } });
  try {
    fs.stat(__filename, error => {
      assert.ok(returned, 'fs callback ran synchronously');
      assert.strictEqual(error, reason);
      callbackCalls++;
      finish();
    });
    returned = true;
  } finally { Object.defineProperty(Promise.prototype, 'constructor', descriptor); }
  await callbackDone;
  assert.strictEqual(callbackCalls, 1);

  for (const kind of ['request', 'response']) {
    const body = new ReadableStream({ start(controller) {
      controller.enqueue(new Uint8Array([120]));
      controller.close();
    } });
    const message = kind === 'request'
      ? new Request('http://localhost/', { method: 'POST', body, duplex: 'half' })
      : new Response(body);
    let result;
    Object.defineProperty(Promise.prototype, 'constructor', { configurable: true, get() { throw reason; } });
    try { result = message.text(); }
    finally { Object.defineProperty(Promise.prototype, 'constructor', descriptor); }
    await rejected(result, reason);
  }

  const server = Ant.serve({ port: 0, fetch() { return bad(reason, new Response('unused')); } });
  try {
    const response = await fetch(`http://127.0.0.1:${server.port}`);
    assert.strictEqual(response.status, 500, 'server did not finish failed response');
    await response.text();
  } finally { server.stop(true); }
  clearTimeout(deadline);
  console.log('Promise continuation errors ok');
}
main().catch(error => { clearTimeout(deadline); console.error(error); process.exit(1); });
