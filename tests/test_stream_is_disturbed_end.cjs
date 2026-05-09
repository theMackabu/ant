const assert = require('node:assert');
const stream = require('node:stream');

async function endedWithoutDataIsReusable() {
  const readable = new stream.Readable({ read() {} });
  const ended = new Promise((resolve) => readable.once('end', resolve));

  readable.resume();
  readable.push(null);
  await ended;

  assert.strictEqual(readable._readableState.endEmitted, true);
  assert.strictEqual(stream.isDisturbed(readable), false);
}

function readingDataDisturbs() {
  const readable = new stream.Readable({ read() {} });
  readable.push(Buffer.from('ok'));
  readable.push(null);

  assert.strictEqual(stream.isDisturbed(readable), false);
  assert.strictEqual(readable.read().toString(), 'ok');
  assert.strictEqual(stream.isDisturbed(readable), true);
}

endedWithoutDataIsReusable().then(() => {
  readingDataDisturbs();
  console.log('stream:is-disturbed-end:ok');
});
