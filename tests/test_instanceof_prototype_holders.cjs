const assert = require('node:assert');

class A {}
class B extends A {}
function* gen() {}

function check(values, ctor) {
  let hits = 0;
  for (let i = 0; i < 20000; i++) if (values[i % values.length] instanceof ctor) hits++;
  return hits;
}

const kinds = [
  new A(), new B(), {}, [], Object.create(Array.prototype), Object.create(null),
  new Map(), new Date(), /x/, Promise.resolve(), gen(), function () {}, class extends A {},
  Math.max, 1, 'a', true, null, undefined, Symbol('s'), 10n,
];

const expected = new Map([[A, 2], [Object, 13], [Array, 2], [Function, 3], [Promise, 1]]);
for (const [ctor, perRound] of expected) {
  const rounds = Math.floor(20000 / kinds.length);
  const rest = 20000 % kinds.length;
  let tail = 0;
  for (let i = 0; i < rest; i++) if (kinds[i] instanceof ctor) tail++;
  assert.strictEqual(check(kinds, ctor), rounds * perRound + tail, ctor.name);
}

const arraySub = class extends Array {};
assert.strictEqual(check([new arraySub(), [], {}], arraySub), 6667);
assert.strictEqual(check([new arraySub(), [], {}], Array), 13334);

const chained = Object.create(Object.create(Array.prototype));
assert.strictEqual(check([chained, new A()], Array), 10000);

function C() {}
const c1 = new C();
const swapped = new C();
assert.strictEqual(check([c1, swapped, []], C), 13334);
Object.setPrototypeOf(swapped, A.prototype);
assert.strictEqual(check([c1, swapped, []], C), 6667);
assert.strictEqual(check([c1, swapped, []], A), 6667);

const middle = Object.create(A.prototype);
const leaf = Object.create(middle);
assert.strictEqual(check([leaf, []], A), 10000);
Object.setPrototypeOf(middle, Array.prototype);
assert.strictEqual(check([leaf, []], A), 0);
assert.strictEqual(check([leaf, []], Array), 20000);

function D() {}
const d = new D();
assert.strictEqual(check([d, {}], D), 10000);
D.prototype = {};
assert.strictEqual(check([d, {}], D), 0);

function E() {}
const e = new E();
assert.strictEqual(check([e, []], E), 10000);
Object.defineProperty(E, Symbol.hasInstance, { value: (v) => Array.isArray(v) });
assert.strictEqual(check([e, []], E), 10000);
assert.strictEqual([] instanceof E, true);
assert.strictEqual(e instanceof E, false);

let traps = 0;
const proxied = new Proxy({}, { getPrototypeOf() { traps++; return A.prototype; } });
const behindProxy = Object.create(proxied);
assert.strictEqual(check([new A(), behindProxy, proxied], A), 20000);
assert.ok(traps > 0);

console.log('ok');
