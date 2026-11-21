const assert = require('node:assert');

class Plain {}
assert.throws(() => ({}) instanceof Math.max, TypeError);
assert.throws(() => [] instanceof JSON.parse, TypeError);
assert.strictEqual(1 instanceof Math.max, false);
assert.strictEqual(new Plain() instanceof new Proxy(new Proxy(Plain, {}), {}), true);
assert.strictEqual(new Map() instanceof new Proxy(new Proxy(Map, {}), {}), true);
assert.throws(() => ({}) instanceof new Proxy(new Proxy({}, {}), {}), TypeError);

console.log('ok');
