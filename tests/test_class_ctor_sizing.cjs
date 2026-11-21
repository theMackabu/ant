const assert = require('node:assert');

const letters = 'abcdefghijklmnopqrstuvwxyz';
function base(count) {
  return class { constructor() { for (let i = 0; i < count; i++) this[letters[i]] = i; } };
}
function fielded(count) {
  return new Function(`return class { ${[...letters.slice(0, count)].map((c, i) => `${c} = ${i};`).join(' ')} }`)();
}
function own(Parent, extra) {
  return class extends Parent { constructor() { super(); for (let i = 0; i < extra; i++) this['x' + i] = i; } };
}
function expectKeys(count, extra) {
  return [...letters.slice(0, count)].concat(Array.from({ length: extra }, (_, i) => 'x' + i));
}

for (const make of [base, fielded]) {
  for (const count of [0, 1, 3, 4, 5, 8, 9, 13, 20]) {
    const Parent = make(count);
    const Default = class extends Parent {};
    const Twice = class extends Default {};
    const bound = Parent.bind(null);
    bound.prototype = Parent.prototype;
    const Bound = class extends bound {};
    for (const extra of [0, 1, 4, 9]) {
      const Derived = own(Parent, extra);
      const Deep = own(Twice, extra);
      const keys = expectKeys(count, extra);
      for (let i = 0; i < 400; i++) {
        assert.deepStrictEqual(Object.keys(new Derived()), keys);
        assert.deepStrictEqual(Object.keys(new Deep()), keys);
        const made = Reflect.construct(Derived, [], Default);
        assert.deepStrictEqual(Object.keys(made), keys);
        assert.strictEqual(Object.getPrototypeOf(made), Default.prototype);
      }
    }
    const plain = expectKeys(count, 0);
    for (let i = 0; i < 400; i++) {
      const a = new Default(), b = new Twice(), c = new Bound();
      assert.deepStrictEqual(Object.keys(a), plain);
      assert.deepStrictEqual(Object.keys(b), plain);
      assert.deepStrictEqual(Object.keys(c), plain);
      a.late = i; b.late = i;
      assert.strictEqual(a.late + b.late, 2 * i);
      if (count) assert.strictEqual(b[letters[count - 1]], count - 1);
      assert.ok(b instanceof Default && b instanceof Parent);
    }
  }
}

{
  const Small = base(1), Large = base(12);
  const mixin = (Parent) => class extends Parent { constructor() { super(); this.tail = 't'; } };
  const A = mixin(Small), B = mixin(Large), C = mixin(A), D = mixin(class extends B {});
  for (let i = 0; i < 3000; i++) {
    assert.deepStrictEqual(Object.keys(new A()), ['a', 'tail']);
    assert.deepStrictEqual(Object.keys(new B()), [...letters.slice(0, 12), 'tail']);
    assert.deepStrictEqual(Object.keys(new C()), ['a', 'tail']);
    assert.deepStrictEqual(Object.keys(new D()), [...letters.slice(0, 12), 'tail']);
  }
  class Moving extends Small {}
  for (let i = 0; i < 3000; i++) {
    if (i === 1000) Object.setPrototypeOf(Moving, Large);
    if (i === 2000) Object.setPrototypeOf(Moving, Small);
    assert.strictEqual(Object.keys(new Moving()).length, i >= 1000 && i < 2000 ? 12 : 1);
  }
}

console.log('ok');
