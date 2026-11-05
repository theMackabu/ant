const assert = require('node:assert');
const { Readable } = require('node:stream');

async function main() {
  assert.strictEqual(typeof Readable.toWeb, 'function');

  let reads = 0;
  const readable = new Readable({
    read() {
      reads++;
      if (reads === 1) this.push(Buffer.from('hello'));
      else if (reads === 2) this.push(Buffer.from(' world'));
      else this.push(null);
    },
  });

  const web = Readable.toWeb(readable);
  assert.strictEqual(typeof web.getReader, 'function');

  const reader = web.getReader();
  const chunks = [];

  for (;;) {
    const result = await reader.read();
    if (result.done) break;
    assert.strictEqual(Object.getPrototypeOf(result.value), Uint8Array.prototype);
    assert.strictEqual(Buffer.isBuffer(result.value), false);
    chunks.push(Buffer.from(result.value));
  }

  assert.strictEqual(Buffer.concat(chunks).toString(), 'hello world');

  const objectChunk = Buffer.from('obj');
  const objectReader = Readable.toWeb(Readable.from([objectChunk], { objectMode: true })).getReader();
  assert.strictEqual((await objectReader.read()).value, objectChunk);

  let controller;
  const deep = new ReadableStream({ start(c) { controller = c; } }, { highWaterMark: 0 });
  const deepReader = deep.getReader();
  let next = 0;
  let expected = 0;
  for (let round = 0; round < 50; round++) {
    for (let i = 0; i < 200; i++) controller.enqueue(next++);
    for (let i = 0; i < 150; i++) assert.strictEqual((await deepReader.read()).value, expected++);
  }
  controller.close();
  for (;;) {
    const result = await deepReader.read();
    if (result.done) break;
    assert.strictEqual(result.value, expected++);
  }
  assert.strictEqual(expected, next);

  console.log('stream-readable-to-web:ok');
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
