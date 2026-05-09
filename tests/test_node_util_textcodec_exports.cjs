const assert = require('node:assert');
const util = require('node:util');

assert.strictEqual(typeof util.TextDecoder, 'function');
assert.strictEqual(typeof util.TextEncoder, 'function');

const encoded = new util.TextEncoder().encode('ok');
assert.strictEqual(new util.TextDecoder().decode(encoded), 'ok');

console.log('node:util-textcodec-exports:ok');
