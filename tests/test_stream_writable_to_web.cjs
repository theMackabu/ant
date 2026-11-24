const assert = require('node:assert');
const { Stream, Readable, Writable, Duplex, Transform, PassThrough } = require('node:stream');

function collector(options) {
  const chunks = [];
  const writable = new Writable({
    ...options,
    write(chunk, encoding, callback) {
      chunks.push(chunk);
      setImmediate(callback);
    },
  });
  return { chunks, writable };
}

async function testToWeb() {
  assert.strictEqual(typeof Writable.toWeb, 'function');
  assert.throws(() => Writable.toWeb({}), { code: 'ERR_INVALID_ARG_TYPE' });

  const { chunks, writable } = collector({ highWaterMark: 4 });
  let finished = false;
  writable.on('finish', () => { finished = true; });

  const web = Writable.toWeb(writable);
  assert.ok(web instanceof WritableStream);

  const writer = web.getWriter();
  await writer.write(new TextEncoder().encode('hello'));
  await writer.write(new TextEncoder().encode(' world'));
  await writer.close();

  assert.strictEqual(finished, true);
  assert.strictEqual(Buffer.concat(chunks).toString(), 'hello world');
}

async function testToWebObjectMode() {
  const { chunks, writable } = collector({ objectMode: true });
  const writer = Writable.toWeb(writable).getWriter();
  const value = { a: 1 };
  await writer.write(value);
  await writer.close();
  assert.strictEqual(chunks[0], value);
}

async function testToWebAbort() {
  const { writable } = collector();
  const errored = new Promise((resolve) => writable.on('error', resolve));
  const writer = Writable.toWeb(writable).getWriter();
  const reason = new Error('stop');
  await writer.abort(reason);
  assert.strictEqual(await errored, reason);
  assert.strictEqual(writable.destroyed, true);
}

async function testToWebSourceError() {
  const reason = new Error('sink failed');
  const writable = new Writable({ write(chunk, encoding, callback) { callback(reason); } });
  writable.on('error', () => {});
  const writer = Writable.toWeb(writable).getWriter();
  writer.write(new Uint8Array(1)).catch(() => {});
  await assert.rejects(writer.closed, (error) => error === reason);
}

async function testFromWeb() {
  assert.strictEqual(typeof Writable.fromWeb, 'function');
  assert.throws(() => Writable.fromWeb({}), { code: 'ERR_INVALID_ARG_TYPE' });

  const chunks = [];
  let closed = false;
  const web = new WritableStream({
    async write(chunk) {
      await new Promise((resolve) => setImmediate(resolve));
      chunks.push(Buffer.from(chunk));
    },
    close() { closed = true; },
  });

  const writable = Writable.fromWeb(web);
  assert.ok(writable instanceof Writable);
  assert.strictEqual(web.locked, true);

  writable.write('hello');
  writable.write(Buffer.from(' world'));
  await new Promise((resolve, reject) => {
    writable.on('error', reject);
    writable.end(resolve);
  });

  assert.strictEqual(closed, true);
  assert.strictEqual(Buffer.concat(chunks).toString(), 'hello world');
}

async function testFromWebErrors() {
  const reason = new Error('web sink failed');
  const failing = Writable.fromWeb(new WritableStream({ write() { throw reason; } }));
  const errored = new Promise((resolve) => failing.on('error', resolve));
  failing.write('x');
  assert.strictEqual(await errored, reason);

  let aborted;
  const web = new WritableStream({ abort(error) { aborted = error; } });
  const writable = Writable.fromWeb(web);
  const destroyReason = new Error('destroyed');
  const closed = new Promise((resolve) => writable.on('close', resolve));
  writable.on('error', () => {});
  writable.destroy(destroyReason);
  await closed;
  assert.strictEqual(aborted, destroyReason);
}

async function testDuplexToWeb() {
  assert.strictEqual(typeof Duplex.toWeb, 'function');
  assert.strictEqual(Object.getPrototypeOf(Duplex), Readable);
  assert.strictEqual(Object.getPrototypeOf(Transform), Duplex);
  assert.strictEqual(Object.getPrototypeOf(PassThrough), Transform);
  assert.strictEqual(Object.getPrototypeOf(Writable), Stream);
  assert.strictEqual(Transform.toWeb, Duplex.toWeb);
  assert.strictEqual(PassThrough.fromWeb, Duplex.fromWeb);
  assert.notStrictEqual(Duplex.toWeb, Readable.toWeb);

  const pair = Duplex.toWeb(new PassThrough());
  assert.ok(pair.readable instanceof ReadableStream);
  assert.ok(pair.writable instanceof WritableStream);

  const writer = pair.writable.getWriter();
  const reader = pair.readable.getReader();
  await writer.write(new TextEncoder().encode('ping'));
  assert.strictEqual(Buffer.from((await reader.read()).value).toString(), 'ping');
  await writer.close();
  assert.strictEqual((await reader.read()).done, true);
}

async function testDuplexFromWeb() {
  assert.strictEqual(typeof Duplex.fromWeb, 'function');
  assert.throws(() => Duplex.fromWeb({}), { code: 'ERR_INVALID_ARG_TYPE' });

  const duplex = Duplex.fromWeb(new TransformStream({
    transform(chunk, controller) {
      controller.enqueue(new TextEncoder().encode(Buffer.from(chunk).toString().toUpperCase()));
    },
  }));
  assert.ok(duplex instanceof Duplex);

  const chunks = [];
  const ended = new Promise((resolve, reject) => {
    duplex.on('error', reject);
    duplex.on('data', (chunk) => chunks.push(chunk));
    duplex.on('end', resolve);
  });

  duplex.write('hello');
  duplex.end(' world');
  await ended;

  assert.strictEqual(Buffer.concat(chunks).toString(), 'HELLO WORLD');
}

async function main() {
  await testToWeb();
  await testToWebObjectMode();
  await testToWebAbort();
  await testToWebSourceError();
  await testFromWeb();
  await testFromWebErrors();
  await testDuplexToWeb();
  await testDuplexFromWeb();
  console.log('stream-writable-to-web:ok');
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
