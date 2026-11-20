const assert = require('node:assert');
const util = require('node:util');

const binding = { get(target, key) { const value = Reflect.get(target, key); return typeof value === 'function' ? value.bind(target) : value; } };

const objectLike = {
  promise: () => Promise.resolve(1),
  asyncResult: () => (async () => 1)(),
  generator: () => (function* () { yield 1; })(),
  builtinFunction: () => Math.max,
  array: () => [],
  arguments: () => (function () { return arguments; })(),
  map: () => new Map(),
  typedArray: () => new Uint8Array(2),
  boundFunction: () => (function () {}).bind(null),
  classConstructor: () => class {},
};

for (const [name, make] of Object.entries(objectLike)) {
  assert.doesNotThrow(() => new Proxy(make(), {}), name);
  assert.doesNotThrow(() => new Proxy({}, make()), `${name} as handler`);
  assert.doesNotThrow(() => Proxy.revocable(make(), {}), `${name} revocable`);
}

for (const primitive of [1, 'text', true, null, undefined, 1n, Symbol('s')]) {
  assert.throws(() => new Proxy(primitive, {}), TypeError);
  assert.throws(() => new Proxy({}, primitive), TypeError);
}

async function promises() {
  const target = Promise.resolve(42);
  assert.strictEqual(typeof new Proxy(target, {}), 'object');
  assert.ok(new Proxy(target, {}) instanceof Promise);
  assert.strictEqual(Object.prototype.toString.call(new Proxy(target, {})), '[object Promise]');
  assert.throws(() => new Proxy(target, {}).then(() => {}), TypeError);

  assert.strictEqual(await new Proxy(target, binding), 42);
  assert.strictEqual(await new Proxy(target, binding).then((value) => value + 1), 43);
  assert.deepStrictEqual(await Promise.all([new Proxy(target, binding), new Proxy(Promise.resolve(2), binding)]), [42, 2]);
  await assert.rejects(new Proxy(Promise.reject(new RangeError('boom')), binding), RangeError);
  assert.strictEqual(await new Proxy((async () => 'async')(), binding), 'async');

  const expando = new Proxy(target, {});
  expando.extra = 5;
  assert.strictEqual(target.extra, 5);
}

function callables() {
  assert.strictEqual(new Proxy(Math.max, {})(1, 5, 3), 5);
  assert.strictEqual(typeof new Proxy(Math.max, {}), 'function');
  assert.strictEqual(new Proxy(Math.max, {}).name, 'max');
  assert.strictEqual(new Proxy(Math.max, { apply(target, self, args) { return target(...args) * 2; } })(1, 5), 10);
  assert.strictEqual(new (new Proxy(Map, {}))([[1, 2]]).get(1), 2);

  const generator = new Proxy((function* () { yield 1; yield 2; })(), binding);
  assert.strictEqual(generator.next().value, 1);
  assert.deepStrictEqual([...generator], [2]);

  const handler = [];
  handler.get = () => 7;
  assert.strictEqual(new Proxy({}, handler).anything, 7);
}

function trapArguments() {
  for (const builtin of [Math.max, parseInt, Array.prototype.push]) {
    const seen = new Proxy(builtin, { get: (target) => target }).anything;
    assert.strictEqual(seen, builtin);
    assert.strictEqual(new WeakMap([[builtin, 1]]).get(seen), 1);
  }

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

callables();
trapArguments();
promises().then(() => console.log('ok'));
