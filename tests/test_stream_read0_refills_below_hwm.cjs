const assert = require('node:assert');
const { Readable } = require('node:stream');

let reads = 0;
const readable = new Readable({
  objectMode: true,
  highWaterMark: 4,
  read() {
    reads++;
    if (reads === 1) this.push('a');
    else if (reads === 2) this.push('b');
  },
});

readable.read(0);
assert.strictEqual(reads, 1);
assert.strictEqual(readable._readableState.length, 1);

readable.read(0);
assert.strictEqual(reads, 2);
assert.strictEqual(readable._readableState.length, 2);
assert.strictEqual(readable.read().toString(), 'a');
assert.strictEqual(readable.read().toString(), 'b');
assert.strictEqual(readable._readableState.length, 0);

{
  let byteReads = 0;
  const bytes = new Readable({
    highWaterMark: 4,
    read() {
      byteReads++;
      if (byteReads === 1) this.push(Buffer.alloc(4));
      else if (byteReads === 2) this.push(Buffer.alloc(1));
    },
  });

  bytes.read(0);
  assert.strictEqual(byteReads, 1);
  assert.strictEqual(bytes._readableState.length, 4);
  bytes.read(0);
  assert.strictEqual(byteReads, 1);
}

console.log('stream-read0-refills-below-hwm:ok');
