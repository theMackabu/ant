const assert = require('node:assert');

class A {
  get z() { return 4; }
  m() { return 'Am'; }
  static s() { return 'As'; }
}

class FieldArrows extends A {
  arrow = () => super.z;
  nested = () => () => super.m();
  direct = super.z;
}

class BaseFields {
  arrow = () => typeof super.toString;
  direct = typeof super.hasOwnProperty;
}

class BaseCtor {
  constructor() { this.v = typeof super.toString; }
}

class EvalSuper extends A {
  read() { return eval('super.z'); }
  write() { eval('super.q = 1'); return this.q; }
}

for (let i = 0; i < 3000; i++) {
  const f = new FieldArrows();
  assert.strictEqual(f.arrow(), 4);
  assert.strictEqual(f.nested()(), 'Am');
  assert.strictEqual(f.direct, 4);

  const b = new BaseFields();
  assert.strictEqual(b.arrow(), 'function');
  assert.strictEqual(b.direct, 'function');
  assert.strictEqual(new BaseCtor().v, 'function');
}

const e = new EvalSuper();
assert.strictEqual(e.read(), 4);
assert.strictEqual(e.write(), 1);

class EvalBase { x() { return 'base x'; } }
class EvalInFieldArrow extends EvalBase { read = () => eval(String('super.x()')); }
class EvalInField { isObject = eval(String('super.constructor === Object')); }
assert.strictEqual(new EvalInFieldArrow().read(), 'base x');
assert.strictEqual(new EvalInField().isObject, true);

console.log('ok');
