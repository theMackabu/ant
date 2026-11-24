const assert = require('node:assert');
const { Readable, Writable, Duplex, Transform, PassThrough } = require('node:stream');

function collect(stream) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    stream.on('error', reject);
    stream.on('data', (chunk) => chunks.push(chunk));
    stream.on('end', () => resolve(chunks));
  });
}

function finished(stream) {
  return new Promise((resolve, reject) => {
    stream.on('error', reject);
    stream.on('finish', resolve);
  });
}

function sink(options) {
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

async function testStatics() {
  assert.strictEqual(typeof Duplex.from, 'function');
  assert.notStrictEqual(Duplex.from, Readable.from);
  assert.strictEqual(Transform.from, Duplex.from);
  assert.strictEqual(PassThrough.from, Duplex.from);
  assert.throws(() => Duplex.from(123), { code: 'ERR_INVALID_ARG_TYPE' });
  assert.throws(() => Duplex.from(() => 1), { code: 'ERR_INVALID_RETURN_VALUE' });

  const duplex = new PassThrough();
  assert.strictEqual(Duplex.from(duplex), duplex);
}

async function testReadableSources() {
  const fromReadable = Duplex.from(Readable.from(['a', 'b']));
  assert.ok(fromReadable instanceof Duplex);
  assert.deepStrictEqual(await collect(fromReadable), ['a', 'b']);

  assert.deepStrictEqual(await collect(Duplex.from(['x', 'y', 'z'])), ['x', 'y', 'z']);
  assert.deepStrictEqual(await collect(Duplex.from('whole')), ['whole']);
  assert.deepStrictEqual(await collect(Duplex.from(Promise.resolve('later'))), ['later']);
  assert.deepStrictEqual(await collect(Duplex.from({ then(resolve) { setImmediate(resolve, 'thenable'); } })), ['thenable']);

  async function* generate() {
    yield 1;
    yield 2;
  }
  assert.deepStrictEqual(await collect(Duplex.from(generate())), [1, 2]);

  const web = new ReadableStream({
    start(controller) {
      controller.enqueue(new TextEncoder().encode('web'));
      controller.close();
    },
  });
  assert.strictEqual(Buffer.concat(await collect(Duplex.from(web))).toString(), 'web');

  const reason = new Error('rejected');
  const failing = Duplex.from(Promise.reject(reason));
  assert.strictEqual(await new Promise((resolve) => failing.on('error', resolve)), reason);
}

async function testWritableSources() {
  const { chunks, writable } = sink();
  const duplex = Duplex.from(writable);
  assert.ok(duplex instanceof Duplex);
  duplex.write('hello');
  duplex.end(' world');
  await finished(duplex);
  assert.strictEqual(Buffer.concat(chunks).toString(), 'hello world');

  const webChunks = [];
  const fromWeb = Duplex.from(new WritableStream({ write(chunk) { webChunks.push(Buffer.from(chunk)); } }));
  fromWeb.end('web');
  await finished(fromWeb);
  assert.strictEqual(Buffer.concat(webChunks).toString(), 'web');
}

async function testPairs() {
  const { chunks, writable } = sink({ objectMode: true });
  const duplex = Duplex.from({ readable: Readable.from(['out']), writable });
  const output = collect(duplex);
  duplex.end({ in: true });
  assert.deepStrictEqual(await output, ['out']);
  assert.deepStrictEqual(chunks, [{ in: true }]);

  const upper = Duplex.from(new TransformStream({
    transform(chunk, controller) {
      controller.enqueue(new TextEncoder().encode(Buffer.from(chunk).toString().toUpperCase()));
    },
  }));
  const transformed = collect(upper);
  upper.end('shout');
  assert.strictEqual(Buffer.concat(await transformed).toString(), 'SHOUT');
}

async function testFunctions() {
  const mapped = Duplex.from(async function* (source) {
    for await (const chunk of source) yield String(chunk).toUpperCase();
  });
  const output = collect(mapped);
  mapped.write('a');
  mapped.write('b');
  mapped.end('c');
  assert.deepStrictEqual(await output, ['A', 'B', 'C']);

  const seen = [];
  let settled = false;
  const consumer = Duplex.from(async function (source) {
    for await (const chunk of source) seen.push(String(chunk));
    await new Promise((resolve) => setImmediate(resolve));
    settled = true;
  });
  consumer.write('one');
  consumer.end('two');
  await finished(consumer);
  assert.deepStrictEqual(seen, ['one', 'two']);
  assert.strictEqual(settled, true);

  let signal;
  const aborted = Duplex.from(async function (source, options) {
    signal = options.signal;
    for await (const chunk of source) assert.fail(String(chunk));
  });
  aborted.on('error', () => {});
  await new Promise((resolve) => {
    aborted.on('close', resolve);
    aborted.destroy();
  });
  assert.strictEqual(signal.aborted, true);

  const reason = new Error('generator failed');
  const throwing = Duplex.from(async function* () {
    throw reason;
  });
  throwing.resume();
  assert.strictEqual(await new Promise((resolve) => throwing.on('error', resolve)), reason);
}

async function main() {
  await testStatics();
  await testReadableSources();
  await testWritableSources();
  await testPairs();
  await testFunctions();
  console.log('stream-duplex-from:ok');
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
