const assert = require('node:assert');

const s = Symbol('s');
function inheritedSetterProxy() {
  const seen = [];
  const proto = { set a(v) { seen.push(this === p); }, set [s](v) { seen.push(this === p); } };
  const p = new Proxy(Object.create(proto), {});
  return { p, seen };
}

{
  const { p, seen } = inheritedSetterProxy();
  p.a = 1;
  p[s] = 2;
  Reflect.set(p, 'a', 3);
  assert.deepStrictEqual(seen, [true, true, true], 'setter through a trap-less proxy receives the proxy');
}

{
  const seen = [];
  const target = { set own(v) { seen.push(this === p); } };
  const p = new Proxy(target, {});
  p.own = 1;
  assert.deepStrictEqual(seen, [true], 'own setter on the target receives the proxy');
}

{
  const seen = [];
  const r = {};
  Reflect.set(new Proxy(Object.create({ set a(v) { seen.push(this === r); } }), {}), 'a', 1, r);
  assert.deepStrictEqual(seen, [true], 'Reflect.set with another receiver calls the inherited setter');

  const target = { a: 1 };
  const other = {};
  assert.strictEqual(Reflect.set(new Proxy(target, {}), 'a', 9, other), true);
  assert.deepStrictEqual([target.a, other.a], [1, 9], 'data writes go to the receiver');
  assert.strictEqual(Reflect.set(new Proxy({}, {}), 'a', 9, Object.freeze({})), false);
}

{
  const seen = [];
  const obj = Object.create(new Proxy({ set c(v) { seen.push(this === obj); } }, {}));
  obj.c = 1;
  assert.deepStrictEqual(seen, [true], 'setter behind a proxy prototype receives the original object');

  const base = { d: 1 };
  const child = Object.create(new Proxy(base, {}));
  child.d = 5;
  assert.deepStrictEqual([Object.hasOwn(child, 'd'), child.d, base.d], [true, 5, 1]);
}

{
  const f = function () {};
  const seen = [];
  Object.setPrototypeOf(f, { set q(v) { seen.push(this === f); } });
  f.q = 1;
  Object.defineProperty(Function.prototype, 'zz', { set(v) { seen.push(this === Math.max); }, configurable: true });
  Math.max.zz = 1;
  delete Function.prototype.zz;
  assert.deepStrictEqual(seen, [true, true], 'function and builtin receivers are passed to setters');
}

assert.throws(() => { 'use strict'; Object.create(new Proxy(Object.freeze({ d: 1 }), {})).d = 2; },
  { message: "Cannot assign to read only property 'd' of object '[object Object]'" });
assert.throws(() => { 'use strict'; Object.create(new Proxy({ get g() { return 1; } }, {})).g = 2; },
  { message: 'Cannot set property g of #<Object> which has only a getter' });
assert.throws(() => { 'use strict'; Object.preventExtensions(Object.create(new Proxy({}, {}))).n = 2; },
  { message: 'Cannot add property n, object is not extensible' });
class H { get g() { return 1; } }
assert.throws(() => { 'use strict'; new (class R extends H {})().g = 2; },
  { message: 'Cannot set property g of #<H> which has only a getter' });
assert.strictEqual((function () { 'use strict'; return Reflect.set(Object.create(new Proxy(Object.freeze({ d: 1 }), {})), 'd', 5); })(), false);

console.log('proxy set receiver ok');
