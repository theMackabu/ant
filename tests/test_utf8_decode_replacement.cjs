'use strict';

const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const net = require('node:net');
const path = require('node:path');
const childProcess = require('node:child_process');
const { StringDecoder } = require('node:string_decoder');

function units(value) {
  const out = [];
  for (let i = 0; i < value.length; i++) out.push(value.charCodeAt(i).toString(16));
  return out.join(' ');
}

function wellFormed(value) {
  for (let i = 0; i < value.length; i++) {
    const c = value.charCodeAt(i);
    if (c >= 0xd800 && c <= 0xdbff) {
      const n = value.charCodeAt(i + 1);
      if (!(n >= 0xdc00 && n <= 0xdfff)) return false;
      i++;
    } else if (c >= 0xdc00 && c <= 0xdfff) return false;
  }
  return true;
}

function check(label, actual, expected) {
  assert.strictEqual(typeof actual, 'string', label);
  assert.strictEqual(units(actual), units(expected), label);
  assert.ok(wellFormed(actual), label + ' well formed');
  assert.strictEqual(Buffer.from(actual).toString(), actual, label + ' round trip');
}

const cases = [
  [[0x61, 0xff, 0x62], 'a�b'],
  [[0xc3], '�'],
  [[0xc3, 0x28], '�('],
  [[0xc0, 0x80], '��'],
  [[0xe0, 0x80, 0x80], '���'],
  [[0xed, 0xa0, 0x80], '���'],
  [[0xed, 0xa0, 0xbd, 0xed, 0xb8, 0x80], '�'.repeat(6)],
  [[0xf0, 0x9f, 0x98], '�'],
  [[0xf0, 0x9f, 0x98, 0x41], '�A'],
  [[0xf4, 0x90, 0x80, 0x80], '�'.repeat(4)],
  [[0xf5, 0x80], '��'],
  [[0x80, 0xbf], '��'],
  [[0xe2, 0x82, 0xac, 0xe2, 0x82], '€�'],
  [[0xf0, 0x9f, 0x98, 0x80, 0xff], '😀�'],
  [[0xef, 0xbb, 0xbf, 0x61, 0xfe], '\ufeffa�'],
  [[0x61, 0xff, 0xc3, 0x62, 0xed, 0xa0, 0x80, 0xf0, 0x9f, 0x98, 0x63, 0xe2, 0x82],
    'a��b����c�'],
];

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-utf8-decode-'));
const file = path.join(dir, 'data.bin');

for (const [bytes, expected] of cases) {
  const buf = Buffer.from(bytes);
  const label = buf.toString('hex');
  const noBom = expected.charCodeAt(0) === 0xfeff ? expected.slice(1) : expected;

  check(label + ' toString', buf.toString(), expected);
  check(label + ' toString utf8', buf.toString('utf8'), expected);
  check(label + ' toString utf-8', buf.toString('utf-8', 0, buf.length), expected);
  check(label + ' slice', buf.toString('utf8', 1), Buffer.from(bytes.slice(1)).toString());
  check(label + ' TextDecoder', new TextDecoder().decode(buf), noBom);
  check(label + ' TextDecoder ignoreBOM', new TextDecoder('utf-8', { ignoreBOM: true }).decode(buf), expected);

  const sd = new StringDecoder('utf8');
  let chunked = '';
  for (const byte of bytes) chunked += sd.write(Buffer.from([byte]));
  chunked += sd.end();
  check(label + ' StringDecoder', chunked, expected);

  const td = new TextDecoder();
  let streamed = '';
  for (const byte of bytes) streamed += td.decode(new Uint8Array([byte]), { stream: true });
  streamed += td.decode();
  check(label + ' TextDecoder stream', streamed, noBom);

  fs.writeFileSync(file, buf);
  check(label + ' readFileSync', fs.readFileSync(file, 'utf8'), expected);
  check(label + ' readFileSync options', fs.readFileSync(file, { encoding: 'utf-8' }), expected);
  assert.strictEqual(fs.readFileSync(file, 'latin1'), buf.toString('latin1'), label + ' latin1');
  assert.strictEqual(fs.readFileSync(file, 'ascii'), buf.toString('ascii'), label + ' ascii');
}

assert.throws(
  () => new TextDecoder('utf-8', { fatal: true }).decode(Buffer.from([0x61, 0xff])),
  (err) => err instanceof TypeError && err.code === 'ERR_ENCODING_INVALID_ENCODED_DATA'
);
assert.throws(
  () => new TextDecoder('utf-8', { fatal: true }).decode(Buffer.from([0xed, 0xa0, 0x80])),
  TypeError
);
assert.strictEqual(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.from('hé☃😀')), 'hé☃😀');

const valid = 'ascii é 中文 😀 '.repeat(40);
const validBuf = Buffer.from(valid);
check('valid toString', validBuf.toString(), valid);
check('valid TextDecoder', new TextDecoder().decode(validBuf), valid);
assert.strictEqual(validBuf.toString().length, valid.length);

check('JSON.parse buffer', JSON.parse(Buffer.from([0x22, 0x61, 0xff, 0x62, 0x22])), 'a�b');
assert.deepStrictEqual(JSON.parse(Buffer.from('{"a":[1,2]}')), { a: [1, 2] });

for (const bad of ['%C0%80', '%ED%A0%80', '%F4%90%80%80', '%F5%80%80%80', '%E0%80%80', '%80', '%C3']) {
  assert.throws(() => decodeURIComponent(bad), URIError, bad);
  assert.throws(() => decodeURI(bad), URIError, bad);
}
assert.strictEqual(decodeURIComponent('%F0%9F%98%80%C3%A9'), '\ud83d\ude00\u00e9');
check('URLSearchParams', new URLSearchParams('a=%FF%C3b').get('a'), '\ufffd\ufffdb');
check('querystring.parse', require('node:querystring').parse('a=%FF%C3b').a, '\ufffd\ufffdb');
check('querystring.unescape', require('node:querystring').unescape('%E9%zz%C3%A9'), '\ufffd%zz\u00e9');

const mod = path.join(dir, 'invalid_source.js');
fs.writeFileSync(mod, Buffer.concat([
  Buffer.from('module.exports = "x'),
  Buffer.from([0xff, 0xc3, 0x28]),
  Buffer.from('y";'),
]));
check('require invalid source', require(mod), 'x��(y');

const sub = path.join(dir, 'names');
fs.mkdirSync(sub);
fs.writeFileSync(path.join(sub, 'café.txt'), '');
assert.deepStrictEqual(fs.readdirSync(sub), ['café.txt']);
assert.ok(wellFormed(fs.readdirSync(sub)[0]));

const printer = path.join(dir, 'printer.js');
fs.writeFileSync(printer, 'process.stdout.write(Buffer.from([0x61, 0xff, 0xe2, 0x82, 0xac, 0xc3]));');
check('execFileSync utf8', childProcess.execFileSync(process.execPath, [printer], { encoding: 'utf8' }), 'a�€�');
assert.strictEqual(
  childProcess.execFileSync(process.execPath, [printer], { encoding: 'latin1' }),
  Buffer.from([0x61, 0xff, 0xe2, 0x82, 0xac, 0xc3]).toString('latin1')
);
const spawned = childProcess.spawnSync(process.execPath, [printer], { encoding: 'utf8' });
check('spawnSync stdout', spawned.stdout, 'a�€�');

const stdinEcho = path.join(dir, 'stdin_echo.js');
fs.writeFileSync(stdinEcho, [
  'let out = "";',
  'process.stdin.setEncoding("utf8");',
  'process.stdin.on("data", (chunk) => { out += chunk; });',
  'process.stdin.on("end", () => {',
  '  const codes = [];',
  '  for (let i = 0; i < out.length; i++) codes.push(out.charCodeAt(i).toString(16));',
  '  process.stdout.write(codes.join(" "));',
  '});',
].join('\n'));
const stdinResult = childProcess.spawnSync(process.execPath, [stdinEcho], {
  input: Buffer.from([0x61, 0xff, 0xf0, 0x9f, 0x98, 0x80, 0xe2, 0x82]),
  encoding: 'utf8',
});
assert.strictEqual(stdinResult.stdout, '61 fffd d83d de00 fffd');

async function main() {
  check('readFile callback', await new Promise((resolve, reject) => {
    fs.writeFileSync(file, Buffer.from([0x61, 0xff, 0x62]));
    fs.readFile(file, 'utf8', (err, data) => (err ? reject(err) : resolve(data)));
  }), 'a�b');
  check('promises.readFile', await fs.promises.readFile(file, 'utf8'), 'a�b');
  check('promises.readFile options', await fs.promises.readFile(file, { encoding: 'utf8' }), 'a�b');

  check('Response.text', await new Response(new Uint8Array([0xef, 0xbb, 0xbf, 0x61, 0xff])).text(), 'a�');
  check('Blob.text', await new Blob([new Uint8Array([0xef, 0xbb, 0xbf, 0x61, 0xff])]).text(), 'a�');

  const execOut = await new Promise((resolve, reject) => {
    childProcess.execFile(process.execPath, [printer], (err, stdout) => (err ? reject(err) : resolve(stdout)));
  });
  check('execFile stdout', execOut, 'a�€�');

  const received = await new Promise((resolve, reject) => {
    const server = net.createServer((socket) => {
      socket.write(Buffer.from([0x61, 0xe2]));
      setTimeout(() => socket.end(Buffer.from([0x82, 0xac, 0xff, 0x62])), 20);
    });
    server.listen(0, '127.0.0.1', () => {
      const client = net.connect(server.address().port, '127.0.0.1');
      let text = '';
      client.setEncoding('utf8');
      client.on('data', (chunk) => { text += chunk; });
      client.on('end', () => { server.close(); resolve(text); });
      client.on('error', reject);
    });
  });
  check('net setEncoding', received, 'a€�b');

  fs.rmSync(dir, { recursive: true, force: true });
  console.log('ok');
}

main().catch((err) => {
  fs.rmSync(dir, { recursive: true, force: true });
  console.error(err);
  process.exit(1);
});
