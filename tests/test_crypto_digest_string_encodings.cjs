const assert = require('node:assert');
const crypto = require('node:crypto');

const md5 = () => crypto.createHash('md5').update('x');
const raw = md5().digest();
assert.strictEqual(raw.toString('hex'), '9dd4e461268c8034f5c8564e155c67a6');

const latin1 = String.fromCharCode(...raw);
const ascii = String.fromCharCode(...[...raw].map((b) => b & 0x7f));
const utf16 = String.fromCharCode(...Array.from({ length: raw.length / 2 }, (_, i) => raw[i * 2] | (raw[i * 2 + 1] << 8)));
assert.strictEqual(md5().digest('latin1'), latin1);
assert.strictEqual(md5().digest('binary'), latin1);
assert.strictEqual(md5().digest('LATIN1'), latin1);
assert.strictEqual(md5().digest('ascii'), ascii);
assert.strictEqual(md5().digest('utf8'), '���a&��4��VN\u0015\\g�');
assert.strictEqual(md5().digest('utf16le'), utf16);
assert.strictEqual(md5().digest('ucs2'), utf16);
assert.strictEqual(md5().digest('hex'), '9dd4e461268c8034f5c8564e155c67a6');
assert.strictEqual(md5().digest('base64'), 'ndTkYSaMgDT1yFZOFVxnpg==');
assert.strictEqual(md5().digest('base64url'), 'ndTkYSaMgDT1yFZOFVxnpg');

assert.ok(Buffer.isBuffer(md5().digest('buffer')));
assert.ok(Buffer.isBuffer(md5().digest('bogus')));
assert.ok(Buffer.isBuffer(md5().digest()));

assert.ok(Buffer.from(md5().digest('latin1'), 'latin1').equals(raw));

const hmac = crypto.createHmac('sha256', 'k').update('x').digest('binary');
assert.strictEqual(typeof hmac, 'string');
assert.strictEqual(hmac.length, 32);

// latin1 input hashes one byte per UTF-16 code unit, so a latin1 digest round-trips
assert.strictEqual(
  crypto.createHash('md5').update('éÿ', 'latin1').digest('hex'),
  crypto.createHash('md5').update(Buffer.from([0xe9, 0xff])).digest('hex')
);
assert.strictEqual(
  crypto.createHash('md5').update(latin1, 'binary').digest('hex'),
  crypto.createHash('md5').update(raw).digest('hex')
);
assert.strictEqual(
  crypto.createHash('md5').update('-_8', 'base64url').digest('hex'),
  crypto.createHash('md5').update(Buffer.from([0xfb, 0xff])).digest('hex')
);

console.log('ok');
