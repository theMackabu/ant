const assert = require('node:assert');

assert.strictEqual(JSON.parse('1e400'), Infinity);
assert.strictEqual(JSON.parse('-1e400'), -Infinity);
assert.strictEqual(JSON.parse('1E+309'), Infinity);
assert.strictEqual(JSON.parse('1.7976931348623159e308'), Infinity);
assert.strictEqual(JSON.parse('1.7976931348623157e308'), Number.MAX_VALUE);

assert.ok(Object.is(JSON.parse('1e-400'), 0));
assert.ok(Object.is(JSON.parse('-1e-400'), -0));

const arr = JSON.parse('[1e400, -1e999, 1e-400, -1e-400]');
assert.strictEqual(arr.length, 4);
assert.strictEqual(arr[0], Infinity);
assert.strictEqual(arr[1], -Infinity);
assert.ok(Object.is(arr[2], 0));
assert.ok(Object.is(arr[3], -0));

assert.deepStrictEqual(JSON.parse('{"a":1e400,"b":{"c":-1e400}}'), { a: Infinity, b: { c: -Infinity } });

const big = '1' + '0'.repeat(400);
assert.strictEqual(JSON.parse(big), Infinity);
assert.strictEqual(JSON.parse('-' + big), -Infinity);
assert.strictEqual(JSON.parse(big + '.5'), Infinity);

assert.strictEqual(JSON.parse('18446744073709551616'), 18446744073709551616);
assert.strictEqual(JSON.parse('-9223372036854775809'), -9223372036854775809);
assert.strictEqual(JSON.parse('123456789012345678901234567890'), 123456789012345678901234567890);
assert.deepStrictEqual(JSON.parse('[18446744073709551616,-18446744073709551617]'), [18446744073709551616, -18446744073709551617]);

assert.deepStrictEqual(JSON.parse('["\\ud800", 1e400]'), ['\ud800', Infinity]);
assert.deepStrictEqual(JSON.parse('[1e400, "\\ud800"]'), [Infinity, '\ud800']);

assert.strictEqual(JSON.parse('[1e400]', (k, v) => (k === '0' ? typeof v : v))[0], 'number');

for (const bad of ['1e', '-', '1e400x', '01e400', '1e+', '.1e400', 'Infinity', '-Infinity']) {
  assert.throws(() => JSON.parse(bad), SyntaxError, bad);
}

console.log('ok');
