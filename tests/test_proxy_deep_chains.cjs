const assert = require('node:assert');

{
  let nested = { deep: 1 };
  for (let i = 0; i < 200; i++) nested = new Proxy(nested, {});
  assert.strictEqual('deep' in nested, true);
  assert.strictEqual(nested.deep, 1);
  assert.deepStrictEqual(Reflect.ownKeys(nested), ['deep']);
  assert.strictEqual(Object.getOwnPropertyDescriptor(nested, 'deep').value, 1);
  assert.strictEqual(Object.isExtensible(nested), true);
  assert.strictEqual(Object.defineProperty(nested, 'added', { value: 2, configurable: true }).added, 2);
  assert.strictEqual(delete nested.added, true);
  assert.strictEqual(typeof nested, 'object');

  let callable = function () { return 'called'; };
  class Target { constructor() { this.made = true; } }
  let constructible = Target;
  for (let i = 0; i < 200; i++) {
    callable = new Proxy(callable, {});
    constructible = new Proxy(constructible, {});
  }
  assert.strictEqual(typeof callable, 'function');
  assert.strictEqual(callable(), 'called');
  assert.strictEqual(new constructible().made, true);
  assert.throws(() => new (new Proxy(() => {}, {}))(), TypeError);
  assert.throws(() => nested(), TypeError);

  const runOrOverflow = (run) => {
    try {
      run();
    } catch (error) {
      assert.ok(error instanceof RangeError, String(error));
    }
  };
  let deep = { deep: 1 };
  for (let i = 0; i < 100000; i++) deep = new Proxy(deep, {});
  runOrOverflow(() => 'deep' in deep);
  runOrOverflow(() => deep.deep);
  runOrOverflow(() => { deep.assigned = 1; });
  runOrOverflow(() => Reflect.ownKeys(deep));
  runOrOverflow(() => delete deep.missing);
  runOrOverflow(() => Object.getPrototypeOf(deep));
  runOrOverflow(() => Object.getOwnPropertyDescriptor(deep, 'deep'));
  runOrOverflow(() => Object.isExtensible(deep));
  runOrOverflow(() => Object.defineProperty(deep, 'defined', { value: 1, configurable: true }));
  runOrOverflow(() => { for (const key in deep) void key; });
  runOrOverflow(() => JSON.stringify(deep));
  runOrOverflow(() => Object.prototype.toString.call(deep));
  runOrOverflow(() => deep instanceof Object);
  runOrOverflow(() => Object.setPrototypeOf(deep, null));
  runOrOverflow(() => Object.isFrozen(Object.preventExtensions(deep)));
}

console.log('ok');
