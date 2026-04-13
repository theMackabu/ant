const fs = require('node:fs/promises');
const path = require('node:path');

async function main() {
  const file = path.join(__dirname, 'tmp_filehandle.txt');

  try {
    await fs.unlink(file);
  } catch {}

  const writer = await fs.open(file, 'w');
  if (!writer || typeof writer !== 'object') {
    throw new Error(`expected FileHandle object, got ${Object.prototype.toString.call(writer)}`);
  }
  if (typeof writer.fd !== 'number') {
    throw new Error(`expected numeric fd, got ${typeof writer.fd}`);
  }
  if (typeof writer.writeFile !== 'function') {
    throw new Error('expected FileHandle.writeFile()');
  }
  if (typeof writer.sync !== 'function') {
    throw new Error('expected FileHandle.sync()');
  }
  if (typeof writer.close !== 'function') {
    throw new Error('expected FileHandle.close()');
  }

  await writer.writeFile('hello');
  await writer.sync();
  const writerStat = await writer.stat();
  if (!writerStat || typeof writerStat.size !== 'number' || writerStat.size !== 5) {
    throw new Error(`expected stat.size === 5, got ${writerStat && writerStat.size}`);
  }
  await writer.close();

  const reader = await fs.open(file, 'r');
  const buf = Buffer.alloc(5);
  const readResult = await reader.read(buf, 0, buf.length, 0);
  if (!readResult || readResult.bytesRead !== 5) {
    throw new Error(`expected bytesRead === 5, got ${readResult && readResult.bytesRead}`);
  }
  if (readResult.buffer !== buf) {
    throw new Error('expected read() to return the original buffer');
  }
  if (buf.toString('utf8') !== 'hello') {
    throw new Error(`expected "hello", got ${JSON.stringify(buf.toString('utf8'))}`);
  }
  await reader.close();
  await fs.unlink(file);

  console.log('fs/promises FileHandle works');
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
