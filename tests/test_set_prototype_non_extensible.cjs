const assert = require('node:assert');

class Parent {}
class Other { constructor() { this.other = true; } }
class FrozenClass extends Parent {}
Object.freeze(FrozenClass);
assert.throws(() => Object.setPrototypeOf(FrozenClass, Other), TypeError);
assert.strictEqual(Object.setPrototypeOf(FrozenClass, Parent), FrozenClass);
assert.strictEqual(Object.getPrototypeOf(FrozenClass), Parent);
assert.strictEqual(new FrozenClass().other, undefined);

const sealed = Object.preventExtensions({});
assert.strictEqual(Reflect.setPrototypeOf(sealed, {}), false);
assert.strictEqual(Reflect.setPrototypeOf(sealed, Object.prototype), true);
assert.throws(() => { sealed.__proto__ = {}; }, TypeError);
assert.strictEqual(Reflect.setPrototypeOf(new Proxy(sealed, {}), {}), false);
assert.strictEqual(Object.getPrototypeOf(Object.setPrototypeOf([1, 2, 3], null)), null);

const target = { target: true };
const viaAccessor = new Proxy({}, {});
viaAccessor.__proto__ = target;
assert.strictEqual(Object.getPrototypeOf(viaAccessor), target);

let trapCalls = 0;
const trapped = new Proxy({}, { setPrototypeOf(object, proto) { trapCalls++; return Reflect.setPrototypeOf(object, proto); } });
trapped.__proto__ = target;
assert.strictEqual(trapCalls, 1);
assert.throws(() => { new Proxy(Object.preventExtensions({}), {}).__proto__ = target; }, TypeError);

const inner = new Proxy({}, { setPrototypeOf(object, proto) { trapCalls++; return Reflect.setPrototypeOf(object, proto); } });
const outer = new Proxy(inner, {});
assert.strictEqual(Reflect.setPrototypeOf(outer, target), true);
assert.strictEqual(trapCalls, 2);
assert.strictEqual(Object.getPrototypeOf(outer), target);
assert.strictEqual(Reflect.setPrototypeOf(new Proxy(new Proxy(Object.preventExtensions({}), {}), {}), target), false);

const selfReferential = {};
assert.throws(() => Object.setPrototypeOf(selfReferential, selfReferential), TypeError);
assert.strictEqual(Reflect.setPrototypeOf(selfReferential, selfReferential), false);
const child = Object.create(selfReferential);
assert.throws(() => { selfReferential.__proto__ = child; }, TypeError);

for (const depth of [1, 7, 8, 9, 40]) {
  const root = {};
  let leaf = root;
  for (let i = 0; i < depth; i++) leaf = Object.create(leaf);
  assert.throws(() => Object.setPrototypeOf(root, leaf), TypeError);
  assert.strictEqual(Reflect.setPrototypeOf(root, leaf), false);
  assert.throws(() => { root.__proto__ = leaf; }, TypeError);
  assert.strictEqual(Object.getPrototypeOf(root), Object.prototype);

  const other = {};
  assert.strictEqual(Object.setPrototypeOf(other, leaf), other);
  assert.strictEqual(Object.getPrototypeOf(other), leaf);
}

const viaFunction = function () {};
const belowFunction = Object.create(Object.create(viaFunction));
assert.throws(() => Object.setPrototypeOf(viaFunction, belowFunction), TypeError);
assert.strictEqual(Object.getPrototypeOf(viaFunction), Function.prototype);

const behindProxy = {};
const proxied = new Proxy(Object.create(behindProxy), {});
assert.strictEqual(Reflect.setPrototypeOf(behindProxy, proxied), true);
assert.strictEqual(Object.getPrototypeOf(behindProxy), proxied);

console.log('ok');
