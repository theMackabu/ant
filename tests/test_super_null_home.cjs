const assert = require('node:assert');

const nullHome = { m() { return super.x; }, k(key) { return super[key]; }, c() { return super.f(); } };
Object.setPrototypeOf(nullHome, null);

for (let i = 0; i < 3000; i++) {
  assert.throws(() => nullHome.m(), TypeError);
  assert.throws(() => nullHome.k('x'), TypeError);
  assert.throws(() => nullHome.c(), TypeError);
}

console.log('ok');
