const assert = require('node:assert');

function trapArguments() {
  const handler = {
    get() { return this; },
    has() { return this === handler; },
    apply() { return this; },
    construct() { return { handler: this }; },
  };
  const proxy = new Proxy(function () {}, handler);
  assert.strictEqual(proxy.anything, handler);
  assert.strictEqual('key' in proxy, true);
  assert.strictEqual(proxy(), handler);
  assert.strictEqual(new proxy().handler, handler);

  assert.strictEqual(new Proxy({ a: 1 }, new Map()).a, undefined);
  assert.throws(() => new Proxy(), TypeError);
  assert.throws(() => new Proxy({}), TypeError);
}

trapArguments();
console.log('ok');
