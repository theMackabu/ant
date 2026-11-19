const assert = require('node:assert');

const getter = function () { return 1; };

{
  const refuses = target => assert.throws(() => Error.captureStackTrace(target), TypeError);
  refuses(Object.freeze({}));
  refuses(Object.seal({}));
  refuses(Object.preventExtensions({}));
  refuses(Object.preventExtensions({ stack: 1 }));
  refuses(Object.freeze(new Error('frozen')));
  refuses(Object.freeze([]));
  refuses(Object.freeze(function () {}));
  refuses(nonConfigurableStack({ value: 1, writable: true }));
  refuses(nonConfigurableStack({ value: 1, writable: false }));
  refuses(nonConfigurableStack({ get: getter }));
  const frozen = Object.freeze({ stack: 1 });
  refuses(frozen);
  assert.strictEqual(frozen.stack, 1);
  assert.deepStrictEqual(Object.getOwnPropertyNames(Object.freeze({})), []);

  const plain = {};
  Error.captureStackTrace(plain);
  assert.strictEqual(typeof plain.stack, 'string');
  const configurable = {};
  Object.defineProperty(configurable, 'stack', { value: 1, writable: false, configurable: true });
  Error.captureStackTrace(configurable);
  assert.strictEqual(typeof configurable.stack, 'string');
  const parent = {};
  Object.defineProperty(parent, 'stack', { value: 1, writable: false });
  const child = Object.create(parent);
  Error.captureStackTrace(child);
  assert.strictEqual(typeof child.stack, 'string');

  function nonConfigurableStack(desc) {
    const target = {};
    Object.defineProperty(target, 'stack', { ...desc, configurable: false });
    return target;
  }
}

{
  assert.throws(() => Error.captureStackTrace(new Proxy({}, {})), TypeError);
}

console.log('ok');
