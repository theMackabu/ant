const assert = require('node:assert');
const crypto = require('crypto');
const nodeCrypto = require('node:crypto');

const first = crypto.getHashes();
const second = crypto.getHashes();

assert(Array.isArray(first), 'getHashes should return an array');
assert(first.length > 0, 'getHashes should return BoringSSL digest names');
assert(first.includes('md5'), 'getHashes should include md5');
assert(first.includes('sha1'), 'getHashes should include sha1');
assert(first.includes('sha256'), 'getHashes should include sha256');
assert(first.includes('sha384'), 'getHashes should include sha384');
assert(first.includes('sha512'), 'getHashes should include sha512');
assert(first.every((name) => typeof name === 'string'), 'hash names should be strings');
assert.deepStrictEqual(first, [...first].sort(), 'hash names should be sorted');
assert.strictEqual(new Set(first).size, first.length, 'hash names should be unique');

for (const name of first) {
  assert.doesNotThrow(() => crypto.createHash(name), `${name} should be accepted by createHash`);
}

first.push('__mutated__');
assert(!second.includes('__mutated__'), 'getHashes should return a fresh array');
assert.deepStrictEqual(nodeCrypto.getHashes(), second, 'node:crypto should expose getHashes');

console.log('crypto:getHashes:ok');
