const assert = require('node:assert');

for (let i = 0; i < 2000; i++) {
  const plain = new Proxy({}, {});
  assert.strictEqual(Reflect.isExtensible(plain), true);
  Object.preventExtensions(plain);
  assert.strictEqual(Reflect.isExtensible(plain), false);
  assert.strictEqual(Object.isExtensible(plain), false);

  const log = [];
  const trapped = new Proxy({}, { isExtensible(target) { log.push('isExtensible'); return Reflect.isExtensible(target); } });
  assert.strictEqual(Reflect.isExtensible(trapped), true);
  assert.deepStrictEqual(log, ['isExtensible']);

  const lying = new Proxy({}, { isExtensible() { return false; } });
  assert.throws(() => Reflect.isExtensible(lying), TypeError);
}

console.log('ok');
