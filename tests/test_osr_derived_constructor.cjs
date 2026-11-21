const assert = require('node:assert');

class B { constructor() { this.a = new.target.name; } }
class D extends B { constructor() { for (let i = 0; i < 600; i++); super(); this.b = 1; } }
class E extends B { constructor(x) { let t = 0; for (let i = 0; i < 30; i++) t += i; super(); this.t = t + x; } }
class Plain extends B { constructor() { super(); this.c = 2; } }
function NT() {}
NT.prototype = { marker: true };
const ProxiedD = new Proxy(D, {});
const BoundD = D.bind(null);
const ProxiedPlain = new Proxy(Plain, {});
for (let i = 0; i < 40; i++) {
  assert.strictEqual(new ProxiedD().b, 1);
  assert.strictEqual(new ProxiedD().a, 'D');
  assert.strictEqual(new D().b, 1);
  assert.strictEqual(new BoundD().b, 1);
  const viaReflect = Reflect.construct(D, [], NT);
  assert.strictEqual(viaReflect.b, 1);
  assert.strictEqual(viaReflect.marker, true);
  assert.strictEqual(viaReflect.a, 'NT');
}
for (let i = 0; i < 400; i++) {
  assert.strictEqual(new (new Proxy(E, {}))(i).t, 435 + i);
  assert.strictEqual(Reflect.construct(E, [i], NT).t, 435 + i);
  assert.strictEqual(new E(i).t, 435 + i);
  assert.strictEqual(new ProxiedPlain().c, 2);
  assert.strictEqual(Reflect.construct(Plain, [], NT).marker, true);
}

class Fields { f = 3; constructor() { this.a = new.target.name; } }
for (let i = 0; i < 400; i++) new Fields();
class G extends Fields { constructor() { for (let i = 0; i < 600; i++); super(); this.b = 1; } }
const proxiedG = new (new Proxy(G, {}))();
assert.deepStrictEqual([proxiedG.f, proxiedG.a, proxiedG.b], [3, 'G', 1]);
for (let i = 0; i < 40; i++) {
  const reflected = Reflect.construct(G, [], NT);
  assert.deepStrictEqual([reflected.f, reflected.a, reflected.b, reflected.marker], [3, 'NT', 1, true]);
  assert.strictEqual(new G().f, 3);
}

class Empty {}
class H extends Empty { constructor() { for (let i = 0; i < 600; i++); super(); this.b = 1; } }
assert.strictEqual(new (new Proxy(H, {}))().b, 1);
assert.strictEqual(Reflect.construct(H, [], NT).marker, true);

console.log('ok');
