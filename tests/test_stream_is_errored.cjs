const assert = require('node:assert');
const stream = require('node:stream');

function tick() {
  return new Promise((resolve) => setImmediate(resolve));
}

(async () => {
  {
    const err = new Error('readable boom');
    const readable = new stream.Readable({ read() {} });
    readable.on('error', () => {});

    assert.strictEqual(stream.isErrored(readable), false);
    readable.destroy(err);
    assert.strictEqual(readable._readableState.errored, err);
    await tick();
    assert.strictEqual(stream.isErrored(readable), true);
  }

  {
    const err = new Error('writable boom');
    const writable = new stream.Writable({
      write(_chunk, _encoding, callback) {
        callback(err);
      },
    });
    writable.on('error', () => {});

    assert.strictEqual(stream.isErrored(writable), false);
    writable.write('x', () => {});
    assert.strictEqual(writable._writableState.errored, err);
    await tick();
    assert.strictEqual(stream.isErrored(writable), true);
  }

  console.log('stream-is-errored:ok');
})().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
