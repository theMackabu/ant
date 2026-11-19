const assert = require('node:assert');

{
  function join(a, b) { return [this === null ? 'null' : typeof this, a, b].join(','); }
  const plain = new Proxy(join, {});
  const trapped = new Proxy(join, { apply(target, self, list) { return 'trap:' + list.join('|'); } });
  assert.strictEqual(plain.apply(null, [1, 2]), join.apply(null, [1, 2]));
  assert.strictEqual(plain.call(null, 1, 2), join.call(null, 1, 2));
  assert.strictEqual(plain.bind(null, 1)(2), join.bind(null, 1)(2));
  assert.strictEqual(Function.prototype.apply.call(plain, 'self', [1, 2]), join.apply('self', [1, 2]));
  assert.strictEqual(Function.prototype.call.call(plain, 'self', 1, 2), join.call('self', 1, 2));
  assert.strictEqual(Function.prototype.bind.call(plain, 'self', 1)(2), join.call('self', 1, 2));
  assert.strictEqual(Reflect.apply(plain, 'self', [1, 2]), join.call('self', 1, 2));
  assert.strictEqual(plain.apply(null), join.apply(null));
  assert.strictEqual(trapped.apply(null, [1, 2]), 'trap:1|2');
  assert.strictEqual(trapped.call(null, 1, 2), 'trap:1|2');
  assert.strictEqual(trapped.bind(null, 1)(2), 'trap:1|2');
  assert.strictEqual(Reflect.apply(trapped, null, [3]), 'trap:3');
  assert.strictEqual(typeof Function.prototype.toString.call(plain), 'string');
  assert.throws(() => Function.prototype.call.call(new Proxy({}, {})), TypeError);
  assert.throws(() => Function.prototype.apply.call(new Proxy({}, {}), null, []), TypeError);
  assert.throws(() => Reflect.apply(new Proxy({}, {}), null, []), TypeError);
  class Box { constructor(v) { this.v = v; } }
  assert.strictEqual(new (new Proxy(Box, {}).bind(null, 4))().v, 4);
}

console.log('ok');
