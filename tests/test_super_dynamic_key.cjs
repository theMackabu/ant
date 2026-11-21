const assert = require('node:assert');

const sym = Symbol('s');
class A {
  a() { return 'a:' + this.tag; }
  b() { return 'b:' + this.tag; }
  get who() { return 'who:' + this.tag; }
  [sym]() { return 'sym:' + this.tag; }
  7() { return 'seven'; }
}
A.prototype.data = 'proto-data';

class D extends A {
  constructor(tag) { super(); this.tag = tag; this.data = 'own-data'; this.a = () => 'own-a'; }
  read(key) { return super[key]; }
  call(key) { return super[key](); }
}

const d = new D('d');
const keys = ['a', 'b'];
for (let i = 0; i < 100000; i++) {
  assert.strictEqual(d.call(keys[i & 1]), keys[i & 1] + ':d');
  assert.strictEqual(d.read('data'), 'proto-data');
}
assert.strictEqual(d.read('who'), 'who:d');
assert.strictEqual(d.read('missing'), undefined);
assert.strictEqual(d.read('tag'), undefined);
assert.strictEqual(d.call(sym), 'sym:d');
assert.strictEqual(d.call(7), 'seven');
assert.strictEqual(d.read('toString'), Object.prototype.toString);
assert.strictEqual(d.read({ toString() { return 'data'; } }), 'proto-data');

A.prototype.late = 1;
assert.strictEqual(d.read('late'), 1);
A.prototype.late = 2;
assert.strictEqual(d.read('late'), 2);
delete A.prototype.late;
assert.strictEqual(d.read('late'), undefined);
Object.defineProperty(A.prototype, 'late', { get() { return 'getter:' + this.tag; }, configurable: true });
assert.strictEqual(d.read('late'), 'getter:d');

const B = { late: 'retargeted', a() { return 'B.a'; } };
Object.setPrototypeOf(D.prototype, B);
assert.strictEqual(d.read('late'), 'retargeted');
assert.strictEqual(d.call('a'), 'B.a');
assert.strictEqual(d.read('data'), undefined);

console.log('ok');
