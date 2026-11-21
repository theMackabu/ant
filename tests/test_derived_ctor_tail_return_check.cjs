const assert = require('node:assert');

const cache = new Map([['1:2', 7], ['3:4', undefined], ['5:6', { tag: 'object' }]]);
class Base {}
class Get extends Base { constructor(a, b) { super(); return cache.get(`${a}:${b}`); } }
class Has extends Base { constructor(a, b) { super(); return cache.has(`${a}:${b}`); } }
class Ternary extends Base {
  constructor(a, b, get) { super(); return get ? cache.get(`${a}:${b}`) : cache.has(`${a}:${b}`); }
}
class Call extends Base { constructor(f) { super(); return f(); } }
class Method extends Base { constructor(o) { super(); return o.m(); } }
class Plain { constructor(a, b) { return cache.get(`${a}:${b}`); } }
function Sloppy(a, b) { return cache.get(`${a}:${b}`); }

const receiver = { m() { return 'text'; } };
const five = () => 5;

for (let i = 0; i < 3000; i++) {
  assert.throws(() => new Get(1, 2), TypeError);
  assert.throws(() => new Has(1, 2), TypeError);
  assert.throws(() => new Ternary(1, 2, i & 1), TypeError);
  assert.throws(() => new Call(five), TypeError);
  assert.throws(() => new Method(receiver), TypeError);
  assert.ok(new Get(3, 4) instanceof Get);
  assert.ok(new Get(8, 9) instanceof Get);
  assert.strictEqual(new Get(5, 6), cache.get('5:6'));
  assert.ok(new Plain(1, 2) instanceof Plain);
  assert.strictEqual(new Plain(5, 6), cache.get('5:6'));
  assert.ok(new Sloppy(1, 2) instanceof Sloppy);
  assert.strictEqual(Sloppy(1, 2), 7);
}

class BaseTail { constructor(x) { this.x = x; return box(x); } }
function box(x) { return { boxed: x }; }
for (let i = 0; i < 3000; i++) assert.strictEqual(new BaseTail(i).boxed, i);

console.log('ok');
