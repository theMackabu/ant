const assert = require('node:assert');
class A { constructor() { this.a = 1; this.v = 'A'; } m() { return 'am'; } }
const SRC = 'v + ":" + typeof this + ":" + (this && this.v)';
const SRC2 = 'v = v + 1';
const SRC3 = 'this.v + v';
function plain(v) { return eval(SRC); }
function plainNested(v) { return (() => eval(SRC))(); }
class B extends A { constructor(v) { super(); this.r = eval(SRC); } }
class C extends A { constructor(v, early) { if (early) eval(SRC); super(); this.r = eval(SRC); this.s = (() => eval(SRC))(); } }
class D extends A { constructor(v) { const f = () => eval(SRC); super(); this.r = f(); } }
class E extends A { x = 5; constructor(v) { eval('super()'); this.r = eval(SRC); } }
class F extends A { constructor(v) { super(); if (v) { eval(SRC2); eval(SRC2); } this.w = v; this.t = eval(SRC3); } }
function inc(v) { eval(SRC2); eval(SRC2); return v; }
function shadow(v) { { let v = 'inner'; return eval(SRC); } }
function declares(n) { eval('var v = "decl' + '"'); return eval(SRC); }
for (let i = 0; i < 200; i++) {
  assert.strictEqual(plain.call({ v: 'p' }, i), i + ':object:p');
  assert.strictEqual(new B(i).r, i + ':object:A');
  assert.strictEqual(plainNested.call({ v: 'n' }, i), i + ':object:n');
  assert.strictEqual(new C(i, false).r, i + ':object:A');
  assert.strictEqual(new C(i, false).s, i + ':object:A');
  assert.throws(() => new C(i, true), ReferenceError);
  assert.strictEqual(new D(i).r, i + ':object:A');
  assert.strictEqual(new E(i).r, i + ':object:A');
  assert.strictEqual(plain.call(undefined, i), i + ':object:undefined');
  assert.strictEqual(inc(i), i + 2);
  const f = new F(i + 1); assert.strictEqual(f.w, i + 3); assert.strictEqual(f.t, 'A' + (i + 3));
  assert.strictEqual(shadow.call({ v: 's' }, i), 'inner:object:s');
  assert.strictEqual(declares.call({ v: 'd' }, i), 'decl:object:d');
  assert.strictEqual((0, eval)('typeof v'), 'undefined');
}
globalThis.v = 'G';
for (let i = 0; i < 50; i++) {
  assert.strictEqual((0, eval)(SRC), 'G:object:G');
  assert.strictEqual(new B(i).r, i + ':object:A');
  assert.strictEqual(plain.call({ v: 'p' }, i), i + ':object:p');
}
console.log('ok');
