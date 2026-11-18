const assert = require('node:assert');

assert.strictEqual(JSON.stringify(new Proxy(function () {}, {})), undefined);
assert.strictEqual(JSON.stringify({ a: new Proxy(function () {}, {}), b: 1 }), '{"b":1}');
assert.strictEqual(JSON.stringify([new Proxy(() => 1, {})]), '[null]');
assert.strictEqual(JSON.stringify(new Proxy({ a: 1 }, {})), '{"a":1}');

console.log('ok');
