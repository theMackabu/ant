const assert = require('assert');

assert.strictEqual(BigInt('9007199254740993'), 9007199254740993n);
assert.strictEqual(BigInt('-9007199254740993'), -9007199254740993n);
assert.notStrictEqual(9007199254740993n, 9007199254740994n);
assert.strictEqual(NaN, NaN);
assert.notStrictEqual(0, -0);

console.log('assert BigInt tests passed');
