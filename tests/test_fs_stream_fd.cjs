'use strict';

const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const watchdog = setTimeout(() => {
  console.error('fs stream fd test timed out');
  process.exit(1);
}, 30000);

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-fs-fd-'));
const file = path.join(dir, 'data.txt');
fs.writeFileSync(file, 'abcdefghijklmnopqrstuvwxyz');

function isOpen(fd) {
  try {
    fs.fstatSync(fd);
    return true;
  } catch {
    return false;
  }
}

function collect(stream) {
  return new Promise((resolve, reject) => {
    const events = [];
    const chunks = [];
    for (const name of ['open', 'ready', 'end', 'close']) stream.on(name, () => events.push(name));
    stream.on('error', reject);
    stream.on('data', chunk => chunks.push(Buffer.from(chunk)));
    let settled = false;
    const settle = () => {
      if (settled) return;
      settled = true;
      setTimeout(() => resolve({ text: Buffer.concat(chunks).toString(), events }), 30);
    };
    stream.on('close', settle);
    stream.on('end', settle);
  });
}

(async () => {
  {
    const stream = fs.createReadStream(file, { highWaterMark: 8 });
    assert.strictEqual(stream.pending, true);
    assert.strictEqual(stream.fd, null);
    const result = await collect(stream);
    assert.strictEqual(result.text, 'abcdefghijklmnopqrstuvwxyz');
    assert.deepStrictEqual(result.events, ['open', 'ready', 'end', 'close']);
  }

  {
    const fd = fs.openSync(file, 'r');
    const stream = fs.createReadStream(null, { fd, highWaterMark: 8 });
    assert.strictEqual(stream.pending, false);
    assert.strictEqual(stream.fd, fd);
    assert.strictEqual(stream.path, undefined);
    const result = await collect(stream);
    assert.strictEqual(result.text, 'abcdefghijklmnopqrstuvwxyz');
    assert.deepStrictEqual(result.events, ['end', 'close']);
    assert.strictEqual(stream.bytesRead, 26);
    assert.strictEqual(isOpen(fd), false);
  }

  {
    const fd = fs.openSync(file, 'r');
    const result = await collect(fs.createReadStream(path.join(dir, 'missing', 'path'), { fd }));
    assert.strictEqual(result.text, 'abcdefghijklmnopqrstuvwxyz');
    assert.strictEqual(isOpen(fd), false);
  }

  {
    const fd = fs.openSync(file, 'r');
    const first = await collect(fs.createReadStream(null, { fd, start: 3, end: 9, autoClose: false }));
    assert.strictEqual(first.text, 'defghij');
    assert.deepStrictEqual(first.events, ['end']);
    assert.strictEqual(isOpen(fd), true);
    const second = await collect(fs.createReadStream(null, { fd, start: 20, autoClose: false }));
    assert.strictEqual(second.text, 'uvwxyz');
    assert.strictEqual(isOpen(fd), true);
    fs.closeSync(fd);
  }

  {
    const fd = fs.openSync(file, 'r');
    fs.readSync(fd, Buffer.alloc(5), 0, 5, null);
    const result = await collect(fs.createReadStream(null, { fd }));
    assert.strictEqual(result.text, 'fghijklmnopqrstuvwxyz');
  }

  {
    const handle = await fs.promises.open(file, 'r');
    const result = await collect(fs.createReadStream(null, { fd: handle, highWaterMark: 10 }));
    assert.strictEqual(result.text, 'abcdefghijklmnopqrstuvwxyz');
    assert.strictEqual(handle.fd, -1);
  }

  {
    const handle = await fs.promises.open(file, 'r');
    const result = await collect(handle.createReadStream({ start: 2, end: 4 }));
    assert.strictEqual(result.text, 'cde');
    assert.strictEqual(handle.fd, -1);
  }

  {
    const fd = fs.openSync(file, 'r');
    const stream = fs.createReadStream(null, { fd });
    stream.once('data', () => stream.destroy());
    await new Promise(resolve => stream.on('close', resolve));
    assert.strictEqual(isOpen(fd), false);
  }

  assert.throws(() => fs.createReadStream(file, { start: 5, end: 2 }), {
    name: 'RangeError',
    code: 'ERR_OUT_OF_RANGE',
    message: 'The value of "start" is out of range. It must be <= "end" (here: 2). Received 5',
  });
  assert.throws(() => fs.createReadStream(file, { start: -1 }), { name: 'RangeError', code: 'ERR_OUT_OF_RANGE' });
  assert.throws(() => fs.createReadStream(null), { name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE' });
  assert.throws(() => fs.createReadStream(null, { fd: 'x' }), { name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE' });

  {
    const script = `
      const fs = require('fs');
      const stream = fs.createReadStream(null, { fd: 0, encoding: 'utf8' });
      let text = '';
      stream.on('data', chunk => { text += chunk; });
      stream.on('end', () => console.log(JSON.stringify(text)));
    `;
    const piped = spawnSync(process.execPath, ['-e', script], { input: 'piped-input', encoding: 'utf8' });
    assert.strictEqual(piped.stdout.trim(), '"piped-input"', piped.stderr);

    const fd = fs.openSync(file, 'r');
    const fromFile = spawnSync(process.execPath, ['-e', script], { stdio: [fd, 'pipe', 'pipe'], encoding: 'utf8' });
    fs.closeSync(fd);
    assert.strictEqual(fromFile.stdout.trim(), '"abcdefghijklmnopqrstuvwxyz"', fromFile.stderr);
  }

  {
    const target = path.join(dir, 'w1.txt');
    const fd = fs.openSync(target, 'w');
    const stream = fs.createWriteStream(null, { fd });
    const events = [];
    for (const name of ['open', 'ready', 'finish', 'close']) stream.on(name, () => events.push(name));
    stream.write('hello ');
    stream.end('world');
    await new Promise(resolve => stream.on('close', resolve));
    assert.strictEqual(fs.readFileSync(target, 'utf8'), 'hello world');
    assert.deepStrictEqual(events, ['finish', 'close']);
    assert.strictEqual(stream.bytesWritten, 11);
    assert.strictEqual(isOpen(fd), false);
  }

  {
    const target = path.join(dir, 'w2.txt');
    const stream = fs.createWriteStream(target);
    const events = [];
    for (const name of ['open', 'ready', 'finish', 'close']) stream.on(name, () => events.push(name));
    assert.strictEqual(stream.pending, true);
    await new Promise(resolve => stream.on('open', resolve));
    assert.strictEqual(typeof stream.fd, 'number');
    stream.end('x');
    await new Promise(resolve => stream.on('close', resolve));
    assert.deepStrictEqual(events, ['open', 'ready', 'finish', 'close']);
  }

  {
    const target = path.join(dir, 'w3.txt');
    fs.writeFileSync(target, '0123456789');
    const fd = fs.openSync(target, 'r+');
    const stream = fs.createWriteStream(null, { fd, start: 2, autoClose: false });
    stream.end('AB');
    await new Promise(resolve => stream.on('finish', resolve));
    await new Promise(resolve => setTimeout(resolve, 10));
    assert.strictEqual(fs.readFileSync(target, 'utf8'), '01AB456789');
    assert.strictEqual(isOpen(fd), true);
    fs.closeSync(fd);
  }

  {
    const target = path.join(dir, 'w4.txt');
    const handle = await fs.promises.open(target, 'w');
    const stream = fs.createWriteStream(null, { fd: handle });
    stream.end('via-handle');
    await new Promise(resolve => stream.on('close', resolve));
    assert.strictEqual(fs.readFileSync(target, 'utf8'), 'via-handle');
    assert.strictEqual(handle.fd, -1);
  }

  fs.rmSync(dir, { recursive: true, force: true });
  clearTimeout(watchdog);
  console.log('fs stream fd ok');
})().catch(err => {
  console.error(err);
  process.exit(1);
});
