const assert = require('node:assert');

const HOT = 400;

function base(reshape) {
  class A {}
  Object.defineProperty(A.prototype, 'x', { configurable: true, set(v) {
    Object.defineProperty(A.prototype, 'x', { value: 0, writable: true, enumerable: true, configurable: true });
    reshape(this, v);
  }});
  return A;
}

function define(target, key, value) {
  Object.defineProperty(target, key, { value, writable: true, enumerable: true, configurable: true });
}

const cases = {
  'an extra key after': [(self, v) => { define(self, 'x', v); define(self, 'extra', 'E'); }, i => ({ a: 'A', n: 'N', x: i })],
  'an extra key before': [(self, v) => { define(self, 'extra', 'E'); define(self, 'x', v); }, i => ({ a: 'A', n: 'N', x: i })],
  'a deleted key': [(self, v) => { delete self.n; define(self, 'x', v); }, i => ({ a: 'A', n: 'N', x: i })],
  'a replaced key': [(self, v) => { delete self.a; define(self, 'x', v); define(self, 'a', 'late'); }, i => ({ a: 'A', n: 'N', x: i })],
  'a new prototype': [(self, v) => { Object.setPrototypeOf(self, { marker: true }); define(self, 'x', v); }, i => ({ a: 'A', n: 'N', x: i })],
  'an accessor': [(self, v) => { Object.defineProperty(self, 'n', { get() { return 'G'; }, enumerable: true, configurable: true }); define(self, 'x', v); }, i => ({ a: 'A', n: 'N', x: i })],
};

for (const [name, [reshape, expected]] of Object.entries(cases)) {
  class B extends base(reshape) { constructor(v) { super(); this.a = 'A'; this.n = 'N'; this.x = v; } }
  new B(-1);
  for (let i = 0; i < HOT; i++) {
    const o = new B(i);
    assert.deepStrictEqual({ ...o }, expected(i), `${name}: receiver ${i}`);
    assert.strictEqual(Object.getPrototypeOf(o), B.prototype, `${name}: receiver ${i}`);
    assert.deepStrictEqual(Object.getOwnPropertyDescriptor(o, 'n'), { value: 'N', writable: true, enumerable: true, configurable: true }, `${name}: receiver ${i}`);
    assert.deepStrictEqual(Object.keys(o), ['a', 'n', 'x'], `${name}: receiver ${i}`);
  }
}

function plain(o, v) { o.x = v; return o; }
for (let i = 0; i < 4000; i++) {
  const o = plain({ a: 1, n: 2 }, i);
  assert.deepStrictEqual(Object.keys(o), ['a', 'n', 'x']);
  assert.strictEqual(o.x, i);
}

console.log('ok');
