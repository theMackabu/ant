const assert = require('node:assert');
const util = require('node:util');

const flat = (value, options) => util.inspect(value, options).replace(/\s+/g, ' ');

let trapped = 0;
const counting = new Proxy({ a: 1 }, {
  get(target, key, receiver) { trapped++; return Reflect.get(target, key, receiver); },
  ownKeys(target) { trapped++; return Reflect.ownKeys(target); },
  getOwnPropertyDescriptor(target, key) { trapped++; return Reflect.getOwnPropertyDescriptor(target, key); },
  getPrototypeOf(target) { trapped++; return Reflect.getPrototypeOf(target); },
});

assert.strictEqual(util.inspect(counting), 'Proxy({ a: 1 })');
assert.strictEqual(trapped, 0);
assert.strictEqual(util.types.isProxy(counting), true);

assert.strictEqual(util.inspect(new Proxy(function named() {}, {})), 'Proxy([Function: named])');
assert.strictEqual(util.inspect(new Proxy(Math.max, {})), 'Proxy([Function: max])');
assert.strictEqual(util.inspect(new Proxy([1, 2], {})), 'Proxy([ 1, 2 ])');
assert.strictEqual(util.inspect(new Proxy(new Proxy({ deep: true }, {}), {})), 'Proxy(Proxy({ deep: true }))');
assert.strictEqual(util.inspect(new Proxy({ a: 1 }, {}), { showProxy: true }), 'Proxy [ { a: 1 }, {} ]');

const nested = new Proxy({ a: { b: { c: 1 } } }, {});
assert.strictEqual(flat(nested, { depth: 0 }), 'Proxy({ a: [Object] })');
assert.strictEqual(flat(nested, { depth: 0, showProxy: true }), 'Proxy [ [Object], {} ]');
assert.strictEqual(flat({ p: nested }, { depth: 0, showProxy: true }), '{ p: Proxy [Array] }');
assert.strictEqual(flat(nested, { depth: 1, showProxy: true }), 'Proxy [ { a: [Object] }, {} ]');

const circular = {};
circular.self = new Proxy(circular, {});
assert.strictEqual(flat(circular), '<ref *1> { self: [Circular *1] }');
assert.strictEqual(flat(circular, { showProxy: true }), '<ref *1> { self: Proxy [ [Circular *1], {} ] }');

const { proxy, revoke } = Proxy.revocable({}, {});
revoke();
assert.strictEqual(util.inspect(proxy), '<Revoked Proxy>');

console.log('ok');
