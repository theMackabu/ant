const assert = require('node:assert');

{
  const symbol = Symbol('key');
  const selfCycle = () => {
    const object = {};
    Object.setPrototypeOf(object, new Proxy(object, {}));
    return object;
  };
  const pairCycle = () => {
    const a = {};
    const b = {};
    Object.setPrototypeOf(a, new Proxy(b, {}));
    Object.setPrototypeOf(b, new Proxy(a, {}));
    return a;
  };

  for (const make of [selfCycle, pairCycle]) {
    assert.throws(() => 'x' in make(), RangeError);
    assert.throws(() => Reflect.has(make(), 'x'), RangeError);
    assert.throws(() => symbol in make(), RangeError);
    assert.throws(() => 0 in make(), RangeError);
    assert.throws(() => make().x, RangeError);
    assert.throws(() => make()[symbol], RangeError);
    assert.throws(() => make()[0], RangeError);
    assert.throws(() => { make().x = 1; }, RangeError);
    assert.throws(() => { make()[symbol] = 1; }, RangeError);
    assert.throws(() => Reflect.set(make(), 'x', 1), RangeError);
    assert.throws(() => make() instanceof Object, RangeError);
    assert.throws(() => Object.prototype.isPrototypeOf(make()), RangeError);
    assert.throws(() => String(make()), RangeError);
    assert.throws(() => JSON.stringify(make()), RangeError);
    assert.throws(() => make().method(), RangeError);

    assert.strictEqual(delete make().x, true);
    assert.deepStrictEqual(Object.keys(make()), []);
    assert.deepStrictEqual(Reflect.ownKeys(make()), []);
    assert.strictEqual(Object.getOwnPropertyDescriptor(make(), 'x'), undefined);
    assert.strictEqual(Object.defineProperty(make(), 'x', { value: 1 }).x, 1);
    assert.strictEqual(typeof Object.getPrototypeOf(make()), 'object');
    assert.strictEqual(Object.hasOwn(make(), 'x'), false);
    assert.deepStrictEqual({ ...make() }, {});
    assert.strictEqual(Object.isFrozen(Object.freeze(make())), true);
  }

  const own = selfCycle();
  Object.defineProperty(own, 'x', { value: undefined });
  assert.strictEqual('x' in own, true);
  assert.strictEqual(own.x, undefined);
}

console.log('ok');
