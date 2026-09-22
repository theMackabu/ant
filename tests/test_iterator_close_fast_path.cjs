const assert = require('node:assert');

const factories = [
  () => [1, 2].values(),
  () => 'ab'[Symbol.iterator](),
  () => new Uint8Array([1, 2]).values(),
  () => new Map([[1, 2]]).entries(),
  () => new Set([1, 2]).values(),
];
if (typeof Headers === 'function') factories.push(() => new Headers({ a: 'b' }).entries());

const some = Iterator.prototype.some;
function restore(object, key, descriptor) {
  if (descriptor) Object.defineProperty(object, key, descriptor);
  else delete object[key];
}

for (const factory of factories) {
  assert.strictEqual(some.call(factory(), () => true), true);

  const own = factory();
  let ownCalls = 0;
  assert.strictEqual(some.call(own, () => {
    own.return = function () { assert.strictEqual(this, own); ownCalls++; return {}; };
    return true;
  }), true);
  assert.strictEqual(ownCalls, 1, 'return added by callback');

  const removed = factory();
  let removedCalls = 0;
  removed.return = () => { removedCalls++; return {}; };
  assert.strictEqual(some.call(removed, () => {
    delete removed.return;
    return true;
  }), true);
  assert.strictEqual(removedCalls, 0, 'return deleted by callback');

  const inherited = factory();
  const proto = Object.getPrototypeOf(inherited);
  const original = Object.getOwnPropertyDescriptor(proto, 'return');
  let reads = 0;
  let calls = 0;
  try {
    assert.strictEqual(some.call(inherited, () => {
      Object.defineProperty(proto, 'return', {
        configurable: true,
        get() {
          assert.strictEqual(this, inherited);
          reads++;
          return function () { assert.strictEqual(this, inherited); calls++; return {}; };
        },
      });
      return true;
    }), true);
    assert.strictEqual(reads, 1, 'inherited return getter called once');
    assert.strictEqual(calls, 1);

    const absent = factory();
    Object.defineProperty(proto, 'return', {
      configurable: true,
      get() { reads++; return undefined; },
    });
    reads = 0;
    assert.strictEqual(some.call(absent, () => true), true);
    assert.strictEqual(reads, 1, 'undefined getter result is still an observed access');

    const shadowed = factory();
    Object.defineProperty(shadowed, 'return', { value: undefined });
    reads = 0;
    assert.strictEqual(some.call(shadowed, () => true), true);
    assert.strictEqual(reads, 0, 'own undefined shadows inherited return getter');
  } finally {
    restore(proto, 'return', original);
  }

  const rewired = factory();
  let rewiredCalls = 0;
  assert.strictEqual(some.call(rewired, () => {
    Object.setPrototypeOf(rewired, {
      return() { assert.strictEqual(this, rewired); rewiredCalls++; return {}; },
    });
    return true;
  }), true);
  assert.strictEqual(rewiredCalls, 1, 'prototype replaced during iteration');

  const proxied = factory();
  let gets = 0;
  assert.strictEqual(some.call(proxied, () => {
    Object.setPrototypeOf(proxied, new Proxy({}, {
      get(target, key, receiver) {
        if (key === 'return') {
          assert.strictEqual(receiver, proxied);
          gets++;
          return () => ({});
        }
        return Reflect.get(target, key, receiver);
      },
    }));
    return true;
  }), true);
  assert.strictEqual(gets, 1, 'proxy prototype get trap');

  for (const invalid of [123, () => 123]) {
    const iterator = factory();
    iterator.return = invalid;
    assert.throws(() => some.call(iterator, () => true), TypeError);
  }

  const throwing = factory();
  const originalError = {};
  let closes = 0;
  throwing.return = () => { closes++; throw new Error('cleanup'); };
  assert.throws(() => some.call(throwing, () => { throw originalError; }), e => e === originalError);
  assert.strictEqual(closes, 1, 'closing preserves the original throw');
}

const custom = {
  [Symbol.iterator]() { return this; },
};
let nextReads = 0;
Object.setPrototypeOf(custom, {
  get next() {
    assert.strictEqual(this, custom);
    nextReads++;
    return () => ({ done: true });
  },
});
assert.strictEqual(some.call(custom, () => true), false);
assert.strictEqual(nextReads, 1, 'next getter remains observable');

const baseReturn = Object.getOwnPropertyDescriptor(Object.prototype, 'return');
let baseCalls = 0;
try {
  const iterator = [1].values();
  assert.strictEqual(some.call(iterator, () => {
    Object.prototype.return = () => { baseCalls++; return {}; };
    return true;
  }), true);
  assert.strictEqual(baseCalls, 1, 'return added to Object.prototype');
} finally {
  restore(Object.prototype, 'return', baseReturn);
}

console.log('PASS iterator method lookup, callback mutations, getters, proxies, and close errors');
