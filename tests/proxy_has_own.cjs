const target = {};
function makeClientCounter() {
  return function ClientCounter() {};
}

const proxy = new Proxy(target, {
  get(_target, key) {
    if (key === 'ClientCounter') return makeClientCounter();
    return undefined;
  },
  getOwnPropertyDescriptor(_target, key) {
    if (key === 'ClientCounter') {
      return {
        value: makeClientCounter(),
        writable: false,
        configurable: false,
        enumerable: false,
      };
    }
    return undefined;
  },
});

console.log(`get:${typeof proxy.ClientCounter}`);
console.log(`hasOwn:${Object.prototype.hasOwnProperty.call(proxy, 'ClientCounter')}`);
console.log(`objHasOwn:${Object.hasOwn(proxy, 'ClientCounter')}`);
const desc = Object.getOwnPropertyDescriptor(proxy, 'ClientCounter');
console.log(`desc.value:${typeof desc?.value}`);
