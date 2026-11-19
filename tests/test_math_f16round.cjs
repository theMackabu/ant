const assert = require('node:assert');

assert.deepStrictEqual(
  [1.337, 65504, 65520, -65520, 1e-8, 2.9802322387695312e-8, 2049, 2051, 0.1].map((x) => Math.f16round(x)),
  [1.3369140625, 65504, Infinity, -Infinity, 0, 0, 2048, 2052, 0.0999755859375]);
assert.ok(Object.is(Math.f16round(-0), -0));
assert.ok(Number.isNaN(Math.f16round()));
assert.strictEqual(Math.f16round.length, 1);

console.log('ok');
