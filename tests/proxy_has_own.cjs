const assert = require('node:assert');

function ClientCounter() {}
const target = {};
Object.defineProperty(target, 'ClientCounter', {
  value: ClientCounter,
  writable: false,
  configurable: false,
  enumerable: false,
});

const proxy = new Proxy(target, {
  get(_target, key) {
    if (key === 'ClientCounter') return ClientCounter;
    return undefined;
  },
  getOwnPropertyDescriptor(_target, key) {
    if (key === 'ClientCounter') {
      return {
        value: ClientCounter,
        writable: false,
        configurable: false,
        enumerable: false,
      };
    }
    return undefined;
  },
});

assert.strictEqual(typeof proxy.ClientCounter, 'function');
assert.strictEqual(Object.prototype.hasOwnProperty.call(proxy, 'ClientCounter'), true);
assert.strictEqual(Object.hasOwn(proxy, 'ClientCounter'), true);
assert.strictEqual(Object.getOwnPropertyDescriptor(proxy, 'ClientCounter').value, ClientCounter);

const phantom = new Proxy({}, {
  getOwnPropertyDescriptor() {
    return { value: 1, writable: false, configurable: false, enumerable: false };
  },
});
assert.throws(() => Object.hasOwn(phantom, 'missing'), TypeError);

console.log('proxy hasOwn ok');
