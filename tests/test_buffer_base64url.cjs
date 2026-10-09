const assert = require('node:assert');

const bytes = Buffer.from([0xfb, 0xff, 0xbf, 0x00, 0x41]);

assert.strictEqual(bytes.toString('base64url'), '-_-_AEE');
assert.strictEqual(bytes.toString('BASE64URL'), '-_-_AEE');
assert.strictEqual(bytes.toString('base64'), '+/+/AEE=');
assert.strictEqual(bytes.toString('base64url', 1, 4), '_78A');

assert.strictEqual(Buffer.isEncoding('base64url'), true);
assert.strictEqual(Buffer.isEncoding('Base64Url'), true);

// [input, decoded hex, byteLength] -- base64 and base64url decode identically in Node
const cases = [
  ['QUJD', '414243', 3],
  ['QUI', '4142', 2],
  ['QUI=', '4142', 2],
  ['QQ', '41', 1],
  ['Q', '', 0],
  ['QQ==QQ==', '41', 4],
  ['Q U\nJ D', '414243', 5],
  ['QU!JD', '414243', 3],
  ['-_-_', 'fbffbf', 3],
  ['+/+/', 'fbffbf', 3],
  ['QUJD====', '414243', 4],
  ['QUJ=D', '4142', 3],
  ['', '', 0],
  ['===', '', 0],
  ['Q=QQ', '', 3],
  ['-_-_AEE', 'fbffbf0041', 5],
];

for (const [input, hex, byteLength] of cases) {
  for (const encoding of ['base64', 'base64url']) {
    const label = `${JSON.stringify(input)} ${encoding}`;
    assert.strictEqual(Buffer.from(input, encoding).toString('hex'), hex, label);
    assert.strictEqual(Buffer.byteLength(input, encoding), byteLength, label);
  }
}

for (let n = 0; n < 70; n++) {
  const buf = Buffer.alloc(n);
  for (let i = 0; i < n; i++) buf[i] = (i * 97 + n * 31) & 255;
  const encoded = buf.toString('base64url');
  assert.doesNotMatch(encoded, /[+/=]/);
  assert.ok(Buffer.from(encoded, 'base64url').equals(buf), `base64url round trip ${n}`);
  assert.ok(Buffer.from(buf.toString('base64'), 'base64url').equals(buf), `base64 -> base64url ${n}`);
}

const target = Buffer.alloc(8);
assert.strictEqual(target.write('-_-_AEE', 'base64url'), 5);
assert.strictEqual(target.toString('hex'), 'fbffbf0041000000');

const offsetTarget = Buffer.alloc(8);
assert.strictEqual(offsetTarget.write('+/+/', 1, 'base64'), 3);
assert.strictEqual(offsetTarget.toString('hex'), '00fbffbf00000000');

assert.strictEqual(bytes.indexOf('AEE', 'base64url'), 3);
assert.strictEqual(bytes.indexOf('_7-_', 'base64url'), -1);

console.log('ok');
