'use strict';
const assert = require('node:assert');

function* gen() {}
async function* agen() {}
const makers = [() => Promise.resolve(1), () => new Promise(() => {}), () => gen(), () => agen()];

class Ret { constructor(o) { return o; } }
class Tagged extends Ret { tag = 1; }

for (const make of makers) {
  for (const lock of [Object.freeze, Object.seal, Object.preventExtensions, o => (Reflect.preventExtensions(o), o)]) {
    const o = make();
    assert.strictEqual(Object.isExtensible(o), true);
    assert.strictEqual(Object.isFrozen(o), false);
    assert.strictEqual(Object.isSealed(o), false);
    assert.strictEqual(lock(o), o);
    assert.strictEqual(Object.isExtensible(o), false);
    assert.strictEqual(Reflect.isExtensible(o), false);
    assert.strictEqual(Object.isFrozen(o), true);
    assert.strictEqual(Object.isSealed(o), true);
    assert.throws(() => Object.defineProperty(o, 'x', { value: 1 }), TypeError);
    assert.strictEqual(Reflect.defineProperty(o, 'x', { value: 1 }), false);
    assert.throws(() => { o.y = 1; }, TypeError);
    assert.throws(() => new Tagged(o), TypeError);
    assert.deepStrictEqual(Reflect.ownKeys(o), []);
  }

  const kept = make();
  kept.a = 1;
  Object.seal(kept);
  kept.a = 2;
  assert.strictEqual(kept.a, 2);
  assert.throws(() => { delete kept.a; }, TypeError);
  assert.strictEqual(Object.isFrozen(kept), false);
  Object.freeze(kept);
  assert.throws(() => { kept.a = 3; }, TypeError);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(kept, 'a'), { value: 2, writable: false, enumerable: true, configurable: false });

  const open = make();
  new Tagged(open);
  assert.strictEqual(open.tag, 1);
}

const settled = Object.freeze(Promise.resolve(5));
settled.then(v => { assert.strictEqual(v, 5); console.log('ok'); });
