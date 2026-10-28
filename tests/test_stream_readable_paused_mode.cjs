'use strict';

const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const net = require('node:net');
const http = require('node:http');
const zlib = require('node:zlib');
const { spawn, spawnSync } = require('node:child_process');
const { Readable, PassThrough } = require('node:stream');

const watchdog = setTimeout(() => {
  console.error('paused-mode test timed out');
  process.exit(1);
}, 30000);

let finished = false;
process.on('exit', () => {
  if (!finished) {
    console.error('paused-mode test exited before completing');
    process.exitCode = 1;
  }
});

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-paused-'));
const bigFile = path.join(dir, 'big.bin');
const bigData = Buffer.alloc(200000);
for (let i = 0; i < bigData.length; i++) bigData[i] = (i * 31) & 255;
fs.writeFileSync(bigFile, bigData);
const textFile = path.join(dir, 'text.txt');
fs.writeFileSync(textFile, 'héllo wörld €\n'.repeat(4));
const emptyFile = path.join(dir, 'empty.txt');
fs.writeFileSync(emptyFile, '');

const tick = () => new Promise(resolve => setImmediate(resolve));

function drainReadable(stream, size) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let readables = 0;
    stream.on('readable', () => {
      readables++;
      let chunk;
      while ((chunk = size === undefined ? stream.read() : stream.read(size)) !== null) chunks.push(chunk);
    });
    stream.on('error', reject);
    stream.on('end', () => resolve({ chunks, readables }));
  });
}

async function syncReadLoop() {
  const source = ['a', 'bb', 'ccc'];
  const stream = new Readable({ read() { this.push(source.length ? source.shift() : null); } });
  const events = [];
  const out = [];
  stream.on('readable', () => {
    events.push(`readable:${stream.readableLength}`);
    let chunk;
    while ((chunk = stream.read()) !== null) out.push(chunk.toString());
  });
  stream.on('end', () => events.push('end'));
  await new Promise(resolve => stream.on('close', resolve));
  assert.deepStrictEqual(out, ['abb', 'ccc']);
  assert.deepStrictEqual(events, ['readable:1', 'readable:0', 'readable:0', 'readable:0', 'end']);
  assert.strictEqual(stream.readableFlowing, false);
  assert.strictEqual(stream.destroyed, true);
}

async function sizedReads() {
  const stream = new Readable({ read() {} });
  stream.push('abcdef');
  stream.push('ghij');
  assert.strictEqual(stream.read(3).toString(), 'abc');
  assert.strictEqual(stream.read(4).toString(), 'defg');
  assert.strictEqual(stream.readableLength, 3);
  assert.strictEqual(stream.read(10), null);
  assert.strictEqual(stream.read(2).toString(), 'hi');
  stream.push(null);
  assert.strictEqual(stream.read(10).toString(), 'j');
  assert.strictEqual(stream.read(), null);
  await tick();
  assert.strictEqual(stream.readableEnded, true);

  const waits = new Readable({ read() {} });
  waits.push('ab');
  assert.strictEqual(waits.read(5), null);
  waits.push('cdefg');
  assert.strictEqual(waits.read(5).toString(), 'abcde');
  waits.push(null);
  assert.strictEqual(waits.read(5).toString(), 'fg');
}

async function encodings() {
  const stream = new Readable({ read() {} });
  stream.setEncoding('utf8');
  const bytes = Buffer.from('héllo wörld €');
  stream.push(bytes.subarray(0, 2));
  stream.push(bytes.subarray(2, 7));
  stream.push(bytes.subarray(7));
  stream.push(null);
  assert.strictEqual(stream.readableLength, 13);
  assert.strictEqual(stream.read(3), 'hél');
  assert.strictEqual(stream.read(4), 'lo w');
  assert.strictEqual(stream.read(), 'örld €');
  assert.strictEqual(stream.readableEncoding, 'utf8');

  const late = new Readable({ read() {} });
  late.push(Buffer.from([0xe2, 0x82]));
  late.push(Buffer.from([0xac, 0x41]));
  assert.strictEqual(late.readableLength, 4);
  late.setEncoding('utf8');
  assert.strictEqual(late.readableLength, 2);
  assert.strictEqual(late.read(1), '€');
  assert.strictEqual(late.read(), 'A');

  const hex = new Readable({ read() {}, encoding: 'hex' });
  hex.push(Buffer.from([1, 2, 255]));
  assert.strictEqual(hex.readableLength, 6);
  assert.strictEqual(hex.read(4), '0102');
}

async function objectModeAndBackpressure() {
  const objects = new Readable({ objectMode: true, read() {} });
  assert.strictEqual(objects.push({ a: 1 }), true);
  objects.push(2);
  objects.push('x');
  assert.strictEqual(objects.readableLength, 3);
  assert.strictEqual(objects.readableHighWaterMark, 16);
  assert.strictEqual(objects.readableObjectMode, true);
  assert.deepStrictEqual(objects.read(), { a: 1 });
  assert.strictEqual(objects.read(5), 2);

  const small = new Readable({ objectMode: true, highWaterMark: 2, read() {} });
  assert.deepStrictEqual([small.push(1), small.push(2), small.push(3)], [true, false, false]);

  const bytes = new Readable({ highWaterMark: 4, read() {} });
  assert.deepStrictEqual([bytes.push('ab'), bytes.push('cd'), bytes.push('ef')], [true, false, false]);
  assert.strictEqual(bytes.readableLength, 6);

  const zero = new Readable({ highWaterMark: 0, read() {} });
  assert.strictEqual(zero.push('x'), false);
  assert.strictEqual(zero.readableHighWaterMark, 0);

  let reads = 0;
  const refill = new Readable({ highWaterMark: 10, read() { reads++; if (reads < 3) this.push('abc'); } });
  assert.strictEqual(refill.read(0), null);
  assert.strictEqual(reads, 1);
  await tick();
  assert.strictEqual(reads, 3);
  assert.strictEqual(refill.readableLength, 6);

  const sizes = [];
  const grow = new Readable({ highWaterMark: 100, read(n) { sizes.push(n); this.push(null); } });
  grow.read(500);
  assert.deepStrictEqual(sizes, [512]);
}

async function flowingStates() {
  const stream = new Readable({ read() {} });
  const states = [stream.readableFlowing, stream.isPaused()];
  stream.on('data', () => {});
  states.push(stream.readableFlowing, stream.isPaused());
  stream.pause();
  states.push(stream.readableFlowing, stream.isPaused());
  stream.resume();
  states.push(stream.readableFlowing, stream.isPaused());
  assert.deepStrictEqual(states, [null, false, true, false, false, true, true, false]);

  const both = new Readable({ read() {} });
  const data = [];
  let readables = 0;
  both.on('data', chunk => data.push(chunk.toString()));
  both.on('readable', () => { readables++; });
  both.push('x');
  both.push('y');
  await tick();
  assert.strictEqual(both.readableFlowing, false);
  assert.deepStrictEqual(data, []);
  assert.strictEqual(readables, 1);
  both.read();
  assert.deepStrictEqual(data, ['xy']);
  both.removeAllListeners('readable');
  await tick();
  assert.strictEqual(both.readableFlowing, true);
  both.push('z');
  await tick();
  assert.deepStrictEqual(data, ['xy', 'z']);
}

async function unshiftAndErrors() {
  const stream = new Readable({ read() {} });
  stream.push('world');
  stream.unshift(stream.read(2));
  stream.unshift(Buffer.from('hello '));
  assert.strictEqual(stream.readableLength, 11);
  assert.strictEqual(stream.read().toString(), 'hello world');

  const afterEof = new Readable({ read() {} });
  const codes = [];
  afterEof.on('error', err => codes.push(err.code));
  afterEof.push(null);
  assert.strictEqual(afterEof.push('x'), false);
  await tick();
  assert.deepStrictEqual(codes, ['ERR_STREAM_PUSH_AFTER_EOF']);

  const empty = new Readable({ read() { this.push(null); } });
  const events = [];
  empty.on('readable', () => { events.push('readable'); events.push(String(empty.read())); });
  empty.on('end', () => events.push('end'));
  await new Promise(resolve => empty.on('close', resolve));
  assert.deepStrictEqual(events, ['readable', 'null', 'end']);

  const destroyed = new Readable({ read() {} });
  destroyed.push('x');
  destroyed.destroy(new Error('boom'));
  const destroyEvents = [];
  destroyed.on('error', err => destroyEvents.push(`error:${err.message}`));
  destroyed.on('close', () => destroyEvents.push('close'));
  assert.strictEqual(destroyed.push('y'), false);
  await tick();
  assert.deepStrictEqual(destroyEvents, ['error:boom', 'close']);
  assert.strictEqual(destroyed.errored.message, 'boom');
  assert.strictEqual(destroyed.readableAborted, true);
}

async function readableFrom() {
  const out = [];
  for await (const chunk of Readable.from(['a', 'b', Buffer.from('c')])) out.push(chunk);
  assert.deepStrictEqual(out.slice(0, 2), ['a', 'b']);
  assert.ok(Buffer.isBuffer(out[2]));

  const fromString = await drainReadable(Readable.from('hello'));
  assert.deepStrictEqual(fromString.chunks, ['hello']);

  async function* gen() { yield 'x'; yield 'y'; }
  const fromGen = await drainReadable(Readable.from(gen()));
  assert.deepStrictEqual(fromGen.chunks, ['x', 'y']);
  assert.throws(() => Readable.from(5), { code: 'ERR_INVALID_ARG_TYPE' });

  const pt = new PassThrough();
  const ptOut = drainReadable(pt);
  pt.write('a');
  pt.end('b');
  assert.strictEqual(Buffer.concat((await ptOut).chunks).toString(), 'ab');
}

async function fsStreams() {
  const whole = await drainReadable(fs.createReadStream(bigFile));
  assert.ok(Buffer.concat(whole.chunks).equals(bigData));

  const sized = await drainReadable(fs.createReadStream(bigFile, { highWaterMark: 1000 }), 777);
  assert.ok(Buffer.concat(sized.chunks).equals(bigData));
  assert.ok(sized.chunks.slice(0, -1).every(chunk => chunk.length === 777));

  const text = await drainReadable(fs.createReadStream(textFile, { encoding: 'utf8', highWaterMark: 5 }), 4);
  assert.strictEqual(text.chunks.join(''), 'héllo wörld €\n'.repeat(4));
  assert.ok(text.chunks.slice(0, -1).every(chunk => typeof chunk === 'string' && chunk.length === 4));

  const empty = await drainReadable(fs.createReadStream(emptyFile));
  assert.deepStrictEqual(empty.chunks, []);
  assert.strictEqual(empty.readables, 1);

  const stream = fs.createReadStream(textFile);
  assert.strictEqual(stream.readableFlowing, null);
  assert.strictEqual(stream.readableHighWaterMark, 65536);
  let total = 0;
  for await (const chunk of fs.createReadStream(bigFile)) total += chunk.length;
  assert.strictEqual(total, bigData.length);
  stream.destroy();
}

async function childAndStdin() {
  const child = spawn(process.execPath, ['-e', 'process.stdout.write("x".repeat(100000)); setTimeout(() => process.stdout.write("tail"), 10)']);
  const { chunks } = await drainReadable(child.stdout);
  const text = Buffer.concat(chunks).toString();
  assert.strictEqual(text.length, 100004);
  assert.ok(text.endsWith('tail'));

  const encoded = spawn(process.execPath, ['-e', 'process.stdout.write("é".repeat(10))']);
  encoded.stdout.setEncoding('utf8');
  assert.deepStrictEqual((await drainReadable(encoded.stdout, 3)).chunks, ['ééé', 'ééé', 'ééé', 'é']);

  const script = `
    const chunks = [];
    process.stdin.setEncoding('utf8');
    process.stdin.on('readable', () => { let c; while ((c = process.stdin.read(4)) !== null) chunks.push(c); });
    process.stdin.on('end', () => console.log(JSON.stringify(chunks)));
  `;
  const piped = spawnSync(process.execPath, ['-e', script], { input: 'abcdefghij', encoding: 'utf8' });
  assert.strictEqual(piped.stdout.trim(), '["abcd","efgh","ij"]', piped.stderr);

  const fd = fs.openSync(bigFile, 'r');
  const fromFile = spawnSync(process.execPath, ['-e', `
    let n = 0;
    process.stdin.on('readable', () => { let c; while ((c = process.stdin.read()) !== null) n += c.length; });
    process.stdin.on('end', () => console.log(n));
  `], { stdio: [fd, 'pipe', 'pipe'], encoding: 'utf8' });
  fs.closeSync(fd);
  assert.strictEqual(fromFile.stdout.trim(), String(bigData.length), fromFile.stderr);
}

async function sockets() {
  const server = net.createServer(socket => socket.end('0123456789'));
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const socket = net.connect(server.address().port, '127.0.0.1');
  socket.setEncoding('utf8');
  assert.deepStrictEqual((await drainReadable(socket, 3)).chunks, ['012', '345', '678', '9']);
  await new Promise(resolve => server.close(resolve));

  const httpServer = http.createServer((req, res) => {
    drainReadable(req).then(({ chunks }) => res.end(`got ${Buffer.concat(chunks).length}`));
  });
  await new Promise(resolve => httpServer.listen(0, '127.0.0.1', resolve));
  const body = await new Promise((resolve, reject) => {
    const req = http.request({ port: httpServer.address().port, host: '127.0.0.1', method: 'POST' }, res => {
      drainReadable(res).then(({ chunks }) => resolve(Buffer.concat(chunks).toString()), reject);
    });
    req.on('error', reject);
    req.end('x'.repeat(5000));
  });
  assert.strictEqual(body, 'got 5000');
  await new Promise(resolve => httpServer.close(resolve));
}

async function zlibStreams() {
  const gzip = zlib.createGzip();
  const gunzip = zlib.createGunzip();
  gzip.pipe(gunzip);
  const pending = drainReadable(gunzip);
  gzip.end(bigData);
  assert.ok(Buffer.concat((await pending).chunks).equals(bigData));
}

(async () => {
  await syncReadLoop();
  await sizedReads();
  await encodings();
  await objectModeAndBackpressure();
  await flowingStates();
  await unshiftAndErrors();
  await readableFrom();
  await fsStreams();
  await childAndStdin();
  await sockets();
  await zlibStreams();
  fs.rmSync(dir, { recursive: true, force: true });
  finished = true;
  clearTimeout(watchdog);
  console.log('stream readable paused mode ok');
})().catch(err => {
  console.error(err);
  process.exit(1);
});
