const assert = require('node:assert');
const stream = require('node:stream');

const readable = new stream.Readable({
  read() {
    this.push(Buffer.from('ok'));
    this.push(null);
  }
});

assert.strictEqual(stream.isReadable(readable), true);
assert.strictEqual(stream.isErrored(readable), false);
assert.strictEqual(stream.isDestroyed(readable), false);
assert.strictEqual(stream.isDisturbed(readable), false);

readable.destroy();
assert.strictEqual(stream.isDestroyed(readable), true);
assert.strictEqual(stream.isReadable(readable), false);

console.log('stream:static-state-predicates:ok');
