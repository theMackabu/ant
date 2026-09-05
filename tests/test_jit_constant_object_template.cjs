const assert = require('node:assert');
function make() { return { root: '', dir: '', base: '', ext: '', name: '' }; }
function values() { return { a: 1, b: true, c: null, d: 'é😀', e: 12345678901234567890n }; }
for (let i = 0; i < 50000; i++) {
  const a = make(), b = make();
  assert.notStrictEqual(a, b);
  a.root = 'changed';
  delete a.name;
  a.extra = i;
  assert.deepStrictEqual(b, { root: '', dir: '', base: '', ext: '', name: '' });
  if (i % 100 === 0) {
    Object.freeze(a);
    assert.strictEqual(Object.isFrozen(make()), false);
    assert.deepStrictEqual(values(), { a: 1, b: true, c: null, d: 'é😀', e: 12345678901234567890n });
  }
}
assert.deepStrictEqual(Object.keys(make()), ['root', 'dir', 'base', 'ext', 'name']);
assert.deepStrictEqual(Object.getOwnPropertyDescriptor(make(), 'name'), {
  value: '', writable: true, enumerable: true, configurable: true
});
let effects = 0;
function dynamic() { return { a: ++effects, [String(effects)]: { nested: effects }, get b() { return effects; } }; }
for (let i = 1; i <= 1000; i++) {
  const result = dynamic();
  assert.strictEqual(result.a, i);
  assert.strictEqual(result.b, i);
  assert.strictEqual(result[String(i)].nested, i);
}
function nested() { return { child: {} }; }
assert.notStrictEqual(nested().child, nested().child);
function special() { return { __proto__: null, a: 1 }; }
assert.strictEqual(Object.getPrototypeOf(special()), null);
const original = Object.getOwnPropertyDescriptor(Object.prototype, 'root');
let setters = 0;
try {
  Object.defineProperty(Object.prototype, 'root', { configurable: true, set() { setters++; } });
  for (let i = 0; i < 1000; i++) assert.strictEqual(make().root, '');
  assert.strictEqual(setters, 0);
} finally {
  if (original) Object.defineProperty(Object.prototype, 'root', original);
  else delete Object.prototype.root;
}
console.log('constant literal identity, mutation, GC churn, descriptors and fallback effects ok');
