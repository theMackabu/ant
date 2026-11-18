const assert = require('node:assert');

{
  const hasTrap = Object.create(new Proxy({}, { has: (target, key) => key === 'virtual' }));
  assert.strictEqual('virtual' in hasTrap, true);
  assert.strictEqual('other' in hasTrap, false);
  const inner = Object.create(new Proxy({}, { has: (target, key) => key === 'deep' }));
  const outer = new Proxy(inner, {});
  assert.strictEqual('deep' in outer, true);
  assert.strictEqual(Reflect.has(outer, 'deep'), true);
  assert.strictEqual('missing' in outer, false);
  assert.strictEqual(Symbol.iterator in new Proxy([], {}), true);
  assert.strictEqual(Symbol.iterator in new Proxy({}, {}), false);
  assert.strictEqual(1 in new Proxy([5, 6], {}), true);
  assert.strictEqual(2 in new Proxy([5, 6], {}), false);
  assert.strictEqual('length' in new Proxy([5, 6], {}), true);
}

console.log('ok');
