const assert = require('node:assert');

class A {
  get z() { return 4; }
  m() { return 'Am'; }
  static s() { return 'As'; }
}

class StaticArrow extends A {
  static m() { return (() => super.s())(); }
  static field = () => super.s();
}

for (let i = 0; i < 3000; i++) {
  assert.strictEqual(StaticArrow.m.call({}), 'As');
  assert.strictEqual(StaticArrow.m(), 'As');
  assert.strictEqual(StaticArrow.field(), 'As');
}

console.log('ok');
