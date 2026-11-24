const assert = require('node:assert');
const { Readable, Writable, Duplex, PassThrough, finished, pipeline } = require('node:stream');

function whenFinished(stream, options) {
  return new Promise((resolve) => {
    if (options) finished(stream, options, (error) => resolve(error ?? null));
    else finished(stream, (error) => resolve(error ?? null));
  });
}

async function testPrematureClose() {
  const readable = new Readable({ read() {} });
  const readableDone = whenFinished(readable);
  readable.destroy();
  assert.strictEqual((await readableDone).code, 'ERR_STREAM_PREMATURE_CLOSE');

  const writable = new Writable({ write(chunk, encoding, callback) { callback(); } });
  const writableDone = whenFinished(writable);
  writable.destroy();
  assert.strictEqual((await writableDone).code, 'ERR_STREAM_PREMATURE_CLOSE');

  const reason = new Error('boom');
  const failing = new Readable({ read() {} });
  const failingDone = whenFinished(failing);
  failing.destroy(reason);
  assert.strictEqual(await failingDone, reason);
}

async function testCleanCompletion() {
  const readable = Readable.from(['a']);
  const readableDone = whenFinished(readable);
  readable.resume();
  assert.strictEqual(await readableDone, null);

  const writable = new Writable({ write(chunk, encoding, callback) { callback(); } });
  const writableDone = whenFinished(writable);
  writable.end('x');
  assert.strictEqual(await writableDone, null);
}

async function testDuplexWaitsForBothSides() {
  const duplex = new PassThrough();
  let fired = false;
  const done = whenFinished(duplex).then((error) => { fired = true; return error; });

  duplex.end('x');
  await new Promise((resolve) => duplex.on('finish', resolve));
  await new Promise((resolve) => setImmediate(resolve));
  assert.strictEqual(fired, false);

  duplex.resume();
  assert.strictEqual(await done, null);

  const writeOnly = new PassThrough();
  const writeOnlyDone = whenFinished(writeOnly, { readable: false });
  writeOnly.end('x');
  assert.strictEqual(await writeOnlyDone, null);

  const halfDone = new Duplex({ read() {}, write(chunk, encoding, callback) { callback(); } });
  const halfDoneResult = whenFinished(halfDone);
  halfDone.end();
  await new Promise((resolve) => halfDone.on('finish', resolve));
  halfDone.destroy();
  assert.strictEqual((await halfDoneResult).code, 'ERR_STREAM_PREMATURE_CLOSE');
}

async function testPipeline() {
  const source = new Readable({ read() {} });
  const sink = new Writable({ write(chunk, encoding, callback) { callback(); } });
  const failed = new Promise((resolve) => pipeline(source, sink, (error) => resolve(error ?? null)));
  source.destroy();
  assert.strictEqual((await failed).code, 'ERR_STREAM_PREMATURE_CLOSE');

  const chunks = [];
  const collected = new Promise((resolve) => pipeline(
    Readable.from(['a', 'b']),
    new PassThrough({ objectMode: true }),
    new Writable({ objectMode: true, write(chunk, encoding, callback) { chunks.push(chunk); callback(); } }),
    (error) => resolve(error ?? null),
  ));
  assert.strictEqual(await collected, null);
  assert.deepStrictEqual(chunks, ['a', 'b']);
}

async function main() {
  await testPrematureClose();
  await testCleanCompletion();
  await testDuplexWaitsForBothSides();
  await testPipeline();
  console.log('stream-finished-premature-close:ok');
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
