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

console.log('ok');
