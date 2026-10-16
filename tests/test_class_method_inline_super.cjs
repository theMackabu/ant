const assert = require('node:assert');

class A {
  m() { return 'A'; }
  static s() { return 'sA'; }
  get g() { return 'gA'; }
}

class B extends A {
  m() { return 'B>' + super.m(); }
  static s() { return 'sB>' + super.s(); }
  get g() { return 'gB>' + super.g; }
  arrow() { return (() => super.m())(); }
  nested() { return this.m() + '|' + B.s(); }
}

const b = new B();
for (let i = 0; i < 2000; i++) {
  assert.strictEqual(b.m(), 'B>A');
  assert.strictEqual(B.s(), 'sB>sA');
  assert.strictEqual(b.g, 'gB>gA');
  assert.strictEqual(b.arrow(), 'A');
  assert.strictEqual(b.nested(), 'B>A|sB>sA');
}
assert.throws(() => B(), TypeError);

function* gen() {
  try { yield 1; } catch (e) { yield 'caught ' + e; }
}
const it = gen();
it.next();
assert.deepStrictEqual(it.throw('x'), { value: 'caught x', done: false });

function nestedCatch() {
  try { try { throw new Error('inner'); } finally { } } catch (e) { return e.message; }
}
for (let i = 0; i < 2000; i++) assert.strictEqual(nestedCatch(), 'inner');

console.log('class method inline super: ok');
