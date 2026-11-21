const assert = require('node:assert');

const message = (run) => {
  try {
    run();
  } catch (error) {
    return `${error.constructor.name}: ${error.message}`;
  }
  return 'no throw';
};

{
  const proto = { marker: 1 };
  const promise = Promise.resolve(1);
  assert.strictEqual(Object.setPrototypeOf(promise, proto), promise);
  assert.strictEqual(Object.getPrototypeOf(promise), proto);
  assert.strictEqual(promise.marker, 1);

  const generator = (function* () {})();
  assert.strictEqual(Object.setPrototypeOf(generator, proto), generator);
  assert.strictEqual(Object.getPrototypeOf(generator), proto);
  assert.strictEqual(generator.__proto__, proto);

  const viaAccessor = Promise.resolve(2);
  viaAccessor.__proto__ = proto;
  assert.strictEqual(Object.getPrototypeOf(viaAccessor), proto);
  assert.strictEqual(Reflect.setPrototypeOf(Promise.resolve(3), null), true);

  const symbol = Symbol('s');
  for (const primitive of [1, 's', true, 10n, symbol]) {
    assert.strictEqual(Object.setPrototypeOf(primitive, {}), primitive);
    assert.strictEqual(Object.setPrototypeOf(primitive, null), primitive);
  }
  assert.throws(() => Object.setPrototypeOf(undefined, {}), TypeError);
  assert.throws(() => Object.setPrototypeOf(null, {}), TypeError);
  assert.throws(() => Object.setPrototypeOf({}, 1), TypeError);
  assert.throws(() => Object.setPrototypeOf({}, undefined), TypeError);
  assert.throws(() => Object.setPrototypeOf(1, 1), TypeError);
  assert.throws(() => Object.create(1), TypeError);
  assert.strictEqual(Object.getPrototypeOf(Object.create(promise)), promise);

  for (const make of [() => Promise.resolve(), () => (function* () {})()]) {
    assert.deepStrictEqual([Object.isFrozen(make()), Object.isSealed(make()), Object.isExtensible(make())], [false, false, true]);
    const frozen = Object.freeze(make());
    assert.deepStrictEqual([Object.isFrozen(frozen), Object.isSealed(frozen), Object.isExtensible(frozen)], [true, true, false]);
  }
}

assert.match(message(() => Object.setPrototypeOf({}, 1)), /^TypeError: Object prototype may only be an Object or null/);
assert.match(message(() => Object.create(1)), /^TypeError: Object prototype may only be an Object or null/);

console.log('ok');
