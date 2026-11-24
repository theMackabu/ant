const assert = require('node:assert');
const { Duplex } = require('node:stream');

async function main() {
  const writeOnly = new Duplex({ readable: false, write(chunk, encoding, callback) { callback(); } });
  assert.strictEqual(writeOnly.readable, false);
  assert.strictEqual(writeOnly.readableEnded, true);
  assert.strictEqual(writeOnly.writable, true);

  const writeOnlyClosed = new Promise((resolve) => writeOnly.on('close', resolve));
  writeOnly.end('x');
  await writeOnlyClosed;
  assert.strictEqual(writeOnly.writableFinished, true);

  const readOnly = new Duplex({ writable: false, read() { this.push('x'); this.push(null); } });
  assert.strictEqual(readOnly.writable, false);
  assert.strictEqual(readOnly.writableEnded, true);
  assert.strictEqual(readOnly.writableFinished, true);
  assert.strictEqual(readOnly.readable, true);

  const chunks = [];
  const readOnlyClosed = new Promise((resolve) => readOnly.on('close', resolve));
  readOnly.on('data', (chunk) => chunks.push(String(chunk)));
  await readOnlyClosed;
  assert.deepStrictEqual(chunks, ['x']);

  const both = new Duplex({ read() {}, write(chunk, encoding, callback) { callback(); } });
  assert.strictEqual(both.readable, true);
  assert.strictEqual(both.writable, true);

  console.log('stream-duplex-one-sided:ok');
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
